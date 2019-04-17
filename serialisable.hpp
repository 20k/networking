#ifndef SERIALISABLE_HPP_INCLUDED
#define SERIALISABLE_HPP_INCLUDED

#include <nlohmann/json.hpp>
#include <type_traits>
#include <vec/vec.hpp>
#include <map>
#include <iostream>

///its going to be this kind of file
///if this makes you sad its not getting any better from here
template<typename T>
struct class_extractor;

template<typename C, typename R, typename... Args>
struct class_extractor<R(C::*)(Args...)>
{
    using class_t = C;
};

template<typename T>
bool serialisable_is_equal(T* one, T* two);

struct serialise_context;

#define SERIALISE_SIGNATURE() void _internal_helper(){}\
using self_t = typename class_extractor<decltype(&_internal_helper)>::class_t;\
void serialise(serialise_context& ctx, nlohmann::json& data, self_t* other = nullptr)

#define DO_SERIALISE(x) do{ \
                            if(ctx.serialisation) \
                            { \
                                if(other) \
                                { \
                                    if(!serialisable_is_equal(&this->x, &other->x)) \
                                        do_serialise(ctx, data, x, std::string(#x), &other->x); \
                                }  \
                                else \
                                { \
                                    decltype(x)* fptr = nullptr;\
                                    do_serialise(ctx, data, x, std::string(#x), fptr); \
                                } \
                            } \
                            if(ctx.exec_rpcs) \
                            { \
                                do_recurse(ctx, x); \
                            } \
                            if(ctx.check_eq) \
                            { \
                                if(!ctx.is_eq_so_far) \
                                    return; \
                                assert(other != nullptr); \
                                if(!serialisable_is_eq_impl(ctx, this->x, other->x)) \
                                { \
                                    ctx.is_eq_so_far = false; \
                                    return; \
                                } \
                            } \
                        }while(0)

#define DO_RPC(x) do{ \
                        if(ctx.exec_rpcs) \
                        { \
                            if(auto it = ctx.inf.built.find(_pid); it != ctx.inf.built.end()) \
                            { \
                                for(rpc_data& dat : it->second) \
                                { \
                                    if(dat.func == std::string(#x)) \
                                    { \
                                        exec_rpc(x, *this, dat.arg); \
                                    } \
                                } \
                            } \
                        } \
                  }while(0)

#define FRIENDLY_RPC_NAME(function_name) template<typename... T> void function_name##_rpc(T&&... t) \
{ \
    rpc(#function_name , *this, std::forward<T>(t)...);\
}

std::string string_hash(const std::string& in);

bool nlohmann_has_name(const nlohmann::json& data, const std::string& name);
bool nlohmann_has_name(const nlohmann::json& data, int name);

nlohmann::json& nlohmann_index(nlohmann::json& data, const std::string& name);
nlohmann::json& nlohmann_index(nlohmann::json& data, int name);

struct serialisable
{
    //virtual void serialise(serialise_context& ctx, nlohmann::json& data){}

    static size_t time_ms();

    virtual ~serialisable();
};

struct rpc_data : serialisable
{
    size_t id = -1;
    std::string func;
    nlohmann::json arg;

    SERIALISE_SIGNATURE();
};

struct global_serialise_info : serialisable
{
    std::vector<rpc_data> all_rpcs;

    std::map<size_t, std::vector<rpc_data>> built;

    SERIALISE_SIGNATURE();
};

struct serialise_context
{
    //nlohmann::json data;
    nlohmann::json faux; ///fake nlohmann

    bool encode = false;
    bool recurse = false;
    bool serialisation = false;

    global_serialise_info inf;
    bool exec_rpcs = false;


    bool check_eq = false;
    ///used for comparing serialisable objects
    bool is_eq_so_far = true;
};

template<typename T>
nlohmann::json serialise(T& in);

inline
void args_to_nlohmann_1(nlohmann::json& in, serialise_context& ctx, int& idx)
{

}

template<typename T, typename... U>
inline
void args_to_nlohmann_1(nlohmann::json& in, serialise_context& ctx, int& idx, T one, U&... two)
{
    in[idx] = serialise(one);

    idx++;

    args_to_nlohmann_1(in, ctx, idx, two...);
}

template<typename... T>
inline
nlohmann::json args_to_nlohmann(T&... args)
{
    nlohmann::json ret;
    serialise_context ctx;
    ctx.encode = true;
    ctx.serialisation = true;

    int idx = 0;
    args_to_nlohmann_1(ret, ctx, idx, args...);

    return ret;
}

global_serialise_info& get_global_serialise_info();
size_t get_next_persistent_id();

struct owned
{
    size_t _pid = get_next_persistent_id();
};

template<typename T>
inline
std::shared_ptr<T>& get_tls_ptr(size_t id)
{
    thread_local static std::map<size_t, std::shared_ptr<T>> tls_pointer_map;

    std::shared_ptr<T>& ptr = tls_pointer_map[id];

    if(!ptr)
    {
        ptr = std::make_shared<T>();
    }

    return ptr;
}

template<int N, typename T, typename I>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, vec<N, T>& in, const I& name, vec<N, T>* other)
{
    if(ctx.encode)
    {
        if(serialisable_is_equal(&in, other))
            return;

        for(int i=0; i < N; i++)
        {
            data[name][i] = in.v[i];
        }
    }
    else
    {
        if(!nlohmann_has_name(data, name))
            return;

        for(int i=0; i < N; i++)
        {
            in.v[i] = data[name][i];
        }
    }
}

template<typename T, typename I>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, T& in, const I& name, T* other)
{
    constexpr bool is_serialisable = std::is_base_of_v<serialisable, T>;
    constexpr bool is_owned = std::is_base_of_v<owned, T>;

    if constexpr(is_serialisable)
    {
        if(!ctx.encode)
        {
            if(!nlohmann_has_name(data, name))
                return;
        }

        if(ctx.encode)
        {
            //if(std::is_base_of_v<owned, T> && serialisable_is_equal(&in, other))
            //    return;
        }

        in.serialise(ctx, data[name], other);

        if constexpr(is_owned)
        {
            if(ctx.encode)
                data[name]["_pid"] = in._pid;
            else
                in._pid = data[name]["_pid"];
        }
    }

    if constexpr(!is_serialisable)
    {
        if(ctx.encode)
        {
            if(serialisable_is_equal(&in, other))
                return;

            data[name] = in;
        }
        else
        {
            if(!nlohmann_has_name(data, name))
                return;

            in = data[name];

            if constexpr(is_owned)
            {
                in._pid = data[name]["_pid"];
            }
        }
    }
}

template<typename T, typename I>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, T*& in, const I& name, T** other)
{
    if(in == nullptr)
    {
        in = new T();
    }

    T* fptr = nullptr;

    if(other)
        do_serialise(ctx, data, *in, name, *other);
    else
        do_serialise(ctx, data, *in, name, fptr);
}

/*template <typename T, typename Compare>
std::vector<std::size_t> sort_permutation(
    const std::vector<T>& vec,
    Compare& compare)
{
    std::vector<std::size_t> p(vec.size());
    std::iota(p.begin(), p.end(), 0);
    std::sort(p.begin(), p.end(),
        [&](std::size_t i, std::size_t j){ return compare(vec[i], vec[j]); });
    return p;
}

template <typename T>
void apply_permutation_in_place(
    std::vector<T>& vec,
    const std::vector<std::size_t>& p)
{
    std::vector<bool> done(vec.size());
    for (std::size_t i = 0; i < vec.size(); ++i)
    {
        if (done[i])
        {
            continue;
        }
        done[i] = true;
        std::size_t prev_j = i;
        std::size_t j = p[i];
        while (i != j)
        {
            std::swap(vec[prev_j], vec[j]);
            done[j] = true;
            prev_j = j;
            j = p[j];
        }
    }
}*/

template<typename T, typename I>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, std::vector<T>& in, const I& name, std::vector<T>* other)
{
    constexpr bool is_owned = std::is_base_of_v<owned, std::remove_pointer_t<T>>;

    if(ctx.encode)
    {
        //if(serialisable_is_equal(&in, other))
        //    return;

        //if constexpr(!is_owned)
        //    data[name]["_c"] = in.size();

        ///think the problem is that we're skipping elements in the array which confuses everything

        nlohmann::json& mname = data[name];

        T* fptr = nullptr;

        if(other)
        {
            /*for(int i=0; i < (int)in.size() && i < (int)other->size(); i++)
            {
                if(serialisable_is_equal(&in[i], &(*other)[i]))
                    continue;

                do_serialise(ctx, data[name], in[i], std::to_string(i), &(*other)[i]);
            }

            for(int i=(int)other->size(); i < (int)in.size(); i++)
            {
                do_serialise(ctx, data[name], in[i], std::to_string(i), fptr);
            }*/

            #if 1
            if(in.size() == other->size())
            {
                for(int i=0; i < (int)in.size(); i++)
                {
                    ///force array element
                    mname[i] = nlohmann::json(nullptr);

                    do_serialise(ctx, mname, in[i], i, &(*other)[i]);
                }
            }
            else
            {
                for(int i=0; i < (int)in.size(); i++)
                {
                    do_serialise(ctx, mname, in[i], i, fptr);
                }
            }
            #endif // 0
        }
        else
        {
            for(int i=0; i < (int)in.size(); i++)
            {
                do_serialise(ctx, mname, in[i], i, fptr);
            }
        }
    }
    else
    {
        if(!nlohmann_has_name(data, name))
            return;

        nlohmann::json& mname = data[name];

        if constexpr(!is_owned)
        {
            T* fptr = nullptr;
            //in = std::vector<T>();

            //int num = mname["_c"];

            //int old_num = in.size();

            int num = mname.size();

            //printf("Num %i\n", num);

            /*std::map<int, nlohmann::json> dat;

            for(auto& info : data[name].items())
            {
                dat[std::stoi(info.key())] = info.value();
            }*/

            if(num < 0 || num >= 100000)
                throw std::runtime_error("Num out of range");

            in.resize(num);

            /*for(int i=0; i < (int)dat.size(); i++)
            {
                T next = T();
                do_serialise(ctx, data[name], next, std::to_string(i), fptr);

                in.push_back(next);
            }*/

            if(other == nullptr || other->size() != num)
            {
                for(int i=0; i < num; i++)
                {
                    do_serialise(ctx, mname, in[i], i, fptr);
                }
            }
            else
            {
                for(int i=0; i < num; i++)
                {
                    do_serialise(ctx, mname, in[i], i, &(*other)[i]);
                }
            }
        }

        if constexpr(is_owned)
        {
            using mtype = std::remove_pointer_t<T>;

            mtype* nptr = nullptr;

            std::map<size_t, mtype*> old_element_map;

            for(auto& i : in)
            {
                if constexpr(!std::is_pointer_v<T>)
                    old_element_map[i._pid] = &i;

                if constexpr(std::is_pointer_v<T>)
                    old_element_map[i->_pid] = i;
            }

            int resize_extra = 0;

            std::vector<size_t> unprocessed_indices;

            int num = mname.size();

            std::map<size_t, size_t> pid_to_index;

            for(int i=0; i < num; i++)
            {
                size_t pid = mname[i]["_pid"];

                pid_to_index[pid] = i;

                if(old_element_map.find(pid) == old_element_map.end())
                {
                    unprocessed_indices.push_back(i);
                    resize_extra++;
                }
            }

            in.erase( std::remove_if(in.begin(), in.end(), [&](const T& my_val)
            {
                if constexpr(!std::is_pointer_v<T>)
                    return pid_to_index.find(my_val._pid) == pid_to_index.end();
                if constexpr(std::is_pointer_v<T>)
                    return pid_to_index.find(my_val->_pid) == pid_to_index.end();
            }),
            in.end());

            size_t old_size = in.size();

            in.resize(old_size + resize_extra);

            ///problem is we're serialising into old vector instead of new, ordering problems
            for(size_t i=0; i < old_size; i++)
            {
                mtype* elem_ptr = nullptr;

                if constexpr(!std::is_pointer_v<T>)
                    elem_ptr = &in[i];

                if constexpr(std::is_pointer_v<T>)
                    elem_ptr = in[i];

                size_t real_index = pid_to_index[elem_ptr->_pid];

                ///our pid used to exist in the last iteration
                if(other == nullptr || other->size() != in.size())
                    do_serialise(ctx, mname, *elem_ptr, real_index, nptr);
                else
                {
                    mtype* other_val = nullptr;

                    if constexpr(!std::is_pointer_v<T>)
                        other_val = &((*other)[real_index]);

                    if constexpr(std::is_pointer_v<T>)
                        other_val = ((*other)[real_index]);

                    do_serialise(ctx, mname, *elem_ptr, real_index, other_val);
                }
            }

            /*std::map<size_t, bool> uniqueness;

            for(auto& i : in)
            {
                if constexpr(!std::is_pointer_v<T>)
                {
                    assert(uniqueness.find(i._pid) == uniqueness.end());
                    uniqueness[i._pid] = true;
                }

                if constexpr(std::is_pointer_v<T>)
                {
                    assert(uniqueness.find(i->_pid) == uniqueness.end());
                    uniqueness[i->_pid] = true;
                }
            }*/

            for(size_t i=old_size; i < in.size(); i++)
            {
                int idx = i - old_size;

                //assert(idx < (int)unprocessed_indices.size());

                int real_index = unprocessed_indices[idx];

                size_t pid = mname[real_index]["_pid"];

                mtype* elem_ptr = nullptr;

                if constexpr(!std::is_pointer_v<T>)
                {
                    elem_ptr = &in[i];
                }

                if constexpr(std::is_pointer_v<T>)
                {
                    in[i] = new mtype();
                    elem_ptr = in[i];
                }

                elem_ptr->_pid = pid;

                do_serialise(ctx, mname, *elem_ptr, real_index, nptr);
            }

            /*assert(in.size() == num);
            assert(num == (int)pid_to_index.size());
            assert(pid_to_index.size() == in.size());*/

            std::sort(in.begin(), in.end(), [&](auto& i1, auto& i2)
            {
                size_t pid1, pid2;

                if constexpr(!std::is_pointer_v<T>)
                {
                    pid1 = i1._pid;
                    pid2 = i2._pid;
                }

                if constexpr(std::is_pointer_v<T>)
                {
                    pid1 = i1->_pid;
                    pid2 = i2->_pid;
                }

                return pid_to_index[pid1] < pid_to_index[pid2];
            });

            /*
            //assert(mname.count("_c") == 0);

            ///this is quite slow if T is not a pointer
            ///fixme, is probably why deserialisation is so slow

            std::vector<T> new_element_vector;

            int num = mname.size();

            for(int i=0; i < num; i++)
            {
                size_t _pid = mname[std::to_string(i)]["_pid"];

                if(old_element_map[_pid])
                {
                    do_serialise(ctx, mname, *old_element_map[_pid], std::to_string(i), nptr);

                    if constexpr(!std::is_pointer_v<T>)
                        new_element_vector.push_back(*old_element_map[_pid]);
                    if constexpr(std::is_pointer_v<T>)
                        new_element_vector.push_back(old_element_map[_pid]);
                }
                else
                {
                    T nelem = T();

                    if constexpr(!std::is_pointer_v<T>)
                        nelem._pid = _pid;
                    if constexpr(std::is_pointer_v<T>)
                    {
                        nelem = new mtype();
                        nelem->_pid = _pid;
                    }

                    do_serialise(ctx, mname, nelem, std::to_string(i), fptr);

                    new_element_vector.push_back(nelem);
                }
            }

            in = new_element_vector;*/


        }
    }
}

///does not support ownership yet
template<typename T, typename U, typename I>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, std::map<T, U>& in, const I& name, std::map<T, U>* other)
{
    T* fptr = nullptr;

    if(ctx.encode)
    {
        //if(serialisable_is_equal(&in, other))
        //    return;

        int idx = 0;

        for(auto& i : in)
        {
            T cstr = i.first;

            do_serialise(ctx, data[name][idx], cstr, "f", fptr);
            do_serialise(ctx, data[name][idx], i.second, "s", fptr);

            idx++;
        }
    }
    else
    {
        if(!nlohmann_has_name(data, name))
            return;

        int idx = 0;

        in.clear();

        for(auto& i : data[name].items())
        {
            T first = T();
            U second = U();

            do_serialise(ctx, data[name][idx], first, "f", fptr);
            do_serialise(ctx, data[name][idx], second, "s", fptr);

            in[first] = second;

            idx++;

            (void)i;
        }
    }
}

