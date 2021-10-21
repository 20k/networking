#ifndef SERIALISABLE_HPP_INCLUDED
#define SERIALISABLE_HPP_INCLUDED

#include <nlohmann/json.hpp>
#include <type_traits>
#include <vec/vec.hpp>
#include <map>

#include "serialisable_fwd.hpp"
#include "netinterpolate.hpp"

#ifdef SERIALISE_ENTT
#include <entt/entt.hpp>
#include <tuple>
#endif // SERIALISE_ENTT

#ifdef USE_MSGPACK
#include <msgpack.h>
#endif // USE_MSGPACK

#include <optional>

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

#define DO_SERIALISE_BASE(obj, x, rlim, stagger) do{ try{\
                            static uint32_t my_id##_x = id_counter++; \
                            static std::string s##_x = std::to_string(my_id##_x);\
                            const std::string& my_name = ctx.mode == serialise_mode::NETWORK ? s##_x : #x;\
                            assert(ctx.mode == serialise_mode::NETWORK || ctx.mode == serialise_mode::DISK);\
                            if(ctx.serialisation) \
                            { \
                                decltype(obj->x)* fptr = nullptr;\
                                \
                                if(other) \
                                    fptr = &other->x; \
                                \
                                bool skip = false; \
                                if constexpr(std::is_base_of_v<rate_limited, decltype(obj)>) \
                                { \
                                    obj->last_ratelimit_time.resize(id_counter); \
                                    \
                                    if(ctx.mode == serialise_mode::NETWORK && ctx.ratelimit && ctx.encode && rlim > 0) \
                                    { \
                                        size_t current_time = serialisable_time_ms(); \
                                        \
                                        if(current_time < obj->last_ratelimit_time[my_id##_x] + rlim) \
                                            skip = true; \
                                        else \
                                            obj->last_ratelimit_time[my_id##_x] = current_time; \
                                    } \
                                } \
                                if(stagger == ratelimits::STAGGER) \
                                    ctx.stagger_stack++; \
                                \
                                if(!skip && (other == nullptr || !ctx.encode || !serialisable_is_equal(&obj->x, fptr))) \
                                    do_serialise(ctx, json_data[my_name], obj->x, fptr); \
                                \
                                if(stagger == ratelimits::STAGGER) \
                                    ctx.stagger_stack--; \
                            } \
                            if(ctx.exec_rpcs) \
                            { \
                                do_recurse(ctx, obj->x); \
                            } \
                            if(ctx.check_eq) \
                            { \
                                if(!ctx.is_eq_so_far) \
                                    return; \
                                assert(other != nullptr); \
                                if(!serialisable_is_eq_impl(ctx, obj->x, other->x)) \
                                { \
                                    ctx.is_eq_so_far = false; \
                                    return; \
                                } \
                            } \
                            if(ctx.get_by_id) \
                            { \
                                if(ctx.get_by_id_found)\
                                    return;\
                                find_owned_id(ctx, obj->x); \
                            }\
                            \
                            if(ctx.update_interpolation){\
                                if(stagger == ratelimits::STAGGER) \
                                    ctx.stagger_stack++;\
                                do_recurse(ctx, obj->x);\
                                if(stagger == ratelimits::STAGGER) \
                                    ctx.stagger_stack--;\
                            }\
                            \
                        } catch(const std::exception& e) { throw; } } while(0);

#define DO_SERIALISE_INTERPOLATE_IMPL(obj, x, mode) do{ \
                            static uint32_t my_id##_x = id_counter2++; \
                            static std::string s##_x = std::to_string(my_id##_x);\
                            obj->last_vals.resize(id_counter2);\
                            if((mode == interpolation_mode::ONLY_IF_STAGGERED && ctx.stagger_stack > 0) || mode == interpolation_mode::SMOOTH)\
                            {\
                                if(ctx.update_interpolation)\
                                    obj->x = obj->last_vals[my_id##_x].get_update<decltype(obj->x)>();\
                                if(ctx.serialisation && !ctx.encode)\
                                    obj->last_vals[my_id##_x].add_val(obj->x, serialisable_time_ms());\
                            }\
                        }while(0);

#define DO_SERIALISE(x)  DO_SERIALISE_BASE(this, x, 0, ratelimits::NO_STAGGER)
#define DO_FSERIALISE(x) DO_SERIALISE_BASE(me, x, 0, ratelimits::NO_STAGGER)

#define DO_SERIALISE_SMOOTH(x, y)   DO_SERIALISE_BASE(this, x, 0, ratelimits::NO_STAGGER) \
                                    DO_SERIALISE_INTERPOLATE_IMPL(this, x, y)

#define DO_FSERIALISE_SMOOTH(x, y)  DO_SERIALISE_BASE(me, x, 0, ratelimits::NO_STAGGER) \
                                    DO_SERIALISE_INTERPOLATE_IMPL(me, x, y)

#define DO_SERIALISE_RATELIMIT(x, y, z)  DO_SERIALISE_BASE(this, x, y, z)
#define DO_FSERIALISE_RATELIMIT(x, y, z) DO_SERIALISE_BASE(me, x, y, z)

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

#define DO_FRPC(x) do{ \
                        if(ctx.exec_rpcs) \
                        { \
                            if(auto it = ctx.inf.built.find(me->_pid); it != ctx.inf.built.end()) \
                            { \
                                for(rpc_data& dat : it->second) \
                                { \
                                    if(dat.func == std::string(#x)) \
                                    { \
                                        exec_rpc(std::remove_pointer_t<decltype(me)>::x, *me, dat.arg); \
                                    } \
                                } \
                            } \
                        } \
                  }while(0)

#define FRIENDLY_RPC_NAME(function_name) template<typename... T> void function_name##_rpc(T&&... t) \
{ \
    rpc(#function_name , *this, std::forward<T>(t)...);\
}

#ifdef SERIALISE_ENTT
#define DECLARE_ENTT_SERIALISE() void serialise_entt_base(entt::entity en, serialise_context& ctx, nlohmann::json& json_data, uint32_t type_id, entt::entity* other)

//Forward declare. This is super hacky, not sure how to solve
DECLARE_ENTT_SERIALISE();

inline
entt::registry*& ptr_get_thread_local_registry()
{
    static thread_local entt::registry* reg = new entt::registry;

    return reg;
}

inline
void set_thread_local_registry(entt::registry& registry)
{
    delete ptr_get_thread_local_registry();
    ptr_get_thread_local_registry() = &registry;
}

#define DEFINE_ENTT_SERIALISE() \
    template<typename T> \
    inline \
    void for_each_tuple(T func) \
    { \
        entt_type_list empty_tuple; \
      \
        std::apply([&](auto&&... args) {((func(args)), ...);}, empty_tuple); \
    } \
      \
    void serialise_entt_base(entt::entity en, serialise_context& ctx, nlohmann::json& json_data, uint32_t type_id, entt::entity* other){ \
        (void)other; \
        for_each_tuple([&](auto& var) \
        { \
            (void)var; \
            using type = std::decay_t<decltype(var)>; \
            if(entt::type_info<type>::id() == type_id) \
            { \
                if(ctx.encode) \
                { \
                    type attach = type(); \
                    type* nptr = nullptr; \
                    do_serialise(ctx, json_data, attach, nptr); \
                    get_thread_local_registry().emplace<type>(en, attach); \
                } \
                else \
                { \
                    assert(get_thread_local_registry().has<type>(en)); \
                    type& val = get_thread_local_registry().get<type>(en); \
                    type* nptr = nullptr; \
                    do_serialise(ctx, json_data, val, nptr); \
                } \
            } \
        });  \
    }

#define DEFINE_ENTT_TYPES(...) using entt_type_list = std::tuple<__VA_ARGS__>

#define DO_ENTT_SERIALISE(x) \
                            if(entt::type_info<x>::id() == type_id) { \
                                if(!ctx.encode) \
                                { \
                                    x attach = x(); \
                                    do_serialise(ctx, json_data, attach, other); \
                                    get_thread_local_registry().emplace<x>(en, attach); \
                                } \
                                else \
                                { \
                                    assert(get_thread_local_registry().has<x>(en)); \
                                    x& val = get_thread_local_registry().get<x>(en); \
                                    do_serialise(ctx, json_data, val, other); \
                                } \
                                return; \
                             }

#define END_ENTT_SERIALISE() throw std::runtime_error("No such class found");

#endif // SERIALISE_ENTT

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

    SERIALISE_SIGNATURE(rpc_data);
};

struct global_serialise_info : serialisable
{
    std::vector<rpc_data> all_rpcs;

    std::map<size_t, std::vector<rpc_data>> built;

    SERIALISE_SIGNATURE(global_serialise_info);
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
    owned* get_by_id_ptr = nullptr; ///well, this is bad!

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

template<typename T>
inline
void make_finite(T& in)
{
    (void)in;
}

template<>
inline
void make_finite(float& in)
{
    if(!isfinite(in))
    {
        in = 0;
    }
}

template<>
inline
void make_finite(double& in)
{
    if(!isfinite(in))
    {
        in = 0;
    }
}

template<typename T>
inline
void call_serialise(T& in, serialise_context& ctx, nlohmann::json& json, T* other = nullptr)
{
    if constexpr(std::is_base_of_v<free_function, T>)
    {
        serialise_base(&in, ctx, json, other);
    }

    if constexpr(!std::is_base_of_v<free_function, T>)
    {
        in.serialise(ctx, json, other);
    }
}

#ifdef SERIALISE_ENTT
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, entt::entity& en, entt::entity* other)
{
    if(ctx.encode)
    {
        get_thread_local_registry().visit(en, [&](auto& component)
        {
            auto id = component;

            std::string string_name = std::to_string(id);

            serialise_entt_base(en, ctx, data[string_name], id, other);
        });
    }
    else
    {
        en = get_thread_local_registry().create();

        for(auto& it : data.items())
        {
            uint32_t id = std::stoul(it.key());

            serialise_entt_base(en, ctx, it.value(), id, other);
        }
    }
}
#endif // SERIALISE_ENTT

template<int N, typename T>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, vec<N, T>& in, vec<N, T>* other)
{
    if(ctx.encode)
    {
        if(serialisable_is_equal(&in, other))
            return;

        for(int i=0; i < N; i++)
        {
            data[i] = in.v[i];
        }
    }
    else
    {
        /*if(!nlohmann_has_name(data, name))
            return;*/

        if(data.is_null())
            return;

        for(int i=0; i < N; i++)
        {
            in.v[i] = data[i];

            make_finite(in.v[i]);
        }
    }
}

template<typename T>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, T& in, T* other)
{
    constexpr bool is_serialisable = std::is_base_of_v<serialisable, T>;
    constexpr bool is_owned = std::is_base_of_v<owned, T>;

    if(!ctx.encode && data.is_null())
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

        call_serialise(in, ctx, data, other);

        if(staggering)
            ctx.stagger_stack++;

        if constexpr(is_owned)
        {
            if(ctx.encode)
                data[PID_STRING] = in._pid;
            else
                in._pid = data[PID_STRING];
        }
    }

    if constexpr(!is_serialisable)
    {
        if(ctx.encode)
        {
            if(serialisable_is_equal(&in, other))
                return;

            data = in;
        }
        else
        {
            in = data;

            make_finite(in);

            if constexpr(is_owned)
            {
                in._pid = data[PID_STRING];
            }
        }
    }
}

