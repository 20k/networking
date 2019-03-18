#ifndef SERIALISABLE_HPP_INCLUDED
#define SERIALISABLE_HPP_INCLUDED

#include <nlohmann/json.hpp>
#include <type_traits>
#include <vec/vec.hpp>
#include <memory>
#include <fstream>
#include <map>

struct serialise_context
{
    //nlohmann::json data;
    nlohmann::json faux; ///fake nlohmann

    bool encode = false;
};

#define SERIALISE_SIGNATURE() virtual void serialise(serialise_context& ctx, nlohmann::json& data) override
#define DECLARE_RPC(x) std::vector<nlohmann::json> x ## rpc;

struct serialisable
{
    virtual void serialise(serialise_context& ctx, nlohmann::json& data){}

    static size_t time_ms();

    virtual ~serialisable();
};

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
        if(data.count(name) == 0)
        {
            in = vec<N, T>();
        }
        else
        {
            for(int i=0; i < N; i++)
            {
                in.v[i] = data[name][i];
            }
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
            if(data.find(name) == data.end())
            {
                in = T();
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

template<typename T, typename U>
inline
void do_recurse(serialise_context& ctx, T& in, const std::string& name, const U& func)
{
    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        func(in);

        in.serialise(ctx, ctx.faux);
    }

    if constexpr(!std::is_base_of_v<serialisable, T>)
    {
        func(in);
    }
}

template<typename T, typename U>
inline
void do_recurse(serialise_context& ctx, T*& in, const std::string& name, const U& func)
{
    do_recurse(ctx, *in, name, func);
}

template<typename T, typename U>
inline
void do_recurse(serialise_context& ctx, std::vector<T>& in, const std::string& name, const U& func)
{
    for(auto& i : in)
    {
        do_recurse(ctx, in, name, func);
    }
}

template<typename T, typename U, typename V>
inline
void do_recurse(serialise_context& ctx, std::map<T, U>& in, const std::string& name, const V& func)
{
    for(auto& i : in)
    {
        do_recurse(ctx, i.second, name, func);
    }
}

///so
///implement a recurse function that simply executes a function against all datamembers
///then, iteratate through structs touching all datamembers, except that if we hit a FUNC_RPC and we're side_1
///write args to serialise context
///if we're side_2, execute function with those args
///will probably have to make weird rpc syntax or something, or an rpc function

#define DO_SERIALISE(x){do_serialise(ctx, data, x, std::string(#x));}

struct test_serialisable : serialisable
{
    virtual void serialise(serialise_context& ctx, nlohmann::json& data) override;

    int test_datamember = 0;
};

template<typename T>
nlohmann::json serialise(T& in)
{
    serialise_context ctx;
    ctx.encode = true;

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
T deserialise(nlohmann::json& in)
{
    serialise_context ctx;
    ctx.encode = false;

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
void deserialise(nlohmann::json& in, T& dat)
{
    serialise_context ctx;
    ctx.encode = false;

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

#endif // SERIALISABLE_HPP_INCLUDED