template<typename T>
inline
void do_recurse(serialise_context& ctx, T& in)
{
    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        //func(in);

        in.serialise(ctx, ctx.faux);
    }

    if constexpr(!std::is_base_of_v<serialisable, T>)
    {

    }
}

template<typename T>
inline
void do_recurse(serialise_context& ctx, T*& in)
{
    do_recurse(ctx, *in);
}

template<typename T>
inline
void do_recurse(serialise_context& ctx, std::vector<T>& in)
{
    for(auto& i : in)
    {
        do_recurse(ctx, i);
    }
}

template<typename T, typename U>
inline
void do_recurse(serialise_context& ctx, std::map<T, U>& in)
{
    for(auto& i : in)
    {
        do_recurse(ctx, i.second);
    }
}

///so
///implement a recurse function that simply executes a function against all datamembers
///then, iteratate through structs touching all datamembers, except that if we hit a FUNC_RPC and we're side_1
///write args to serialise context
///if we're side_2, execute function with those args
///will probably have to make weird rpc syntax or something, or an rpc function

template<typename T>
inline
nlohmann::json serialise(T& in)
{
    serialise_context ctx;
    ctx.encode = true;
    ctx.serialisation = true;

    nlohmann::json data;

    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        in.serialise(ctx, data);
    }

    if constexpr(!std::is_base_of_v<serialisable, T>)
    {
        data = in;
    }

    if constexpr(std::is_base_of_v<owned, T>)
    {
        if(ctx.encode)
        {
            data["_pid"] = in._pid;
        }
        else
        {
            in._pid = data["_pid"];
        }
    }

    return data;
}

