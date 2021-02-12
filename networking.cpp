#include "networking.hpp"

#ifdef __EMSCRIPTEN__
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <iostream>
#include <toolkit/clock.hpp>
#include <netinet/tcp.h>
#endif // __EMSCRIPTEN__

#ifndef __EMSCRIPTEN__
#define BOOST_BEAST_SEPARATE_COMPILATION

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

#include <boost/asio/buffer.hpp>
#include <memory>
#include <thread>
#include <vector>
#include <SFML/System/Sleep.hpp>
#include <fstream>

using tcp = boost::asio::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>
namespace ssl = boost::asio::ssl;               // from <boost/asio/ssl.hpp>

namespace
{

std::string read_file_bin(const std::string& file)
{
    std::ifstream t(file, std::ios::binary);
    std::string str((std::istreambuf_iterator<char>(t)),
                     std::istreambuf_iterator<char>());

    if(!t.good())
        throw std::runtime_error("Could not open file " + file);

    return str;
}

//#define CONNECTION_PER_THREAD
#ifdef CONNECTION_PER_THREAD
template<typename T>
void server_session(connection& conn, boost::asio::io_context* psocket_ioc, tcp::socket* psocket, ssl::context& ctx)
{
    auto& socket_ioc = *psocket_ioc;
    auto& socket = *psocket;

    uint64_t id = -1;
    T* wps = nullptr;

    try
    {
        boost::asio::ip::tcp::no_delay nagle(true);

        if constexpr(std::is_same_v<T, websocket::stream<boost::beast::tcp_stream>>)
        {
            wps = new T{std::move(socket)};
            wps->text(false);

            wps->next_layer().socket().set_option(nagle);
        }

        if constexpr(std::is_same_v<T, websocket::stream<ssl::stream<boost::beast::tcp_stream>>>)
        {
            wps = new T{std::move(socket), ctx};
            wps->text(false);

            wps->next_layer().next_layer().socket().set_option(nagle);

            wps->next_layer().handshake(ssl::stream_base::server);
        }

        assert(wps != nullptr);

        T& ws = *wps;

        ws.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res)
        {
            res.insert(boost::beast::http::field::sec_websocket_protocol, "binary");
        }));

        boost::beast::websocket::permessage_deflate opt;
        opt.server_enable = true;
        opt.client_enable = true;

        ws.set_option(opt);

        ws.accept();

        id = conn.id++;

        {
            std::unique_lock guard(conn.mut);

            conn.new_clients.push_back(id);
        }

        boost::beast::multi_buffer rbuffer;
        boost::beast::multi_buffer wbuffer;

        bool async_read = false;
        bool async_write = false;

        bool should_continue = false;

        std::vector<write_data>* write_queue_ptr = nullptr;
        std::mutex* write_mutex_ptr = nullptr;

        std::vector<write_data>* read_queue_ptr = nullptr;
        std::mutex* read_mutex_ptr = nullptr;

        {
            std::unique_lock guard(conn.mut);

            write_queue_ptr = &conn.directed_write_queue[id];
            write_mutex_ptr = &conn.directed_write_lock[id];

            read_queue_ptr = &conn.fine_read_queue[id];
            read_mutex_ptr = &conn.fine_read_lock[id];
        }

        std::vector<write_data>& write_queue = *write_queue_ptr;
        std::mutex& write_mutex = *write_mutex_ptr;

        std::vector<write_data>& read_queue = *read_queue_ptr;
        std::mutex& read_mutex = *read_mutex_ptr;

        while(1)
        {
            try
            {
                if(!async_write)
                {
                    std::lock_guard guard(write_mutex);

                    for(auto it = write_queue.begin(); it != write_queue.end();)
                    {
                        const write_data& next = *it;

                        if(next.id != id)
                            throw std::runtime_error("Should be impossible to have id != write id");

                        async_write = true;

                        wbuffer.consume(wbuffer.size());

                        size_t n = buffer_copy(wbuffer.prepare(next.data.size()), boost::asio::buffer(next.data));
                        wbuffer.commit(n);

                        ws.async_write(wbuffer.data(), [&](boost::system::error_code ec, std::size_t)
                                       {
                                            if(ec.failed())
                                                throw std::runtime_error(std::string("Write err ") + ec.message() + "\n");

                                            async_write = false;
                                            should_continue = true;
                                       });

                        should_continue = true;
                        write_queue.erase(it);
                        break;
                    }
                }

                if(!async_read)
                {
                    ws.async_read(rbuffer, [&](boost::system::error_code ec, std::size_t)
                                  {
                                      if(ec.failed())
                                          throw std::runtime_error(std::string("Read err ") + ec.message() + "\n");

                                      std::string next = boost::beast::buffers_to_string(rbuffer.data());

                                      std::lock_guard guard(read_mutex);

                                      write_data ndata;
                                      ndata.data = std::move(next);
                                      ndata.id = id;

                                      read_queue.push_back(ndata);

                                      rbuffer.clear();

                                      async_read = false;
                                      should_continue = true;
                                  });

                    async_read = true;
                    should_continue = true;
                }

                if(async_read || async_write)
                {
                    socket_ioc.poll();
                    socket_ioc.restart();
                }

                if(should_continue)
                {
                    should_continue = false;
                    continue;
                }
            }
            catch(std::runtime_error& e)
            {
                std::cout << "Server Thread Exception: " << e.what() << std::endl;
                break;
            }
            catch(...)
            {
                std::cout << "Server Thread Exception\n";
                break;
            }

            sf::sleep(sf::milliseconds(1));

            if(conn.should_terminate)
            {
                printf("Terminated thread\n");
                break;
            }
        }
    }
    catch(boost::system::system_error const& se)
    {
        // This indicates that the session was closed
        if(se.code() != websocket::error::closed)
            std::cerr << "Websock Session Error: " << se.code().message() << std::endl;
    }
    catch(std::exception const& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    if(wps)
    {
        delete wps;
        wps = nullptr;
    }

    {
        std::lock_guard guard(conn.disconnected_lock);
        conn.disconnected_clients.push_back(id);
    }

    delete psocket;
    delete psocket_ioc;
}

