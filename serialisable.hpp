#ifndef SERIALISABLE_HPP_INCLUDED
#define SERIALISABLE_HPP_INCLUDED

#include <nlohmann/json.hpp>
#include <type_traits>

struct serialisable
{
    virtual void serialise(nlohmann::json& data, bool encode);
};

template<typename T>
void do_serialise(nlohmann::json& data, T& in, const std::string& name, bool encode)
{
    if constexpr(std::is_base_of_v<serialisable, T>)
    {
        in.serialise(data[name], encode);
    }

    if constexpr(!std::is_base_of_v<serialisable, T>)
    {
        if(encode)
        {
            data[name] = in;
        }
        else
        {
            if(data.count(name) == 0)
            {
                in = T();
            }
            else
            {
                in = data[name];
            }
        }
    }
}

#define DO_SERIALISE(x){do_serialise(data, x, #x, encode);}

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

#endif // SERIALISABLE_HPP_INCLUDED