template<typename T>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, T*& in, T** other)
{
    if(in == nullptr)
    {
        in = new T();
    }

    T* fptr = nullptr;

    if(other)
        do_serialise(ctx, data, *in, *other);
    else
        do_serialise(ctx, data, *in, fptr);
}

template<typename T>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, std::shared_ptr<T>& in, std::shared_ptr<T>* other)
{
    if(!in)
        in = std::make_shared<T>();

    T* fptr = nullptr;

    if(other)
        do_serialise(ctx, data, *in, &(**other));
    else
        do_serialise(ctx, data, *in, fptr);
}

template<typename T>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, std::optional<T>& in, std::optional<T>* other)
{
    T* nptr = nullptr;

    if(ctx.encode)
    {
        if(!in.has_value())
            return;

        if(other && other->has_value())
            do_serialise(ctx, data, in.value(), &other->value());
        else
            do_serialise(ctx, data, in.value(), nptr);
    }
    else
    {
        if(data.is_null())
        {
            in = std::nullopt;
        }
        else
        {
            T val = T();

            do_serialise(ctx, data, val, nptr);

            in = val;
        }
    }
}

template<typename T, std::size_t N>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, std::array<T, N>& in, std::array<T, N>* other)
{
    T* fptr = nullptr;

    for(int i=0; i < (int)N; i++)
    {
        if(other)
            do_serialise(ctx, data[i], in[i], &(*other)[i]);
        else
            do_serialise(ctx, data[i], in[i], fptr);
    }
}