template<typename T>
void server_thread(connection& conn, std::string saddress, uint16_t port)
{
    auto const address = boost::asio::ip::make_address(saddress);

    std::atomic_bool accepted = true;
    boost::asio::io_context acceptor_context{1};

    tcp::acceptor acceptor{acceptor_context, {address, port}};
    acceptor.set_option(boost::asio::socket_base::reuse_address(true));

    ssl::context ctx{ssl::context::tls_server};

    std::string cert = read_file_bin("./deps/secret/cert/cert.crt");
    std::string dh = read_file_bin("./deps/secret/cert/dh.pem");
    std::string key = read_file_bin("./deps/secret/cert/key.pem");

    ctx.set_options(boost::asio::ssl::context::default_workarounds |
                    boost::asio::ssl::context::no_sslv2 |
                    boost::asio::ssl::context::single_dh_use |
                    boost::asio::ssl::context::no_sslv3);

    ctx.use_certificate_chain(
        boost::asio::buffer(cert.data(), cert.size()));

    ctx.use_private_key(
        boost::asio::buffer(key.data(), key.size()),
        boost::asio::ssl::context::file_format::pem);

    ctx.use_tmp_dh(
        boost::asio::buffer(dh.data(), dh.size()));

    while(1)
    {
        boost::asio::io_context* next_context = nullptr;
        tcp::socket* socket = nullptr;

        try
        {
            next_context = new boost::asio::io_context{1};
            socket = new tcp::socket{*next_context};

            acceptor.accept(*socket);
        }
        catch(...)
        {
            if(socket)
                delete socket;

            if(next_context)
                delete next_context;

            sf::sleep(sf::milliseconds(1));
            continue;
        }

        std::thread(server_session<T>, std::ref(conn), next_context, socket, std::ref(ctx)).detach();

        sf::sleep(sf::milliseconds(1));;
    }
}
#endif // CONNECTION_PER_THREAD

#define ASYNC_THREAD
#ifdef ASYNC_THREAD

template<typename T>
struct websocket_session_data
{
    T* wps = nullptr;
    uint64_t id = -1;
    tcp::socket socket;
    bool can_cancel = false;
    bool has_cancelled = false;
    ssl::context* ctx = nullptr;
    connection_settings sett;

    boost::beast::flat_buffer rbuffer;
    boost::beast::flat_buffer wbuffer;

    bool async_read = false;
    bool async_write = false;

    std::vector<write_data>* write_queue_ptr = nullptr;
    std::mutex* write_mutex_ptr = nullptr;

    std::vector<write_data>* read_queue_ptr = nullptr;
    std::mutex* read_mutex_ptr = nullptr;

    boost::system::error_code last_ec{};

    enum state
    {
        start,
        has_handshake,
        has_accept,
        read_write,
        blocked,
        err,
        terminated,
    };

    state current_state = start;

    websocket_session_data(tcp::socket&& _sock, connection_settings _sett) : socket(std::move(_sock)), sett(_sett){}

