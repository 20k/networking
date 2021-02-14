#ifndef NETWORKING_HPP_INCLUDED
#define NETWORKING_HPP_INCLUDED

#include <string>
#include <stdint.h>
#include <thread>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <atomic>
#include <map>
#include <set>
#include <deque>

#ifndef NO_SERIALISATION
#include "serialisable.hpp"
#endif

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
        SSL,
        EMSCRIPTEN_AUTOMATIC
    };
}

struct connection_settings
{
    int compression_level = 8;
    int memory_level = 4;
    int max_window_bits = 15; ///affects socket memory usage, must be >= 9
    bool enable_compression = true;

    ///uncompressed, handled through boost::beast
    uint64_t max_read_size = 16 * 1024 * 1024;
    ///uncompressed unfortunately, handled through beast
    uint64_t max_write_size = 16 * 1024 * 1024;
};

struct connection_received_data
{
    std::deque<uint64_t> new_clients;
    std::deque<uint64_t> disconnected_clients;

    std::map<uint64_t, std::vector<write_data>> read_queue;

    //std::optional<write_data> get_next_read();

private:
    size_t last_read_from = -1;
};

struct connection_send_data
{
    connection_settings sett;

    connection_send_data(const connection_settings& _sett);

    std::map<uint64_t, std::vector<write_data>> write_queue;
    std::set<uint64_t> force_disconnection_list;

    void disconnect(uint64_t id);
    ///returns true on success
    bool write_to(const write_data& dat);
};

///so: Todo. I think the submitting side needs to essentially create a batch of work, that gets transferred all at once
///that way, this avoids the toctou problem with some of this api, and would especially avoid toctou while doing http
struct connection
{
    void host(const std::string& address, uint16_t port, connection_type::type type = connection_type::PLAIN, connection_settings sett = connection_settings());
    void connect(const std::string& address, uint16_t port, connection_type::type type = connection_type::PLAIN, std::string sni_hostname = "");

    void send_bulk(connection_send_data& in);
    void receive_bulk(connection_received_data& in);

    [[deprecated]]
    std::optional<uint64_t> has_new_client();
    [[deprecated]]
    void pop_new_client();

    [[deprecated]]
    std::optional<uint64_t> has_disconnected_client();
    [[deprecated]]
    void pop_disconnected_client();

    size_t last_read_from = -1;

    bool connection_pending();

    [[deprecated]]
    bool has_read();
    [[deprecated]]
    write_data read_from();
    [[deprecated]]
    void pop_read(uint64_t id);

    [[deprecated]]
    void force_disconnect(uint64_t id) noexcept;

    void set_client_sleep_interval(uint64_t time_ms);

    #ifndef NO_SERIALISATION
    template<typename T>
    [[deprecated]]
    uint64_t reads_from(T& old)
    {
        write_data data = read_from();

        nlohmann::json nl = nlohmann::json::from_cbor(data.data);

        deserialise<T>(nl, old);

        return data.id;
    }
    #endif

    [[deprecated]]
    void write_to(const write_data& data);
    [[deprecated]]
    void write(const std::string& data);

    #ifndef NO_SERIALISATION
    template<typename T>
    [[deprecated]]
    void writes_to(T& data, uint64_t to_id)
    {
        nlohmann::json ret = serialise(data);

        std::vector<uint8_t> cb = nlohmann::json::to_cbor(ret);

        write_data dat;
        dat.id = to_id;
        dat.data = std::string(cb.begin(), cb.end());
        //dat.data = ret.dump();

        write_to(dat);
    }
    #endif

    std::mutex mut;
    std::map<uint64_t, std::vector<write_data>> directed_write_queue;
    std::map<uint64_t, std::mutex> directed_write_lock;

    //std::vector<write_data> read_queue;

    std::map<uint64_t, std::vector<write_data>> fine_read_queue;
    std::map<uint64_t, std::mutex> fine_read_lock;

    std::mutex wake_lock;
    std::vector<uint64_t> wake_queue;

    std::atomic_int id = 0;
    std::deque<uint64_t> new_clients;
    std::vector<std::thread> thrd;

    std::mutex disconnected_lock;
    std::deque<uint64_t> disconnected_clients;

    std::mutex force_disconnection_lock;
    std::set<uint64_t> force_disconnection_queue;

    std::mutex free_id_queue_lock;
    std::vector<uint64_t> free_id_queue;

    std::atomic_int client_connected_to_server{0};
    std::atomic_bool should_terminate{false};
    std::atomic_bool connection_in_progress = false;

    ~connection(){should_terminate = true; for(auto& i : thrd){i.join();}}

    const connection_settings& get_settings(){return sett;}

private:
    bool is_client = true;
    bool is_connected = false;
    int client_sleep_interval_ms = 1;
    connection_settings sett;
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

#ifndef NO_SERIALISATION
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
#endif

#endif // NETWORKING_HPP_INCLUDED