template<typename T>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, std::vector<T>& in, std::vector<T>* other)
{
    //constexpr bool is_owned = std::is_base_of_v<owned, std::remove_pointer_t<T>>;
    T* fptr = nullptr;

    if(ctx.mode == serialise_mode::DISK)
    {
        if(ctx.encode)
        {
            for(int i=0; i < (int)in.size(); i++)
            {
                do_serialise(ctx, data[i], in[i], fptr);
            }

            if(in.size() == 0)
            {
                data = nlohmann::json::array();
            }
        }
        else
        {
            int num = data.size();

            in.resize(num);

            for(int i=0; i < num; i++)
            {
                do_serialise(ctx, data[i], in[i], fptr);
            }
        }
    }

    if(ctx.mode == serialise_mode::NETWORK)
    {
        if(ctx.encode)
        {
            ///think the problem is that we're skipping elements in the array which confuses everything

            nlohmann::json& mname = data["a"];
            data["c"] = in.size();

            if(other && in.size() == other->size())
            {
                for(int i=0; i < (int)in.size(); i++)
                {
                    std::string idx = std::to_string(i);

                    do_serialise(ctx, mname[idx], in[i], &(*other)[i]);

                }
            }
            else
            {
                for(int i=0; i < (int)in.size(); i++)
                {
                    std::string idx = std::to_string(i);

                    do_serialise(ctx, mname[idx], in[i], fptr);
                }
            }
        }
        else
        {
            /*if(!nlohmann_has_name(data, name))
                return;*/

            if(data.is_null())
                return;

            //if constexpr(!is_owned)
            {
                nlohmann::json& mname = data["a"];
                int num = data["c"];

                T* nptr = nullptr;

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
                        do_serialise(ctx, i.value(), in[idx], nptr);
                    }
                    else
                    {
                        do_serialise(ctx, i.value(), in[idx], &(*other)[idx]);
                    }
                }
            }
        }
    }
}