    void tick(connection& conn, std::vector<uint64_t>& wake_queue)
    {
        if(current_state == blocked)
            return;

        if(current_state == terminated)
            return;

        if(current_state == err && !async_read && !async_write)
        {
            if(wps)
            {
                delete wps;
                wps = nullptr;
            }

            printf("Got networking error %s with value %s:%i\n", last_ec.message().c_str(), last_ec.category().name(), last_ec.value());

            current_state = terminated;
            return;
        }

        ///the callbacks for both async_write and async_read both cause this to wake
        if(current_state == err && (async_write || async_read))
        {
            if(can_cancel && !has_cancelled)
            {
                boost::system::error_code ec;
                boost::beast::get_lowest_layer(*wps).socket().cancel(ec);
                has_cancelled = true;
            }

            return;
        }

    try
    {
        if(current_state == start)
        {
            current_state = blocked;
            boost::asio::ip::tcp::no_delay nagle(true);

            if constexpr(std::is_same_v<T, websocket::stream<boost::beast::tcp_stream>>)
            {
                wps = new T{std::move(socket)};
                wps->text(false);
                wps->write_buffer_bytes(8192);
                wps->read_message_max(sett.max_read_size);

                wps->next_layer().socket().set_option(nagle);
                current_state = has_handshake;
                ///don't need to awake, as we didn't sleep
            }

            if constexpr(std::is_same_v<T, websocket::stream<ssl::stream<boost::beast::tcp_stream>>>)
            {
                wps = new T{std::move(socket), *ctx};
                wps->text(false);
                wps->write_buffer_bytes(8192);
                wps->read_message_max(sett.max_read_size);

                wps->next_layer().next_layer().socket().set_option(nagle);

                wps->next_layer().async_handshake(ssl::stream_base::server, [&](auto ec)
                {
                    if(ec)
                    {
                        last_ec = ec;
                        current_state = err;
                    }
                    else
                    {
                        current_state = has_handshake;
                    }

                    wake_queue.push_back(id);
                });
            }

            can_cancel = true;
        }

        if(current_state == has_handshake)
        {
            current_state = blocked;
            assert(wps != nullptr);

            T& ws = *wps;

            ws.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res)
            {
                res.insert(boost::beast::http::field::sec_websocket_protocol, "binary");
            }));

            if(sett.enable_compression)
            {
                boost::beast::websocket::permessage_deflate opt;
                ///enabling deflate causes server memory usage to climb extremely high
                ///todo tomorrow: check if this is a real memory leak
                opt.server_enable = true;
                opt.client_enable = true;
                opt.server_max_window_bits = sett.max_window_bits;
                opt.memLevel = sett.memory_level;
                opt.compLevel = sett.compression_level;

                ws.set_option(opt);
            }

            ws.async_accept([&](auto ec)
            {
                if(ec)
                {
                    last_ec = ec;
                    current_state = err;
                    wake_queue.push_back(id);
                    return;
                }

                current_state = has_accept;

                wake_queue.push_back(id);
            });
        }

        if(current_state == has_accept)
        {
            printf("Connection %" PRIu64 " is negotiated\n", id);
            current_state = read_write;

            {
                std::unique_lock guard(conn.mut);

                write_queue_ptr = &conn.directed_write_queue[id];
                write_mutex_ptr = &conn.directed_write_lock[id];

                read_queue_ptr = &conn.fine_read_queue[id];
                read_mutex_ptr = &conn.fine_read_lock[id];
            }

            {
                std::unique_lock guard(conn.mut);

                conn.new_clients.push_back(id);
            }
            ///no need to wake
        }

        if(current_state == read_write)
        {
            T& ws = *wps;

            std::vector<write_data>& write_queue = *write_queue_ptr;
            std::mutex& write_mutex = *write_mutex_ptr;

            std::vector<write_data>& read_queue = *read_queue_ptr;
            std::mutex& read_mutex = *read_mutex_ptr;

            ///so, theoretically if we don't have any writes, according to this state machine, we'll never wake up if a read doesn't hit us
            ///the server is responsible for fixing this
            if(!async_write)
            {
                std::lock_guard guard(write_mutex);

                for(auto it = write_queue.begin(); it != write_queue.end();)
                {
                    const write_data& next = *it;

                    if(next.id != id)
                    {
                        current_state = err;
                        wake_queue.push_back(id);
                        return;
                    }


                    wbuffer.consume(wbuffer.size());

                    size_t n = buffer_copy(wbuffer.prepare(next.data.size()), boost::asio::buffer(next.data));
                    wbuffer.commit(n);

                    async_write = true;

                    ws.async_write(wbuffer.data(), [&](boost::system::error_code ec, std::size_t)
                    {
                        if(ec.failed())
                        {
                            last_ec = ec;
                            current_state = err;
                            wake_queue.push_back(id);
                            async_write = false;
                            return;
                        }

                        async_write = false;
                        wake_queue.push_back(id);
                    });

                    write_queue.erase(it);
                    break;
                }
            }

            if(!async_read)
            {
                ws.async_read(rbuffer, [&](boost::system::error_code ec, std::size_t)
                {
                    if(ec.failed())
                    {
                        last_ec = ec;
                        current_state = err;
                        wake_queue.push_back(id);
                        async_read = false;
                        return;
                    }

                    std::string next = boost::beast::buffers_to_string(rbuffer.data());

                    {
                        write_data ndata;
                        ndata.data = std::move(next);
                        ndata.id = id;

                        std::lock_guard guard(read_mutex);
                        read_queue.push_back(ndata);
                    }

                    rbuffer.clear();

                    async_read = false;
                    wake_queue.push_back(id);
                });

                async_read = true;
            }
        }
    }
    catch(...)
    {
        printf("Err in session data\n");
        last_ec = {};
        current_state = err;
        wake_queue.push_back(id);
    }
    }
};

struct acceptor_data
{
    ssl::context ctx{ssl::context::tls_server};
    boost::asio::io_context acceptor_context{1};
    tcp::acceptor acceptor;
    tcp::socket next_socket;

    bool async_in_flight = false;

