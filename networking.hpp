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

template<typename T>
using connection_queue_type = std::deque<T>;

struct http_read_info
{
    /*enum method
    {
        head,
        body,
        other
    };*/

    enum request_type
    {
        GET,
        POST,
    };

    request_type type = request_type::GET;

    std::string path;
    bool keep_alive = false;
    std::string body;
};

struct http_data
{
    const char* ptr = nullptr;
    size_t len = 0;
    bool owned = false;

    http_data(){}
    http_data(std::string_view view); ///view
    http_data(const char* data, size_t size); ///own. there is unfortunately no way to take a std::string here
    http_data(http_data&& other);
    http_data& operator=(http_data&& other);
    ~http_data();

    http_data(const http_data&) = delete;
    http_data& operator=(const http_data&) = delete;

    const char* data() const;
    size_t size() const;
};

struct http_write_info
{
    enum status_code
    {
        ok,
        bad_request,
        not_found,
    };

    status_code code = status_code::bad_request;

    uint64_t id;
    std::string mime_type;
    http_data body;
    bool keep_alive = false;
    bool cross_origin_isolated = true;
};

struct connection_received_data
{
    std::deque<uint64_t> new_clients;
    std::deque<uint64_t> new_http_clients;
    std::deque<uint64_t> disconnected_clients;
    std::deque<uint64_t> upgraded_to_websocket;

    std::map<uint64_t, std::vector<write_data>> websocket_read_queue;
    std::map<uint64_t, std::vector<http_read_info>> http_read_queue;

    //std::optional<write_data> get_next_read();

private:
    size_t last_read_from = -1;
};

struct connection_send_data
{
    connection_settings sett;

    connection_send_data(const connection_settings& _sett);

    std::map<uint64_t, connection_queue_type<write_data>> websocket_write_queue;
    std::map<uint64_t, connection_queue_type<http_write_info>> http_write_queue;
    std::set<uint64_t> force_disconnection_list;

    void disconnect(uint64_t id);
    ///returns true on success
    bool write_to_websocket(const write_data& dat);
    bool write_to_websocket(write_data&& dat);
    bool write_to_http(http_write_info&& info);
    //bool write_to_http_unchecked(http_write_info info);
    bool write_to_http_unchecked(http_write_info&& info);
};

///so: Todo. I think the submitting side needs to essentially create a batch of work, that gets transferred all at once
///that way, this avoids the toctou problem with some of this api, and would especially avoid toctou while doing http
struct connection
{
    void host(const std::string& address, uint16_t port, connection_type::type type = connection_type::PLAIN, connection_settings sett = connection_settings());
    void connect(const std::string& address, uint16_t port, connection_type::type type = connection_type::PLAIN, std::string sni_hostname = "");

    void send_bulk(connection_send_data& in);
    void receive_bulk(connection_received_data& in);

    size_t last_read_from = -1;

    bool connection_pending();

    void set_client_sleep_interval(uint64_t time_ms);

    std::mutex mut;

    ///fat because it protects everything read and write related
    std::mutex fat_readwrite_mutex;
    std::map<uint64_t, connection_queue_type<write_data>> pending_websocket_write_queue;
    std::map<uint64_t, connection_queue_type<http_write_info>> pending_http_write_queue;

    std::map<uint64_t, std::vector<write_data>> pending_websocket_read_queue;
    std::map<uint64_t, std::vector<http_read_info>> pending_http_read_queue;

    std::mutex wake_lock;
    std::vector<uint64_t> wake_queue;

    std::atomic_int id = 0;
    std::deque<uint64_t> new_clients;
    std::deque<uint64_t> new_http_clients;
    std::deque<uint64_t> upgraded_to_websocket;
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

    std::atomic_int client_sleep_interval_ms = 1;
private:
    bool is_client = true;
    bool is_connected = false;
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
#if 0
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
#endif // 0
#endif

#endif // NETWORKING_HPP_INCLUDED
