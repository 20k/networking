#ifndef SERIALISABLE_HPP_INCLUDED
#define SERIALISABLE_HPP_INCLUDED

#include <nlohmann/json.hpp>
#include <type_traits>
#include <vec/vec.hpp>
#include <map>
#include <iostream>

#include "serialisable_fwd.hpp"
#include <ndb/db_storage.hpp>
#include "netinterpolate.hpp"

///its going to be this kind of file
///if this makes you sad its not getting any better from here

template<typename T>
bool serialisable_is_equal(T* one, T* two);

namespace ratelimits
{
    enum stagger_mode
    {
        NO_STAGGER,
        STAGGER,
    };
}

namespace interpolation_mode
{
    enum type
    {
        NONE,
        SMOOTH,
        ONLY_IF_STAGGERED
    };
}

namespace serialise_mode
{
    enum type
    {
        NETWORK,
        DISK,
    };
}

#define PID_STRING "_"

#define DO_SERIALISE_BASE(x, rlim, stagger) do{ \
                            static uint32_t my_id##_x = id_counter++; \
                            static std::string s##_x = std::to_string(my_id##_x);\
                            const std::string& my_name = ctx.mode == serialise_mode::NETWORK ? s##_x : #x;\
                            assert(ctx.mode == serialise_mode::NETWORK || ctx.mode == serialise_mode::DISK);\
                            if(ctx.serialisation) \
                            { \
                                decltype(this->x)* fptr = nullptr;\
                                \
                                if(other) \
                                    fptr = &other->x; \
                                \
                                last_ratelimit_time.resize(id_counter); \
                                \
                                bool skip = false; \
                                if(ctx.mode == serialise_mode::NETWORK && ctx.ratelimit && ctx.encode && rlim > 0) \
                                { \
                                    size_t current_time = time_ms(); \
                                    \
                                    if(current_time < last_ratelimit_time[my_id##_x] + rlim) \
                                        skip = true; \
                                    else \
                                        last_ratelimit_time[my_id##_x] = current_time; \
                                } \
                                if(stagger == ratelimits::STAGGER) \
                                    ctx.stagger_stack++; \
                                \
                                if(!skip && (other == nullptr || !ctx.encode || !serialisable_is_equal_cached(ctx, &this->x, fptr))) \
                                    do_serialise(ctx, data, this->x, my_name, fptr); \
                                \
                                if(stagger == ratelimits::STAGGER) \
                                    ctx.stagger_stack--; \
                            } \
                            if(ctx.exec_rpcs) \
                            { \
                                do_recurse(ctx, this->x); \
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
                            if(ctx.get_by_id) \
                            { \
                                if(ctx.get_by_id_found)\
                                    return;\
                                find_owned_id(ctx, this->x); \
                            }\
                            \
                            if(ctx.update_interpolation){\
                                if(stagger == ratelimits::STAGGER) \
                                    ctx.stagger_stack++;\
                                do_recurse(ctx, this->x);\
                                if(stagger == ratelimits::STAGGER) \
                                    ctx.stagger_stack--;\
                            }\
                            \
                        }while(0);

#define DO_SERIALISE_INTERPOLATE_IMPL(x, mode) do{ \
                            static uint32_t my_id##_x = id_counter2++; \
                            static std::string s##_x = std::to_string(my_id##_x);\
                            last_vals.resize(id_counter2);\
                            if((mode == interpolation_mode::ONLY_IF_STAGGERED && ctx.stagger_stack > 0) || mode == interpolation_mode::SMOOTH)\
                            {\
                                if(ctx.update_interpolation)\
                                    this->x = last_vals[my_id##_x].get_update<decltype(this->x)>();\
                                if(ctx.serialisation && !ctx.encode)\
                                    last_vals[my_id##_x].add_val(this->x, serialisable_time_ms());\
                            }\
                        }while(0);

#define DO_SERIALISE(x) DO_SERIALISE_BASE(x, 0, ratelimits::NO_STAGGER)

#define DO_SERIALISE_SMOOTH(x, y)   DO_SERIALISE_BASE(x, 0, ratelimits::NO_STAGGER) \
                                    DO_SERIALISE_INTERPOLATE_IMPL(x, y)

#define DO_SERIALISE_RATELIMIT(x, y, z) DO_SERIALISE_BASE(x, y, z)

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

uint32_t string_hash(const std::string& in);

//uint32_t hacky_string_hash(const char* static_string);

inline
bool nlohmann_has_name(const nlohmann::json& data, const char* name)
{
    auto it = data.find(name);

    return it != data.end() && !it->is_null();
}

inline
bool nlohmann_has_name(const nlohmann::json& data, size_t name)
{
    return name < data.size() && !data[name].is_null();
}

inline
bool nlohmann_has_name(const nlohmann::json& data, const std::string& name)
{
    auto it = data.find(name);

    return it != data.end() && !it->is_null();
}

nlohmann::json& nlohmann_index(nlohmann::json& data, const std::string& name);
nlohmann::json& nlohmann_index(nlohmann::json& data, int name);

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

/*struct serialise_context;

struct serialise_context_proxy
{
    nlohmann::json& last;

    serialise_context_proxy(serialise_context& in);
    serialise_context_proxy(serialise_context_proxy& in, const char* name);
    serialise_context_proxy(serialise_context_proxy& in, int name);
};*/

struct serialise_context
{
    nlohmann::json data;
    nlohmann::json faux; ///fake nlohmann

    serialise_mode::type mode = serialise_mode::NETWORK;

    bool encode = false;
    bool recurse = false;
    bool serialisation = false;

    global_serialise_info inf;
    bool exec_rpcs = false;

    bool caching = false;
    bool check_eq = false;
    ///used for comparing serialisable objects
    bool is_eq_so_far = true;

    std::map<uint64_t, bool> cache;

    bool ratelimit = false;
    int stagger_id = 0;
    int stagger_stack = 0;

    bool get_by_id = false;
    size_t get_id = -1;
    bool get_by_id_found = false;
    void* get_by_id_ptr = nullptr; ///well, this is bad!

    bool update_interpolation = false;
};

template<typename T>
nlohmann::json serialise(T& in, serialise_mode::type mode = serialise_mode::NETWORK);

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
        if(serialisable_is_equal_cached(ctx, &in, other))
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

    if(!ctx.encode && !nlohmann_has_name(data, name))
        return;

    if constexpr(is_serialisable)
    {
        if(ctx.encode)
        {
            /*bool eq_cached = serialisable_is_equal_cached(ctx, &in, other);

            if(eq_cached)
                return;*/

            if constexpr(std::is_base_of_v<owned, T>)
            {
                if(ctx.ratelimit && ctx.stagger_stack > 0)
                {
                    ///the problem with this i think, is that if we store subcomponents with non congruent stuff itll never be sent
                    if((ctx.stagger_id % 32) != (int)(in._pid % 32))
                        return;

                    //data[name][PID_STRING] = in._pid;
                }
            }

            if(serialisable_is_equal(&in, other))
                return;
        }

        bool staggering = ctx.stagger_stack > 0;

        if(staggering)
            ctx.stagger_stack--;

        in.serialise(ctx, data[name], other);

        if(staggering)
            ctx.stagger_stack++;

        if constexpr(is_owned)
        {
            if(ctx.encode)
                data[name][PID_STRING] = in._pid;
            else
                in._pid = data[name][PID_STRING];
        }
    }

    if constexpr(!is_serialisable)
    {
        if(ctx.encode)
        {
            if(serialisable_is_equal_cached(ctx, &in, other))
                return;

            data[name] = in;
        }
        else
        {
            in = data[name];

            if constexpr(is_owned)
            {
                in._pid = data[name][PID_STRING];
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

    if(ctx.encode)
    {
        ///think the problem is that we're skipping elements in the array which confuses everything
        T* fptr = nullptr;

        if(!is_owned)
        {
            nlohmann::json& mname = data[name]["a"];
            data[name]["c"] = in.size();

            if(other && in.size() == other->size())
            {
                for(int i=0; i < (int)in.size(); i++)
                {
                    do_serialise(ctx, mname, in[i], std::to_string(i), &(*other)[i]);
                }
            }
            else
            {
                for(int i=0; i < (int)in.size(); i++)
                {
                    do_serialise(ctx, mname, in[i], std::to_string(i), fptr);
                }
            }
        }
        else
        {
            nlohmann::json& mname = data[name]["a"];
            data[name]["c"] = in.size();

            if(other && in.size() == other->size())
            {
                for(int i=0; i < (int)in.size(); i++)
                {
                    do_serialise(ctx, mname, in[i], std::to_string(i), &(*other)[i]);
                }
            }
            else
            {
                for(int i=0; i < (int)in.size(); i++)
                {
                    do_serialise(ctx, mname, in[i], std::to_string(i), fptr);
                }
            }
        }

    }
    else
    {
        if(!nlohmann_has_name(data, name))
            return;

        //if constexpr(!is_owned)
        {
            nlohmann::json& mname = data[name]["a"];
            int num = data[name]["c"];

            T* fptr = nullptr;

            /*int num = mname.size();

            if(num < 0 || num >= 100000)
                throw std::runtime_error("Num out of range");

            in.resize(num);

            if(other == nullptr || other->size() != (size_t)num)
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
            }*/

            //int num = mname.size();

            if(num < 0 || num >= 100000)
                throw std::runtime_error("Num out of range");

            in.resize(num);

            for(auto& i : mname.items())
            {
                int idx = std::stoi(i.key());

                if(idx < 0 || idx >= (int)in.size())
                    throw std::runtime_error("Bad");

                if(other == nullptr || other->size() != (size_t)num)
                {
                    do_serialise(ctx, mname, in[idx], std::to_string(idx), fptr);
                }
                else
                {
                    do_serialise(ctx, mname, in[idx], std::to_string(idx), &(*other)[idx]);
                }

                /*if constexpr(is_owned)
                {
                    if constexpr(std::is_pointer_v<T>)
                        in[idx]->_pid = mname[std::to_string(idx)][PID_STRING];
                    if constexpr(!std::is_pointer_v<T>)
                        in[idx]._pid = mname[std::to_string(idx)][PID_STRING];
                }*/
            }
        }

        /*if constexpr(is_owned)
        {
            nlohmann::json& mname = data[name]["a"];
            int num = data[name]["c"];

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

            //int num = mname.size();

            std::map<size_t, size_t> pid_to_index;

            for(int i=0; i < num; i++)
            {
                size_t pid = mname[i][PID_STRING];

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


            for(size_t i=old_size; i < in.size(); i++)
            {
                int idx = i - old_size;

                //assert(idx < (int)unprocessed_indices.size());

                int real_index = unprocessed_indices[idx];

                size_t pid = mname[real_index][PID_STRING];

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
        }*/
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

template<typename T>
inline
void find_owned_id(serialise_context& ctx, T& in)
{
    if(ctx.get_by_id_found)
        return;

    if constexpr(std::is_base_of_v<owned, T>)
    {
        if(in._pid == ctx.get_id)
        {
            ctx.get_by_id_found = true;
            ctx.get_by_id_ptr = &in;
            return;
        }
    }

    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        //func(in);

        in.serialise(ctx, ctx.faux);
    }
}

template<typename T>
inline
void find_owned_id(serialise_context& ctx, T*& in)
{
    find_owned_id(ctx, *in);
}

template<typename T>
inline
void find_owned_id(serialise_context& ctx, std::vector<T>& in)
{
    for(auto& i : in)
    {
        find_owned_id(ctx, i);
    }
}

template<typename T, typename U>
inline
void find_owned_id(serialise_context& ctx, std::map<T, U>& in)
{
    for(auto& i : in)
    {
        find_owned_id(ctx, i.second);
    }
}

template<typename T>
inline
owned* find_by_id(T& in, size_t id)
{
    serialise_context ctx;
    ctx.get_by_id = true;
    ctx.get_id = id;

    find_owned_id(ctx, in);

    if(ctx.get_by_id_found)
        return (owned*)ctx.get_by_id_ptr;

    return nullptr;
}

///so
///implement a recurse function that simply executes a function against all datamembers
///then, iteratate through structs touching all datamembers, except that if we hit a FUNC_RPC and we're side_1
///write args to serialise context
///if we're side_2, execute function with those args
///will probably have to make weird rpc syntax or something, or an rpc function

template<typename T>
inline
void update_interpolated_variables(T& in)
{
    serialise_context ctx;
    ctx.update_interpolation = true;

    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        in.serialise(ctx, ctx.faux);
    }
}

template<typename T>
inline
nlohmann::json serialise(T& in, serialise_mode::type mode)
{
    serialise_context ctx;
    ctx.encode = true;
    ctx.serialisation = true;
    ctx.mode = mode;

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
            data[PID_STRING] = in._pid;
        }
        else
        {
            in._pid = data[PID_STRING];
        }
    }

    return data;
}

///produces the serialisation of in, assuming that the client has against
template<typename T>
inline
nlohmann::json serialise_against(T& in, T& against, bool ratelimit, int stagger_id)
{
    serialise_context ctx;
    ctx.encode = true;
    ctx.serialisation = true;
    ctx.ratelimit = ratelimit;
    ctx.stagger_id = stagger_id;

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
            data[PID_STRING] = in._pid;
        }
        else
        {
            in._pid = data[PID_STRING];
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
        ret._pid = in[PID_STRING];
    }

    return ret;
}

template<typename T>
inline
void deserialise(nlohmann::json& in, T& dat, serialise_mode::type mode = serialise_mode::NETWORK)
{
    serialise_context ctx;
    ctx.encode = false;
    ctx.serialisation = true;
    ctx.mode = mode;

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
        dat._pid = in[PID_STRING];
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
    constexpr bool is_serialisable = std::is_base_of_v<serialisable, T>;

    constexpr bool is_owned = std::is_base_of_v<owned, T>;

    bool success = false;
    bool has_success = false;
    /*bool do_cache = false;

    if(ctx.caching && is_serialisable && is_owned)
    {
        auto it_1 = ctx.cache.find((uint64_t)&one);

        if(it_1 != ctx.cache.end())
        {
            return it_1->second;
        }
        else
        {
            do_cache = true;
        }
    }*/

    if constexpr(is_owned)
    {
        if(one._pid != two._pid)
        {
            has_success = true;
        }
    }

    if constexpr(!is_serialisable)
    {
        if(!has_success)
        {
            success = one == two;
            has_success = true;
        }
    }

    if constexpr(is_serialisable)
    {
        if(!has_success)
        {
            ///this hack is getting kind of bad
            nlohmann::json dummy;

            one.serialise(ctx, dummy, &two);
            success = ctx.is_eq_so_far;
            has_success = true;
        }
    }

    /*if(do_cache)
    {
        ctx.cache[(uint64_t)&one] = success;
    }*/

    return success;
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
bool serialisable_is_equal_cached(serialise_context& octx, T* one, T* two)
{
    if(one == nullptr && two == nullptr)
        return true;

    if(one == nullptr || two == nullptr)
        return false;

    serialise_context ctx;
    ctx.check_eq = true;
    ctx.cache = std::move(octx.cache);
    ctx.caching = true;

    bool res = serialisable_is_eq_impl(ctx, *one, *two);

    octx.cache = std::move(ctx.cache);

    return res;
}

template<typename T>
inline
void serialisable_clone(T& one, T& into)
{
    nlohmann::json s1 = serialise(one);

    deserialise(s1, into);
}

void serialise_tests();

template<typename T>
inline
void serialise_to_db(const std::string& key, T& in, db_read_write& tx)
{
    nlohmann::json data = serialise(in, serialise_mode::DISK);

    auto vec = nlohmann::json::to_cbor(data);

    if(vec.size() > 0)
    {
        std::string_view view((const char*)&vec[0], vec.size());

        tx.write(key, view);
    }
    else
    {
        tx.write(key, "");
    }
}

template<typename T>
inline
bool serialise_from_db(const std::string& key, T& in, db_read& tx)
{
    std::optional<db_data> data = tx.read(key);

    if(!data)
        return false;

    nlohmann::json js = nlohmann::json::from_cbor(data.value().data);

    deserialise(js, in, serialise_mode::DISK);

    return true;
}

///keep db dirty state so we can defer

template<typename T>
struct db_storable
{
    std::string key;

    void save(db_read_write& tx)
    {
        serialise_to_db(key, static_cast<T&>(*this), tx);
    }

    bool load(const std::string& _key, db_read& tx)
    {
        key = _key;

        return serialise_from_db(key, static_cast<T&>(*this), tx);
    }

    void del(db_read_write& tx)
    {
        tx.del(key);
    }
};

#define DB_PERSIST_ID 2

#endif // SERIALISABLE_HPP_INCLUDED