    acceptor_data(const std::string& saddress, uint16_t port) :
        acceptor_context{1},
        acceptor{acceptor_context, {boost::asio::ip::make_address(saddress), port}},
        next_socket{acceptor_context}
    {
        acceptor.set_option(boost::asio::socket_base::reuse_address(true));

        std::string cert = read_file_bin("./deps/secret/cert/cert.crt");
        std::string dh = read_file_bin("./deps/secret/cert/dh.pem");
        std::string key = read_file_bin("./deps/secret/cert/key.pem");

        ctx.set_options(boost::asio::ssl::context::default_workarounds |
                        boost::asio::ssl::context::no_sslv2 |
                        boost::asio::ssl::context::single_dh_use |
                        boost::asio::ssl::context::no_sslv3);

        ctx.use_certificate_chain(
            boost::asio::buffer(cert.data(), cert.size()));

        ctx.use_private_key(
            boost::asio::buffer(key.data(), key.size()),
            boost::asio::ssl::context::file_format::pem);

        ctx.use_tmp_dh(
            boost::asio::buffer(dh.data(), dh.size()));
    }

    std::optional<tcp::socket> get_next_socket()
    {
        std::optional<tcp::socket> ret;

        if(!async_in_flight)
        {
            async_in_flight = true;

            acceptor.async_accept(next_socket, [&](auto ec)
            {
                if(ec)
                {
                    std::cout << "Error in async accept " << ec.message() << std::endl;
                }
                else
                {
                    ret = std::move(next_socket);
                }

                next_socket = tcp::socket{acceptor_context};
                async_in_flight = false;
            });
        }

        acceptor_context.poll();

        if(acceptor_context.stopped())
            acceptor_context.restart();

        return ret;
    }
};

template<typename T>
void server_thread(connection& conn, std::string saddress, uint16_t port, connection_settings sett)
{
    acceptor_data acceptor_ctx(saddress, port);

    ///owning, individual elements are recycled
    std::vector<websocket_session_data<T>*> all_session_data;

    std::vector<uint64_t> wake_queue;
    std::vector<uint64_t> next_wake_queue;

    uint64_t tick = 0;

    while(1)
    {
        tick++;

        auto sock_opt = acceptor_ctx.get_next_socket();

        if(sock_opt.has_value())
        {
            uint64_t id = -1;

            {
                std::lock_guard guard(conn.free_id_queue_lock);

                if(conn.free_id_queue.size() == 0)
                {
                    id = conn.id++;
                    all_session_data.push_back(nullptr);
                }
                else
                {
                    id = conn.free_id_queue.back();
                    conn.free_id_queue.pop_back();
                }
            }

            websocket_session_data<T>* dat = new websocket_session_data<T>(std::move(sock_opt.value()), sett);

            all_session_data[id] = dat;

            dat->id = id;
            dat->ctx = &acceptor_ctx.ctx;

            wake_queue.push_back(id);
        }

        {
            std::lock_guard guard(conn.wake_lock);

            wake_queue.insert(wake_queue.end(), conn.wake_queue.begin(), conn.wake_queue.end());
            conn.wake_queue.clear();

            wake_queue.insert(wake_queue.end(), next_wake_queue.begin(), next_wake_queue.end());
            next_wake_queue.clear();
        }

        if((tick % 100) == 0)
        {
            ///don't hold this lock so long!
            std::lock_guard guard(conn.force_disconnection_lock);

            for(auto i : conn.force_disconnection_queue)
            {
                if(i >= all_session_data.size())
                    continue;

                websocket_session_data<T>* pdata = all_session_data[i];

                ///if we're not terminated, and we're not already in an error, transition
                if(pdata->current_state != websocket_session_data<T>::err && pdata->current_state != websocket_session_data<T>::terminated)
                {
                    pdata->current_state = websocket_session_data<T>::err;
                    wake_queue.push_back(i);
                }
            }

            conn.force_disconnection_queue.clear();
        }

        for(auto& id : wake_queue)
        {
            if(id >= all_session_data.size())
                continue;

            websocket_session_data<T>* pdata = all_session_data[id];

            ///spurious wakeup
            if(pdata == nullptr)
                continue;

            ///doesn't matter if we get a spurious wakeup
            pdata->tick(conn, next_wake_queue);

            if(pdata->current_state == websocket_session_data<T>::terminated)
            {
                delete pdata;
                all_session_data[id] = nullptr;

                ///only now once memory has been freed, is the host informed that the client is disconnected
                std::lock_guard guard(conn.disconnected_lock);
                conn.disconnected_clients.push_back(id);

                {
                    std::lock_guard guard(conn.mut);

                    for(int i=0; i < (int)conn.new_clients.size(); i++)
                    {
                        if(conn.new_clients[i] == id)
                        {
                            conn.new_clients.erase(conn.new_clients.begin() + i);
                            i--;
                            continue;
                        }
                    }
                }
            }
        }

        if(wake_queue.size() == 0 && next_wake_queue.size() == 0)
            sf::sleep(sf::milliseconds(1));

        wake_queue.clear();
    }
}
#endif // ASYNC_THREAD

