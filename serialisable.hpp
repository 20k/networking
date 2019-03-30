#ifndef SERIALISABLE_HPP_INCLUDED
#define SERIALISABLE_HPP_INCLUDED

#include <nlohmann/json.hpp>
#include <type_traits>
#include <vec/vec.hpp>
#include <memory>
#include <fstream>
#include <map>
#include <type_traits>

///its going to be this kind of file
///if this makes you sad its not getting any better from here
template<typename T>
struct class_extractor;

template<typename C, typename R, typename... Args>
struct class_extractor<R(C::*)(Args...)>
{
    using class_t = C;
};

struct serialise_context;

#define SERIALISE_SIGNATURE() void _internal_helper(){}\
using self_t = typename class_extractor<decltype(&_internal_helper)>::class_t;\
void serialise(serialise_context& ctx, nlohmann::json& data, self_t* other = nullptr)

#define DO_SERIALISE(x) do{ \
                            if(ctx.serialisation) \
                            { \
                                if(other) \
                                { \
                                    if(!serialisable_is_equal(this->x, other->x)) \
                                        do_serialise(ctx, data, x, std::string(#x)); \
                                }  \
                                else \
                                { \
                                    do_serialise(ctx, data, x, std::string(#x)); \
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

inline
void args_to_nlohmann_1(nlohmann::json& in, serialise_context& ctx, int& idx)
{

}

template<typename T, typename... U>
inline
void args_to_nlohmann_1(nlohmann::json& in, serialise_context& ctx, int& idx, T& one, U&... two)
{
    if constexpr(std::is_base_of_v<T, serialisable>)
    {
        one.serialise(ctx, in[idx]);
    }

    if constexpr(!std::is_base_of_v<T, serialisable>)
    {
        in[idx] = one;
    }

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

inline
global_serialise_info& get_global_serialise_info()
{
    thread_local static global_serialise_info inf;

    return inf;
}

inline
size_t get_next_persistent_id()
{
    thread_local static size_t gpid = 0;

    return gpid++;
}

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

template<int N, typename T>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, vec<N, T>& in, const std::string& name)
{
    if(ctx.encode)
    {
        for(int i=0; i < N; i++)
        {
            data[name][i] = in.v[i];
        }
    }
    else
    {
        for(int i=0; i < N; i++)
        {
            in.v[i] = data[name][i];
        }
    }
}

template<typename T>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, T& in, const std::string& name)
{
    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        in.serialise(ctx, data[name]);

        if constexpr(std::is_base_of_v<owned, T>)
        {
            data[name]["_pid"] = in._pid;
        }
    }

    if constexpr(!std::is_base_of_v<serialisable, T>)
    {
        if(ctx.encode)
        {
            data[name] = in;
        }
        else
        {
            in = data[name];

            if constexpr(std::is_base_of_v<owned, T>)
            {
                in._pid = data[name]["_pid"];
            }
        }
    }
}

template<typename T>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, T*& in, const std::string& name)
{
    assert(in);

    do_serialise(ctx, data, *in, name);
}

template<typename T>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, std::vector<T>& in, const std::string& name)
{
    if(ctx.encode)
    {
        for(int i=0; i < (int)in.size(); i++)
        {
            do_serialise(ctx, data[name], in[i], std::to_string(i));
        }
    }
    else
    {
        if constexpr(!std::is_base_of_v<owned, T>)
        {
            in = std::vector<T>();

            std::map<int, nlohmann::json> dat;

            for(auto& info : data[name].items())
            {
                dat[std::stoi(info.key())] = info.value();
            }

            for(int i=0; i < (int)dat.size(); i++)
            {
                T next = T();
                do_serialise(ctx, data[name], next, std::to_string(i));

                in.push_back(next);
            }
        }

        if constexpr(std::is_base_of_v<owned, T>)
        {
            //std::map<size_t, bool> received;
            std::map<size_t, bool> has;

            std::map<int, bool> dat;

            std::map<size_t, T*> old_element_map;

            for(auto& i : in)
            {
                has[i._pid] = true;
                old_element_map[i._pid] = &i;
            }

            for(auto& info : data[name].items())
            {
                //received[info.value()["_pid"]] = true;
                dat[std::stoi(info.key())] = true;
            }

            ///remove any elements not received this tick
            /*in.erase(std::remove_if(in.begin(), in.end(), [&](T& val)
            {
                return !received[val._pid];
            }));*/

            std::vector<T> new_element_vector;

            for(int i=0; i < (int)dat.size(); i++)
            {
                size_t _pid = data[name][std::to_string(i)]["_pid"];

                if(has[_pid])
                {
                    do_serialise(ctx, data[name], *old_element_map[_pid], std::to_string(i));

                    new_element_vector.push_back(*old_element_map[_pid]);
                }
                else
                {
                    T nelem = T();
                    nelem._pid = _pid;

                    do_serialise(ctx, data[name], nelem, std::to_string(i));

                    new_element_vector.push_back(nelem);
                }
            }

            in = new_element_vector;
        }
    }
}

///does not support ownership yet
template<typename T, typename U>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, std::map<T, U>& in, const std::string& name)
{
    if(ctx.encode)
    {
        int idx = 0;

        for(auto& i : in)
        {
            T cstr = i.first;

            do_serialise(ctx, data[name][idx], cstr, "f");
            do_serialise(ctx, data[name][idx], i.second, "s");

            idx++;
        }
    }
    else
    {
        int idx = 0;

        in.clear();

        for(auto& i : data[name].items())
        {
            T first = T();
            U second = U();

            do_serialise(ctx, data[name][idx], first, "f");
            do_serialise(ctx, data[name][idx], second, "s");

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

/*template<typename T>
inline
bool do_is_eq(serialise_context& ctx, T& in_1, T& in_2)
{

}*/

///so
///implement a recurse function that simply executes a function against all datamembers
///then, iteratate through structs touching all datamembers, except that if we hit a FUNC_RPC and we're side_1
///write args to serialise context
///if we're side_2, execute function with those args
///will probably have to make weird rpc syntax or something, or an rpc function

struct test_serialisable : serialisable
{
    SERIALISE_SIGNATURE();

    int test_datamember = 0;
};

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
}

inline
void save_to_file(const std::string& fname, const nlohmann::json& data)
{
    std::vector<unsigned char> input = nlohmann::json::to_cbor(data);
    std::ofstream out(fname, std::ios::binary);
    out << std::string(input.begin(), input.end());
}

inline
nlohmann::json load_from_file(const std::string& fname)
{
    std::ifstream t(fname, std::ios::binary);
    std::string str((std::istreambuf_iterator<char>(t)),
                     std::istreambuf_iterator<char>());

    return nlohmann::json::from_cbor(str);
}

///ok
///we gotta do pointers basically
///or each frame, just before checking rpcs, make a map if ids to things that could be rpc'd
///bit dangerous though if we touch containers
template<typename T, typename U, typename... V>
inline
void rpc(const std::string& func_name, T& obj, U func, const V&... args)
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

    if constexpr(!std::is_base_of_v<serialisable, T>)
    {
        return one == two;
    }

    ///this hack is getting kind of bad
    nlohmann::json dummy;

    if constexpr(std::is_base_of_v<serialisable, T>)
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

        if(!serialisable_is_eq_impl(i.second, two[i]))
            return false;
    }

    return true;
}

template<typename T>
inline
bool serialisable_is_eq_impl(serialise_context& ctx, T*& one, T*& two)
{
    if(one != two)
        return false;

    if(one == nullptr)
        return true;

    return serialisable_is_eq_impl(ctx, *one, *two);
}

template<typename T>
inline
bool serialisable_is_equal(T& one, T& two)
{
    serialise_context ctx;
    ctx.check_eq = true;

    return serialisable_is_eq_impl(ctx, one, two);
}

void serialise_tests();

#endif // SERIALISABLE_HPP_INCLUDED