///does not support ownership yet
template<typename T, typename U>
inline
void do_serialise(serialise_context& ctx, nlohmann::json& data, std::map<T, U>& in, std::map<T, U>* other)
{
    //T* fptr = nullptr;
    //U* uptr = nullptr;

    std::vector<T>* vfptr = nullptr;
    std::vector<U>* vuptr = nullptr;

    std::vector<T> keys;
    std::vector<U> vals;

    for(auto& i : in)
    {
        keys.push_back(i.first);
        vals.push_back(i.second);
    }

    std::vector<T> okeys;
    std::vector<U> ovals;

    if(other)
    {
        for(auto& i : *other)
        {
            okeys.push_back(i.first);
            ovals.push_back(i.second);
        }
    }

    if(ctx.encode)
    {
        if(other)
        {
            do_serialise(ctx, data["f"], keys, &okeys);
            do_serialise(ctx, data["s"], vals, &ovals);
        }
        else
        {
            do_serialise(ctx, data["f"], keys, vfptr);
            do_serialise(ctx, data["s"], vals, vuptr);
        }
    }
    else
    {
        if(other)
        {
            do_serialise(ctx, data["f"], keys, &okeys);
            do_serialise(ctx, data["s"], vals, &ovals);
        }
        else
        {
            do_serialise(ctx, data["f"], keys, vfptr);
            do_serialise(ctx, data["s"], vals, vuptr);
        }

        if(keys.size() != vals.size())
            throw std::runtime_error("Bad decode in map");

        in.clear();

        for(int i=0; i < (int)keys.size(); i++)
        {
            in[keys[i]] = vals[i];
        }
    }

    #if 0
    if(ctx.encode)
    {
        //if(serialisable_is_equal(&in, other))
        //    return;

        int idx = 0;

        for(auto& i : in)
        {
            /*bool found = false;

            T cstr = i.first;

            do_serialise(ctx, data[name][idx], cstr, "f", fptr);

            if(other)
            {
                auto them_it = other->find(i.first);

                if(them_it != other->end())
                {
                    do_serialise(ctx, data[name][idx], i.second, "s", &them_it->second);

                    found = true;
                }
            }

            if(!found)
                do_serialise(ctx, data[name][idx], i.second, "s", uptr);*/

            bool found = false;

            if(other)
            {
                auto them_it = other->find(i.first);

                if(them_it != other->end())
                {
                    do_serialise(ctx, data[name], i.second, i.first, &them_it->second);
                    found = true;
                }
            }

            if(!found)
                do_serialise(ctx, data[name], i.second, i.first, uptr);

            idx++;
        }
    }
    else
    {
        if(!nlohmann_has_name(data, name))
            return;

        if(other)
        {
            for(auto it = in.begin(); it != in.end();)
            {
                if(!nlohmann_has_name(data[name], it->first))
                {
                    it = in.erase(it);
                }
                else
                {
                    T fst = it->first;

                    auto o_it = other->find(fst);

                    if(o_it != other->end())
                        do_serialise(ctx, data[name], it->second, fst, &o_it->second);
                    else
                        do_serialise(ctx, data[name], it->second, fst, uptr);

                    it++;
                }
            }

            for(auto& i : data[name].items())
            {
                U second = U();
                T first = i.key();

                if(in.find(first) != in.end())
                    continue;

                do_serialise(ctx, data[name], second, first, uptr);

                in[i.first] = second;
            }
        }
        else
        {
            in.clear();

            for(auto& i : data[name].items())
            {
                U second = U();
                T first = i.key();

                do_serialise(ctx, data[name], second, first, uptr);

                in[first] = second;
            }
        }

        #if 0
        int idx = 0;

        //in.clear();

        std::map<T, U> nmap;

        for(auto& i : data[name].items())
        {
            T first = T();
            U second = U();

            if(!nlohmann_has_name(data[name], idx))
            {
                idx++;
                continue;
            }

            do_serialise(ctx, data[name][idx], first, "f", fptr);

            if(in.find(first) != in.end())
            {
                //nmap[first] = in.find(first)->second;
            }

            bool found = false;

            if(other)
            {
                auto them_it = other->find(first);

                auto my_it = nmap.find(first);

                if(them_it != other->end() && my_it != nmap.end())
                {
                    do_serialise(ctx, data[name][idx], my_it->second, "s", &them_it->second);
                    found = true;
                }
            }

            if(!found)
            {
                do_serialise(ctx, data[name][idx], second, "s", uptr);

                nmap[first] = second;
            }

            //used[first] = true;

            idx++;

            (void)i;
        }

        in = nmap;
        #endif // 0

        /*for(auto it = in.begin(); it != in.end();)
        {
            if(!used[it->first])
                it = in.erase(it);
            else
                it++;
        }*/
    }
    #endif // 0
}