template<typename T>
void client_thread(connection& conn, std::string address, uint16_t port, std::string sni_hostname, uint64_t client_sleep_time_ms)
{
    T* wps = nullptr;
    boost::asio::io_context ioc;
    ssl::context ctx{ssl::context::tls_client};

    ctx.set_options(ssl::context::no_sslv2);
    ctx.set_options(ssl::context::no_sslv3);
    ctx.set_options(ssl::context::no_tlsv1);
    ctx.set_options(ssl::context::no_tlsv1_1);
    ctx.set_options(ssl::context::no_tlsv1_2);

    conn.connection_in_progress = true;

    try
    {
        boost::asio::ip::tcp::no_delay nagle(true);

        tcp::resolver resolver{ioc};

        auto const results = resolver.resolve(address, std::to_string(port));

        if constexpr(std::is_same_v<T, websocket::stream<tcp::socket>>)
        {
            wps = new T{ioc};
            wps->text(false);

            boost::asio::connect(wps->next_layer(), results.begin(), results.end());

            wps->next_layer().set_option(nagle);
        }

        if constexpr(std::is_same_v<T, websocket::stream<ssl::stream<tcp::socket>>>)
        {
            /*ctx.set_options(boost::asio::ssl::context::default_workarounds |
                            boost::asio::ssl::context::no_sslv2 |
                            boost::asio::ssl::context::single_dh_use |
                            boost::asio::ssl::context::no_sslv3);*/

            wps = new T{ioc, ctx};
            wps->text(false);

            if(sni_hostname.size() > 0)
            {
                if(!SSL_set_tlsext_host_name(wps->next_layer().native_handle(), sni_hostname.c_str()))
                {
                    boost::system::error_code ec{static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category()};
                    throw boost::system::system_error{ec};
                }
            }

            boost::asio::connect(wps->next_layer().next_layer(), results.begin(), results.end());

            wps->next_layer().next_layer().set_option(nagle);
            wps->next_layer().handshake(ssl::stream_base::client);
        }

        assert(wps != nullptr);

        T& ws = *wps;

        boost::beast::websocket::permessage_deflate opt;
        opt.server_enable = true;
        opt.client_enable = true;

        ws.set_option(opt);

        ws.handshake(address, "/");


        boost::beast::multi_buffer rbuffer;
        boost::beast::multi_buffer wbuffer;

        bool async_write = false;
        bool async_read = false;

        bool should_continue = false;

        std::vector<write_data>* write_queue_ptr = nullptr;
        std::mutex* write_mutex_ptr = nullptr;

        std::vector<write_data>* read_queue_ptr = nullptr;
        std::mutex* read_mutex_ptr = nullptr;

        {
            std::unique_lock guard(conn.mut);

            write_queue_ptr = &conn.directed_write_queue[-1];
            write_mutex_ptr = &conn.directed_write_lock[-1];

            read_queue_ptr = &conn.fine_read_queue[-1];
            read_mutex_ptr = &conn.fine_read_lock[-1];
        }

        std::vector<write_data>& write_queue = *write_queue_ptr;
        std::mutex& write_mutex = *write_mutex_ptr;

        std::vector<write_data>& read_queue = *read_queue_ptr;
        std::mutex& read_mutex = *read_mutex_ptr;

        conn.client_connected_to_server = 1;
        conn.connection_in_progress = false;

        while(1)
        {
            {
                std::lock_guard guard(conn.wake_lock);
                conn.wake_queue.clear();
            }

            try
            {
                if(!async_write)
                {
                    std::lock_guard guard(write_mutex);

                    while(write_queue.size() > 0)
                    {
                        const write_data& next = write_queue.front();

                        wbuffer.consume(wbuffer.size());

                        size_t n = buffer_copy(wbuffer.prepare(next.data.size()), boost::asio::buffer(next.data));
                        wbuffer.commit(n);

                        write_queue.erase(write_queue.begin());

                        ws.async_write(wbuffer.data(), [&](boost::system::error_code ec, std::size_t)
                                       {
                                            if(ec.failed())
                                                throw std::runtime_error("Write err\n");

                                            async_write = false;
                                            should_continue = true;
                                       });

                        async_write = true;
                        should_continue = true;
                        break;
                    }
                }

                if(!async_read)
                {
                    ws.async_read(rbuffer, [&](boost::system::error_code ec, std::size_t)
                                  {
                                      if(ec.failed())
                                          throw std::runtime_error("Read err\n");

                                      std::string next = boost::beast::buffers_to_string(rbuffer.data());

                                      std::lock_guard guard(read_mutex);

                                      write_data ndata;
                                      ndata.data = std::move(next);
                                      ndata.id = -1;

                                      read_queue.push_back(ndata);

                                      rbuffer.clear();

                                      async_read = false;
                                      should_continue = true;
                                  });

                    async_read = true;
                    should_continue = true;
                }
            }
            catch(...)
            {
                std::cout << "exception in client write\n";
            }

            if(async_read || async_write)
            {
                ioc.poll();
                ioc.restart();
            }

            if(should_continue)
            {
                should_continue = false;
                continue;
            }

            sf::sleep(sf::milliseconds(client_sleep_time_ms));

            if(conn.should_terminate)
                break;
        }
    }
    catch(std::exception& e)
    {
        std::cout << "exception in client write outer " << e.what() << std::endl;
    }

    {
        std::unique_lock guard(conn.mut);

        {
            std::lock_guard g2(conn.directed_write_lock[-1]);

            conn.directed_write_queue.clear();
        }

        {
            std::lock_guard g3(conn.fine_read_lock[-1]);

            conn.fine_read_queue.clear();
        }
    }

    if(wps)
    {
        delete wps;
        wps = nullptr;
    }

    conn.client_connected_to_server = 0;
    conn.connection_in_progress = false;
}
}
#endif // __EMSCRIPTEN__

