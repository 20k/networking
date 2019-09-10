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

namespace connection_type
{
    enum type
    {
        PLAIN,
        SSL
    };
}

struct connection
{
    void host(const std::string& address, uint16_t port, connection_type::type type = connection_type::PLAIN);
    void connect(const std::string& address, uint16_t port, connection_type::type type = connection_type::PLAIN);

    std::optional<uint64_t> has_new_client();
    void pop_new_client();

    std::optional<uint64_t> has_disconnected_client();
    void pop_disconnected_client();

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

    std::mutex disconnected_lock;
    std::vector<uint64_t> disconnected_clients;

    std::atomic_int client_connected_to_server{0};
    std::atomic_bool should_terminate{false};

    ~connection(){should_terminate = true; for(auto& i : thrd){i.join();}}

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
    global_serialise_info rpcs;

    SERIALISE_SIGNATURE(network_protocol)
    {
        DO_SERIALISE(type);
        DO_SERIALISE(data);
        DO_SERIALISE(rpcs);
    }
};

template<typename T>
struct network_data_model
{
    std::map<uint64_t, T> data;
    std::map<uint64_t, T> backup;

    T& fetch_by_id(uint64_t id)
    {
        backup[id];

        return data[id];
    }

    void update_client(connection& conn, uint64_t i, int stagger_id)
    {
        T& next_data = fetch_by_id(i);

        if(backup.find(i) != backup.end())
        {
            nlohmann::json ret = serialise_against(next_data, backup[i], true, stagger_id);

            std::vector<uint8_t> cb = nlohmann::json::to_cbor(ret);

            write_data dat;
            dat.id = i;
            dat.data = std::string(cb.begin(), cb.end());

            conn.write_to(dat);

            ///basically clones model, by applying the current diff to last model
            ///LEAKS MEMORY ON POINTERS
            deserialise(ret, backup[i]);
        }
        else
        {
            nlohmann::json ret = serialise(next_data);

            std::vector<uint8_t> cb = nlohmann::json::to_cbor(ret);

            write_data dat;
            dat.id = i;
            dat.data = std::string(cb.begin(), cb.end());

            conn.write_to(dat);

            backup[i] = T();
            serialisable_clone(next_data, backup[i]);
        }
    }
};

#endif // NETWORKING_HPP_INCLUDED
