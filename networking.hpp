#ifndef NETWORKING_HPP_INCLUDED
#define NETWORKING_HPP_INCLUDED

#include <string>
#include <stdint.h>
#include <thread>
#include <vector>
#include <mutex>
#include <optional>
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

    bool has_read();
    write_data read_from();
    std::string read();
    void pop_read();

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

    connection();

    std::mutex mut;
    std::vector<write_data> write_queue;
    std::vector<write_data> read_queue;

    uint64_t id = 0;
    std::vector<uint64_t> new_clients;

private:
    bool is_client = true;
    bool is_connected = false;
    std::vector<std::thread> thrd;
};

#endif // NETWORKING_HPP_INCLUDED