template<typename T>
inline
void do_recurse(serialise_context& ctx, T& in)
{
    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        //func(in);

        call_serialise(in, ctx, ctx.faux);
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

template<typename T, std::size_t N>
inline
void do_recurse(serialise_context& ctx, std::array<T, N>& in)
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
void do_recurse(serialise_context& ctx, std::shared_ptr<T>& ptr)
{
    do_recurse(ctx, *ptr);
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

        call_serialise(in, ctx, ctx.faux);
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

template<typename T, std::size_t N>
inline
void find_owned_id(serialise_context& ctx, std::array<T, N>& in)
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
void find_owned_id(serialise_context& ctx, std::shared_ptr<T>& in)
{
    find_owned_id(ctx, *in);
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
        call_serialise(in, ctx, ctx.faux);
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
        call_serialise(in, ctx, data);
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
        call_serialise(in, ctx, data, &against);
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
        call_serialise(ret, ctx, in);
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
        call_serialise(dat, ctx, in);
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

void save_to_file_json(const std::string& fname, const nlohmann::json& data);
nlohmann::json load_from_file_json(const std::string& fname);

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

            call_serialise(one, ctx, dummy, &two);
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

template<typename T, std::size_t N>
inline
bool serialisable_is_eq_impl(serialise_context& ctx, std::array<T, N>& one, std::array<T, N>& two)
{
    for(int i=0; i < (int)N; i++)
    {
        if(!serialisable_is_eq_impl(ctx, one[i], two[i]))
            return false;
    }

    return true;
}


template<typename T>
inline
bool serialisable_is_eq_impl(serialise_context& ctx, std::shared_ptr<T>& one, std::shared_ptr<T>& two)
{
    if((bool)one != (bool)two)
        return false;

    if(!one || !two)
        return true;

    return serialisable_is_eq_impl(ctx, *one, *two);
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
        if(two.find(i.first) == two.end())
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

#endif // SERIALISABLE_HPP_INCLUDED
