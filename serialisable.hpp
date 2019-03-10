#ifndef SERIALISABLE_HPP_INCLUDED
#define SERIALISABLE_HPP_INCLUDED

#include <nlohmann/json.hpp>
#include <type_traits>
#include <vec/vec.hpp>
#include <memory>

struct serialisable
{
    virtual void serialise(nlohmann::json& data, bool encode){}

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
void do_serialise(nlohmann::json& data, vec<N, T>& in, const std::string& name, bool encode)
{
    if(encode)
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
void do_serialise(nlohmann::json& data, T& in, const std::string& name, bool encode)
{
    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        in.serialise(data[name], encode);

        if constexpr(std::is_base_of_v<owned, T>)
        {
            data[name]["_pid"] = in._pid;
        }
    }

    if constexpr(!std::is_base_of_v<serialisable, T>)
    {
        if(encode)
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
void do_serialise(nlohmann::json& data, T*& in, const std::string& name, bool encode)
{
    assert(in);

    do_serialise(data, *in, name, encode);
}

template<typename T>
void do_serialise(nlohmann::json& data, std::vector<T>& in, const std::string& name, bool encode)
{
    if(encode)
    {
        for(int i=0; i < (int)in.size(); i++)
        {
            do_serialise(data[name], in[i], std::to_string(i), encode);
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
                do_serialise(data[name], next, std::to_string(i), encode);

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
                    do_serialise(data[name], *old_element_map[_pid], std::to_string(i), encode);

                    new_element_vector.push_back(*old_element_map[_pid]);
                }
                else
                {
                    T nelem = T();
                    nelem._pid = _pid;

                    do_serialise(data[name], nelem, std::to_string(i), encode);

                    new_element_vector.push_back(nelem);
                }
            }

            in = new_element_vector;
        }
    }
}


#define DO_SERIALISE(x){do_serialise(data, x, std::string(#x), encode);}

struct test_serialisable : serialisable
{
    virtual void serialise(nlohmann::json& data, bool encode) override;

    int test_datamember = 0;
};

template<typename T>
nlohmann::json serialise(T& in)
{
    nlohmann::json ret;

    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        in.serialise(ret, true);
    }

    if constexpr(!std::is_base_of_v<serialisable, T>)
    {
        ret = in;
    }

    return ret;
}

template<typename T>
T deserialise(nlohmann::json& in)
{
    T ret;

    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        ret.serialise(in, false);
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
    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        dat.serialise(in, false);
    }

    if constexpr(!std::is_base_of_v<serialisable, T>)
    {
        dat = (T)in;
    }
}

#endif // SERIALISABLE_HPP_INCLUDED