///produces the serialisation of in, assuming that the client has against
template<typename T>
inline
nlohmann::json serialise_against(T& in, T& against)
{
    serialise_context ctx;
    ctx.encode = true;
    ctx.serialisation = true;

    nlohmann::json data;

    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        in.serialise(ctx, data, &against);
    }

    if constexpr(!std::is_base_of_v<serialisable, T>)
    {
        data = in;
    }

    if constexpr(std::is_base_of_v<owned, T>)
    {
        if(ctx.encode)
        {
            data["_pid"] = in._pid;
        }
        else
        {
            in._pid = data["_pid"];
        }
    }

    return data;
}

template<typename T>
inline
T deserialise(nlohmann::json& in)
{
    serialise_context ctx;
    ctx.encode = false;
    ctx.serialisation = true;

    T ret;

    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        ret.serialise(ctx, in);
    }

    if constexpr(!std::is_base_of_v<serialisable, T>)
    {
        ret = (T)in;
    }

    if constexpr(std::is_base_of_v<owned, T>)
    {
        if(ctx.encode && in.count("_pid") > 0)
        {
            in["_pid"] = ret._pid;
        }

        if(!ctx.encode)
        {
            ret._pid = in["_pid"];
        }
    }

    return ret;
}

template<typename T>
inline
void deserialise(nlohmann::json& in, T& dat)
{
    serialise_context ctx;
    ctx.encode = false;
    ctx.serialisation = true;

    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        dat.serialise(ctx, in);
    }

    if constexpr(!std::is_base_of_v<serialisable, T>)
    {
        dat = (T)in;
    }

    if constexpr(std::is_base_of_v<owned, T>)
    {
        if(ctx.encode && in.count("_pid") > 0)
        {
            in["_pid"] = dat._pid;
        }

        if(!ctx.encode)
        {
            dat._pid = in["_pid"];
        }
    }
}