#ifdef __EMSCRIPTEN__

#include <emscripten/emscripten.h>

namespace
{

bool sock_readable(int fd)
{
    fd_set fds;
    struct timeval tmo;

    tmo.tv_sec=0;
    tmo.tv_usec=0;

    FD_ZERO(&fds);
    FD_SET((uint32_t)fd, &fds);

    select(fd+1, &fds, NULL, NULL, &tmo);

    return FD_ISSET((uint32_t)fd, &fds);
}

bool sock_writable(int fd, long seconds = 0, long milliseconds = 0)
{
    fd_set fds;
    struct timeval tmo;

    tmo.tv_sec=seconds;
    tmo.tv_usec=milliseconds;

    FD_ZERO(&fds);
    FD_SET((uint32_t)fd, &fds);

    select(fd+1, NULL, &fds, NULL, &tmo);

    return FD_ISSET((uint32_t)fd, &fds);
}

std::optional<std::string> tick_tcp_sender(int sock,
                                           std::vector<write_data>& write_queue, std::mutex& write_mutex,
                                           std::vector<write_data>& read_queue, std::mutex& read_mutex,
                                           int MAXDATASIZE,
                                           char buf[])
{
    {
        std::lock_guard guard(write_mutex);

        while(write_queue.size() > 0 && sock_writable(sock))
        {
            write_data& next = write_queue.front();

            std::string& to_send = next.data;

            int num = send(sock, to_send.data(), to_send.size(), 0);

            if(num < 0)
                return "Broken write";

            if(num == to_send.size())
            {
                write_queue.erase(write_queue.begin());
                continue;
            }

            if(num < to_send.size())
            {
                to_send = std::string(to_send.begin() + num, to_send.end());
                break;
            }
        }
    }

    {
        while(sock_readable(sock))
        {
            int num = -1;

            if((num = recv(sock, buf, MAXDATASIZE-1, 0)) == -1)
                return "SOCK BROKEN";

            buf[num] = '\0';

            std::string ret(buf, buf + num);

            std::lock_guard guard(read_mutex);

            write_data ndata;
            ndata.data = std::move(ret);
            ndata.id = -1;

            read_queue.push_back(ndata);
        }
    }

    return std::nullopt;
}


void client_thread_tcp(connection& conn, std::string address, uint16_t port)
{
    printf("In thread?\n");

    int sock = -1;

    try
    {
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        if(sock == -1)
        {
            printf("Socket error, server down? %i\n", sock);
            throw std::runtime_error("Sock err 1");
        }

        fcntl(sock, F_SETFL, O_NONBLOCK);

        int flag = 1;
        int result = setsockopt(sock,            /* socket affected */
                                IPPROTO_TCP,     /* set option at TCP level */
                                TCP_NODELAY,     /* name of option */
                                (char *) &flag,  /* the cast is historical cruft */
                                sizeof(int));    /* length of option value */

        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));

        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        printf("Pre inet\n");

        inet_pton(AF_INET, address.c_str(), &addr.sin_addr);

        printf("Post inet\n");

        conn.connection_in_progress = true;

        int connect_err = connect(sock, (sockaddr*)&addr, sizeof(addr));

        if(connect_err == -1)
        {
            if(errno == EINPROGRESS)
            {
                printf("INPROGRESS\n");

                fd_set sockets;
                FD_ZERO(&sockets);
                FD_SET((uint32_t)sock, &sockets);

                /*steady_timer timer;

                while(select((uint32_t)sock + 1, nullptr, &sockets, nullptr, nullptr) <= 0)
                {
                    if(timer.get_elapsed_time_s() > 10)
                        throw std::runtime_error("Timed out");

                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }

                printf("Connected\n");*/
            }
            else
            {
                printf("Socket error, server down (2) %i\n", connect_err);
                throw std::runtime_error("Sock err 2");
            }
        }

        conn.client_connected_to_server = 1;

        conn.connection_in_progress = false;

        printf("Fin\n");

        std::vector<write_data>* write_queue_ptr = nullptr;
        std::mutex* write_mutex_ptr = nullptr;

        std::vector<write_data>* read_queue_ptr = nullptr;
        std::mutex* read_mutex_ptr = nullptr;

        {
            std::unique_lock guard(conn.mut);

            write_queue_ptr = &conn.directed_write_queue[-1];
            write_mutex_ptr = &conn.directed_write_lock[-1];

            read_queue_ptr = &conn.fine_read_queue[-1];
            read_mutex_ptr = &conn.fine_read_lock[-1];
        }

