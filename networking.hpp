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

template<typename T>
struct writes_data
{
    uint64_t id = 0;
    T data = T();
};

struct connection
{
    void host(const std::string& address, uint16_t port);
    void connect(const std::string& address, uint16_t port);

    std::optional<uint64_t> has_new_client();
    void pop_new_client();

    std::vector<uint64_t> clients();

    bool has_read();
    write_data read_from();
    std::string read();
    void pop_read();

    static inline thread_local int thread_is_client = 0;
    static inline thread_local int thread_is_server = 0;

    template<typename T>
    writes_data<T> reads_from()
    {
        write_data data = read_from();

        nlohmann::json nl = nlohmann::json::parse(data.data);

        T ret = deserialise<T>(nl);

        return {data.id, ret};
    }

    void write_to(const write_data& data);
    void write(const std::string& data);

    void writes_to(serialisable& data, uint64_t id)
    {
        nlohmann::json ret = serialise(data);

        write_data dat;
        dat.id = id;
        dat.data = ret.dump();

        write_to(dat);
    }

    std::mutex mut;
    std::vector<write_data> write_queue;
    std::vector<write_data> read_queue;

    std::atomic_int id = 0;
    std::vector<uint64_t> new_clients;
    std::vector<uint64_t> connected_clients;
    std::vector<std::thread> thrd;

private:
    bool is_client = true;
    bool is_connected = false;
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

#endif // NETWORKING_HPP_INCLUDED