void save_to_file(const std::string& fname, const nlohmann::json& data);
nlohmann::json load_from_file(const std::string& fname);

///ok
///we gotta do pointers basically
///or each frame, just before checking rpcs, make a map if ids to things that could be rpc'd
///bit dangerous though if we touch containers
template<typename T, typename... V>
inline
void rpc(const std::string& func_name, T& obj, const V&... args)
{
    global_serialise_info& ser = get_global_serialise_info();

    nlohmann::json narg = args_to_nlohmann(args...);

    rpc_data dat;
    dat.id = obj._pid;
    dat.func = func_name;
    dat.arg = narg;

    ser.all_rpcs.push_back(dat);
}

#define RPC(x, ...) rpc(#x, *this, x, __VA_ARGS__)

template<int N, int M, typename... T>
inline
void extract_args(std::tuple<T...>& in_tup, nlohmann::json& args)
{
    if constexpr(N < M)
    {
        auto found = std::get<N>(in_tup);

        deserialise(args[N], found);

        std::get<N>(in_tup) = found;

        extract_args<N+1, M, T...>(in_tup, args);
    }
}

template<typename C, typename R, typename... Args>
inline
void exec_rpc(R(C::*func)(Args...), C& obj, nlohmann::json& args)
{
    constexpr int nargs = sizeof...(Args);

    std::tuple<Args...> tup_args;

    extract_args<0, nargs, Args...>(tup_args, args);

    std::apply(func, std::tuple_cat(std::forward_as_tuple(obj), tup_args));
}