        std::vector<write_data>& write_queue = *write_queue_ptr;
        std::mutex& write_mutex = *write_mutex_ptr;

        std::vector<write_data>& read_queue = *read_queue_ptr;
        std::mutex& read_mutex = *read_mutex_ptr;

        constexpr int MAXDATASIZE = 100000;
        char buf[MAXDATASIZE] = {};

        while(1)
        {
            auto error_opt = tick_tcp_sender(sock, write_queue, write_mutex, read_queue, read_mutex, MAXDATASIZE, buf);

            if(error_opt.has_value())
            {
                std::cout << "Exception in tcp send " << error_opt.value() << std::endl;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }

    }
    catch(std::exception& e)
    {
        std::cout << "exception in emscripten tcp write " << e.what() << std::endl;
    }

    conn.connection_in_progress = false;

    if(sock != -1)
        close(sock);

    {
        std::unique_lock guard(conn.mut);

        {
            std::lock_guard g2(conn.directed_write_lock[-1]);

            conn.directed_write_queue.clear();
        }

        {
            std::lock_guard g3(conn.fine_read_lock[-1]);

            conn.fine_read_queue.clear();
        }
    }

    conn.client_connected_to_server = 0;
}
}
#endif // __EMSCRIPTEN__

template<typename T>
void server_http_thread(connection& conn, const std::string& address, uint16_t port, connection_settings sett)
{

}

/*std::optional<write_data> connection_received_data::get_next_read()
{
    for(auto& i : read_queue)
    {
        if(i.first <= last_read_from)
            continue;

        if(i.second.size() > 0)
        {
            last_read_from = i.first;
            return i.second.front();
        }
    }

    ///nobody suitable available, check if we have a read available from anyone at all
    ///std::map is sorted so we'll read from lowest id person in the queue
    for(auto& i : read_queue)
    {
        if(i.second.size() > 0)
        {
            last_read_from = i.first;
            return i.second.front();
        }
    }

    return std::nullopt;
}*/

bool connection::connection_pending()
{
    return connection_in_progress;
}

#ifndef __EMSCRIPTEN__
void connection::host(const std::string& address, uint16_t port, connection_type::type type, connection_settings _sett)
{
    sett = _sett;

    is_client = false;

    #ifdef SUPPORT_NO_SSL_SERVER
    if(type == connection_type::PLAIN)
        thrd.emplace_back(server_thread<websocket::stream<boost::beast::tcp_stream>>, std::ref(*this), address, port, sett);
    #endif // SUPPORT_NO_SSL_SERVER

    if(type == connection_type::SSL)
        thrd.emplace_back(server_thread<websocket::stream<ssl::stream<boost::beast::tcp_stream>>>, std::ref(*this), address, port, sett);
}
#endif // __EMSCRIPTEN__

void connection::connect(const std::string& address, uint16_t port, connection_type::type type, std::string sni_hostname)
{
    is_client = true;

    #ifndef SERVER_ONLY

    #ifndef __EMSCRIPTEN__
    #ifdef SUPPORT_NO_SSL_CLIENT
    if(type == connection_type::PLAIN)
        thrd.emplace_back(client_thread<websocket::stream<tcp::socket>>, std::ref(*this), address, port, sni_hostname, client_sleep_interval_ms);
    #endif // SUPPORT_NO_SSL_CLIENT

    if(type == connection_type::SSL)
        thrd.emplace_back(client_thread<websocket::stream<ssl::stream<tcp::socket>>>, std::ref(*this), address, port, sni_hostname, client_sleep_interval_ms);
    #else
    ///-s WEBSOCKET_URL=wss://
    if(type != connection_type::EMSCRIPTEN_AUTOMATIC)
        throw std::runtime_error("emscripten uses compiler options for secure vs non secure websockets");

    thrd.emplace_back(client_thread_tcp, std::ref(*this), address, port);
    #endif
    #endif // SERVER_ONLY
}

template<typename T>
inline
void conditional_erase(T& in, int id)
{
    auto it = in.find(id);

    if(it == in.end())
        return;

    in.erase(it);
}

void connection::receive_bulk(connection_received_data& in)
{
    in = connection_received_data();

    {
        std::scoped_lock guard(mut);

        in.new_clients = std::move(new_clients);

        new_clients.clear();
    }

    {
        {
            std::scoped_lock guard(disconnected_lock);

            in.disconnected_clients = std::move(disconnected_clients);

            disconnected_clients.clear();

            for(uint64_t id : in.disconnected_clients)
            {
                std::scoped_lock guard(mut);

                conditional_erase(directed_write_queue, id);
                conditional_erase(directed_write_lock, id);
                conditional_erase(fine_read_queue, id);
                conditional_erase(fine_read_lock, id);
            }
        }

        {
            std::lock_guard guard(force_disconnection_lock);

            for(uint64_t id : in.disconnected_clients)
            {
                auto it = force_disconnection_queue.find(id);

                if(it != force_disconnection_queue.end())
                    force_disconnection_queue.erase(it);
            }
        }

        {
            std::lock_guard guard(free_id_queue_lock);

            for(uint64_t id : in.disconnected_clients)
            {
                free_id_queue.push_back(id);
            }
        }
    }

    {
        std::scoped_lock guard(mut);

        for(auto& i : fine_read_queue)
        {
            std::lock_guard g2(fine_read_lock[i.first]);

            in.read_queue[i.first] = std::move(i.second);

            i.second.clear();
        }
    }
}

