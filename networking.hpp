#ifndef NETWORKING_HPP_INCLUDED
#define NETWORKING_HPP_INCLUDED

#include <string>
#include <stdint.h>
#include <thread>
#include <vector>
#include <mutex>
#include <optional>
#include <atomic>
#include "serialisable.hpp"

struct write_data
{
    uint64_t id = 0;
    std::string data;
};

/*template<typename T>
struct writes_data
{
    uint64_t id = 0;
    T data = T();
};*/

struct connection
{
    void host(const std::string& address, uint16_t port);
    void connect(const std::string& address, uint16_t port);

    std::optional<uint64_t> has_new_client();
    void pop_new_client();

    std::vector<uint64_t> clients();

    size_t last_read_from = -1;

    bool has_read();
    write_data read_from();
    //std::string read();
    void pop_read(uint64_t id);

    static inline thread_local int thread_is_client = 0;
    static inline thread_local int thread_is_server = 0;

    template<typename T>
    uint64_t reads_from(T& old)
    {
        write_data data = read_from();

        nlohmann::json nl = nlohmann::json::from_cbor(data.data);

        deserialise<T>(nl, old);

        return data.id;
    }


    /*template<typename T>
    writes_data<T> reads_from()
    {
        T none = T();
        return reads_from(none);
    }*/

    void write_to(const write_data& data);
    void write(const std::string& data);

    template<typename T>
    void writes_to(T& data, uint64_t id)
    {
        nlohmann::json ret = serialise(data);

        std::vector<uint8_t> cb = nlohmann::json::to_cbor(ret);

        write_data dat;
        dat.id = id;
        dat.data = std::string(cb.begin(), cb.end());
        //dat.data = ret.dump();

        write_to(dat);
    }

    std::mutex mut;
    std::map<uint64_t, std::vector<write_data>> directed_write_queue;
    std::map<uint64_t, std::mutex> directed_write_lock;

    //std::vector<write_data> read_queue;

    std::map<uint64_t, std::vector<write_data>> fine_read_queue;
    std::map<uint64_t, std::mutex> fine_read_lock;

    std::atomic_int id = 0;
    std::vector<uint64_t> new_clients;
    std::vector<uint64_t> connected_clients;
    std::vector<std::thread> thrd;

private:
    bool is_client = true;
    bool is_connected = false;
};

namespace network_mode
{
    enum type
    {
        STEAM_AUTH,
        DATA,
        COUNT
    };
}

struct network_protocol : serialisable
{
    network_mode::type type = network_mode::COUNT;
    nlohmann::json data;

    SERIALISE_SIGNATURE(network_protocol)
    {
        DO_SERIALISE(type);
        DO_SERIALISE(data);
    }
};

///I am a variable that lives on the server
///do not accept client input, aka don't decode
template<typename T>
inline
void server_serialise(nlohmann::json& data, T& in, const std::string& name, bool encode)
{
    if(connection::thread_is_server)
    {
        if(!encode)
            return;

        do_serialise(data, in, name, encode);
        return;
    }

    if(connection::thread_is_client)
    {
        if(encode)
            return;

        do_serialise(data, in, name, encode);
        return;
    }
}

///lives on the client, networked to server
template<typename T>
inline
void client_serialise(nlohmann::json& data, T& in, const std::string& name, bool encode)
{
    if(connection::thread_is_server)
    {
        if(encode)
            return;

        do_serialise(data, in, name, encode);
        return;
    }

    if(connection::thread_is_client)
    {
        if(!encode)
            return;

        do_serialise(data, in, name, encode);
        return;
    }
}

template<typename T>
struct delta_container : serialisable
{
    T c;

    using value_type = typename T::value_type;

    std::vector<value_type> d;

    void push_back(value_type&& u)
    {
        c.push_back(u);

        d.push_back(u);
    }

    auto begin()
    {
        return c.begin();
    }

    auto end()
    {
        return c.end();
    }

    template<typename U>
    auto erase(U&& u)
    {
        return c.erase(u);
    }

    auto size()
    {
        return c.size();
    }

    auto& operator[](size_t idx){return c[idx];}

    SERIALISE_SIGNATURE(delta_container<T>)
    {
        if(!ctx.serialisation)
            return;

        if(ctx.encode)
        {
            if(d.size() == 0)
                return;

            DO_SERIALISE(d);
            d.clear();
        }
        else
        {
            DO_SERIALISE(d);
            normalise();

            //if(d.size() != 0)
            //std::cout << "ds " << d.size() << std::endl;
        }
    }

    void normalise()
    {
        for(auto& i : d)
        {
            c.push_back(i);
        }

        d.clear();
    }
};

#endif // NETWORKING_HPP_INCLUDED