template<typename T>
inline
bool serialisable_is_eq_impl(serialise_context& ctx, T& one, T& two)
{
    if(!ctx.is_eq_so_far)
        return false;

    constexpr bool is_serialisable = std::is_base_of_v<serialisable, T>;

    constexpr bool is_owned = std::is_base_of_v<owned, T>;

    if constexpr(is_owned)
    {
        if(one._pid != two._pid)
        {
            ctx.is_eq_so_far = false;
            return false;
        }
    }

    if constexpr(!is_serialisable)
    {
        if(one != two)
        {
            ctx.is_eq_so_far = false;
            return false;
        }

        return true;
    }

    ///this hack is getting kind of bad
    nlohmann::json dummy;

    if constexpr(is_serialisable)
    {
        one.serialise(ctx, dummy, &two);
    }

    return ctx.is_eq_so_far;
}

template<typename T>
inline
bool serialisable_is_eq_impl(serialise_context& ctx, std::vector<T>& one, std::vector<T>& two)
{
    if(one.size() != two.size())
    {
        return false;
    }

    for(int i=0; i < (int)one.size(); i++)
    {
        if(!serialisable_is_eq_impl(ctx, one[i], two[i]))
            return false;
    }

    return true;
}

template<typename T, typename U>
inline
bool serialisable_is_eq_impl(serialise_context& ctx, std::map<T, U>& one, std::map<T, U>& two)
{
    if(one.size() != two.size())
    {
        return false;
    }

    for(auto& i : one)
    {
        if(two.find(i) == two.end())
            return false;

        if(!serialisable_is_eq_impl(ctx, i.second, two[i.first]))
            return false;
    }

    return true;
}

template<typename T>
inline
bool serialisable_is_eq_impl(serialise_context& ctx, T*& one, T*& two)
{
    if(one == nullptr && two == nullptr)
        return true;

    if(one == nullptr || two == nullptr)
        return false;

    return serialisable_is_eq_impl(ctx, *one, *two);
}

template<typename T>
inline
bool serialisable_is_equal(T* one, T* two)
{
    if(one == nullptr && two == nullptr)
        return true;

    if(one == nullptr || two == nullptr)
        return false;

    serialise_context ctx;
    ctx.check_eq = true;

    return serialisable_is_eq_impl(ctx, *one, *two);
}

template<typename T>
inline
void serialisable_clone(T& one, T& into)
{
    nlohmann::json s1 = serialise(one);

    deserialise(s1, into);
}

void serialise_tests();

#endif // SERIALISABLE_HPP_INCLUDED
