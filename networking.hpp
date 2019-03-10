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

        nlohmann::json nl = nlohmann::json::from_cbor(data.data);

        T ret = deserialise<T>(nl);

        return {data.id, ret};
    }

    void write_to(const write_data& data);
    void write(const std::string& data);

    void writes_to(serialisable& data, uint64_t id)
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

    virtual void serialise(nlohmann::json& data, bool encode) override
    {
        if(encode)
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


///ok so these two classes are compatible with each other
///the idea is that the owning one sets itself to be host_persistent by inheriting
///then serialises pid
///then the child one uses persistent<type>

///maybe if host_persistent<> kept a far away version
///then whenever we receive a new one, we copy over data members from far away to us
///and vice versa

///maybe just have a unique base which contains a _pid and integrate it right into
///serialisation so it does the detection and stuff
template<typename T>
struct persistent : serialisable
{
    size_t _pid = 0;

    persistent()
    {
        _pid = get_next_persistent_id();
    }

    T& operator*()
    {
        std::shared_ptr<T>& sptr = get_tls_ptr<T>(_pid);

        return *sptr.get();
    }

    T* operator->()
    {
        std::shared_ptr<T>& sptr = get_tls_ptr<T>(_pid);

        return sptr.get();
    }

    virtual void serialise(nlohmann::json& data, bool encode) override
    {
        DO_SERIALISE(_pid);

        std::shared_ptr<T>& sptr = get_tls_ptr<T>(_pid);

        T* ptr = sptr.get();

        ptr->serialise(data, encode);
    }

    /*persistent(const persistent<T>& other)
    {
        pid = get_next_persistent_id();

        std::shared_ptr<T>& p1 = get_tls_ptr<T>(pid);
        std::shared_ptr<T>& p2 = get_tls_ptr<T>(other.pid);

        *p1.get() = *p2.get();
    }

    persistent<T>& operator=(const persistent<T>& other)
    {
        std::shared_ptr<T>& p1 = get_tls_ptr<T>(pid);
        std::shared_ptr<T>& p2 = get_tls_ptr<T>(other.pid);

        *p1.get() = *p2.get();

        return *this;
    }*/
};

template<typename T>
struct host_persistent : virtual serialisable
{
    size_t _pid = 0;

    host_persistent()
    {
        _pid = get_next_persistent_id();
    }

    virtual void serialise(nlohmann::json& data, bool encode) override
    {
        DO_SERIALISE(_pid);
    }
};

#endif // NETWORKING_HPP_INCLUDED