void connection::write(const std::string& data)
{
    write_data ndata;
    ndata.data = data;
    ndata.id = -1;

    write_to(ndata);
}

bool connection::has_read()
{
    std::scoped_lock guard(mut);

    for(auto& i : fine_read_queue)
    {
        std::lock_guard g2(fine_read_lock[i.first]);

        if(i.second.size() > 0)
            return true;
    }

    return false;
}

write_data connection::read_from()
{
    /*std::lock_guard guard(mut);

    if(read_queue.size() == 0)
        throw std::runtime_error("Bad queue");

    return read_queue.front();*/

    ///there's a version of this function that could be written
    ///where mut is not held all the time

    std::scoped_lock guard(mut);

    ///check through queue, basically round robins people based on ids
    for(auto& i : fine_read_queue)
    {
        if(i.first <= last_read_from)
            continue;

        std::lock_guard g2(fine_read_lock[i.first]);

        if(i.second.size() > 0)
        {
            last_read_from = i.first;
            return i.second.front();
        }
    }

    ///nobody suitable available, check if we have a read available from anyone at all
    ///std::map is sorted so we'll read from lowest id person in the queue
    for(auto& i : fine_read_queue)
    {
        std::lock_guard g2(fine_read_lock[i.first]);

        if(i.second.size() > 0)
        {
            last_read_from = i.first;
            return i.second.front();
        }
    }

    throw std::runtime_error("Bad queue");
}

void connection::pop_read(uint64_t to_id)
{
    std::vector<write_data>* read_ptr = nullptr;
    std::mutex* mut_ptr = nullptr;

    {
        std::unique_lock guard(mut);

        read_ptr = &fine_read_queue[to_id];
        mut_ptr = &fine_read_lock[to_id];
    }

    std::lock_guard guard(*mut_ptr);

    if(read_ptr->size() == 0)
        throw std::runtime_error("Bad queue");

    read_ptr->erase(read_ptr->begin());

    /*if(read_queue.size() == 0)
        throw std::runtime_error("Bad queue");

    read_queue.erase(read_queue.begin());*/
}

void connection::force_disconnect(uint64_t id) noexcept
{
    std::scoped_lock guard(force_disconnection_lock);

    force_disconnection_queue.insert(id);
}

void connection::set_client_sleep_interval(uint64_t time_ms)
{
    client_sleep_interval_ms = time_ms;
}

void connection::write_to(const write_data& data)
{
    if(data.data.size() > sett.max_write_size)
        throw std::runtime_error("Exceeded max write size to client " + std::to_string(data.id) + " with size " + std::to_string(data.data.size()) + ". Max size is " + std::to_string(sett.max_write_size));

    {
        std::vector<write_data>* write_dat = nullptr;
        std::mutex* write_mutex = nullptr;

        {
            std::unique_lock guard(mut);

            write_dat = &directed_write_queue[data.id];
            write_mutex = &directed_write_lock[data.id];
        }

        std::lock_guard guard(*write_mutex);

        write_dat->push_back(data);
    }

    {
        std::lock_guard guard(wake_lock);
        wake_queue.push_back(data.id);
    }
}

std::optional<uint64_t> connection::has_new_client()
{
    std::scoped_lock guard(mut);

    for(auto& i : new_clients)
    {
        return i;
    }

    return std::nullopt;
}

std::optional<uint64_t> connection::has_disconnected_client()
{
    std::lock_guard guard(disconnected_lock);

    for(auto& i : disconnected_clients)
    {
        return i;
    }

    return std::nullopt;
}

void connection::pop_disconnected_client()
{
    int id;

    {
        std::lock_guard guard(disconnected_lock);

        if(disconnected_clients.size() == 0)
            throw std::runtime_error("No disconnected clients");

        id = *disconnected_clients.begin();

        {
            std::scoped_lock guard(mut);

            conditional_erase(directed_write_queue, id);
            conditional_erase(directed_write_lock, id);
            conditional_erase(fine_read_queue, id);
            conditional_erase(fine_read_lock, id);
        }

        disconnected_clients.erase(disconnected_clients.begin());
    }

    {
        std::lock_guard guard(force_disconnection_lock);

        auto it = force_disconnection_queue.find(id);

        if(it != force_disconnection_queue.end())
            force_disconnection_queue.erase(it);
    }

    {
        std::lock_guard guard(free_id_queue_lock);
        free_id_queue.push_back(id);
    }
}

void connection::pop_new_client()
{
    std::unique_lock guard(mut);

    if(new_clients.size() > 0)
    {
        new_clients.erase(new_clients.begin());
    }
}
