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
#include <boost/beast/http.hpp>
#include <boost/beast/http/span_body.hpp>
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

template<typename T>
inline
void conditional_erase(T& in, int id)
{
    auto it = in.find(id);

    if(it == in.end())
        return;

    in.erase(it);
}

template<typename T>
void move_append(T& dest, T&& source)
{
    if(dest.empty())
        dest = std::move(source);
    else
        dest.insert(dest.end(),
                    std::make_move_iterator(source.begin()),
                    std::make_move_iterator(source.end()));
}

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

            write_queue_ptr = &conn.directed_websocket_write_queue[id];
            write_mutex_ptr = &conn.directed_write_lock[id];

            read_queue_ptr = &conn.fine_websocket_read_queue[id];
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

struct session_data
{
    //session_data(tcp::socket&& _sock, connection_settings _sett) : socket(std::move(_sock)), sett(_sett){}

    virtual bool can_terminate(){return false;}
    virtual bool has_terminated(){return false;}
    virtual bool has_errored(){return false;}
    virtual void terminate(){}
    virtual session_data* tick(connection& conn, std::vector<uint64_t>& wake_queue,
                               std::map<uint64_t, connection_queue_type<write_data>>& websocket_write_queue,
                               std::map<uint64_t, connection_queue_type<http_write_info>>& http_write_queue,
                               std::map<uint64_t, std::vector<write_data>>& websocket_read_queue,
                               std::map<uint64_t, std::vector<http_read_info>>& http_read_queue) {return nullptr;}
    virtual bool perform_upgrade(){return false;}

    virtual ~session_data(){}
};

template<typename T>
struct websocket_session_data : session_data
{
    uint64_t id = -1;
    //tcp::socket socket;
    connection_settings sett;
    websocket::stream<T> ws;

    bool can_cancel = false;
    bool has_cancelled = false;

    boost::beast::flat_buffer rbuffer;
    boost::beast::flat_buffer wbuffer;

    bool async_read = false;
    bool async_write = false;

    connection_queue_type<write_data>* write_queue_ptr = nullptr;
    std::vector<write_data>* read_queue_ptr = nullptr;

    boost::system::error_code last_ec{};

    enum state
    {
        start,
        has_accept,
        read_write,
        blocked,
        err,
        terminated,
    };

    state current_state = start;

    websocket_session_data(T&& _stream, connection_settings _sett) : sett(_sett), ws(std::move(_stream))
    {

    }

    virtual bool can_terminate() override
    {
        return current_state != state::err && current_state != state::terminated;
    }

    virtual bool has_terminated() override
    {
        return current_state == state::terminated;
    }

    virtual bool has_errored() override
    {
        return current_state == state::err;
    }

    virtual void terminate() override
    {
        current_state = err;
    }

    void create_stream()
    {
        ws.text(false);
        ws.write_buffer_bytes(8192);
        ws.read_message_max(sett.max_read_size);

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
    }

    template<typename U>
    void perform_upgrade_from_accept(U req, std::vector<uint64_t>& wake_queue)
    {
        create_stream();

        can_cancel = true;
        current_state = blocked;

        ws.async_accept(req,
        [&](auto ec)
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

    virtual session_data* tick(connection& conn, std::vector<uint64_t>& wake_queue,
                               std::map<uint64_t, connection_queue_type<write_data>>& websocket_write_queue,
                               std::map<uint64_t, connection_queue_type<http_write_info>>& http_write_queue,
                               std::map<uint64_t, std::vector<write_data>>& websocket_read_queue,
                               std::map<uint64_t, std::vector<http_read_info>>& http_read_queue) override
    {
        if(current_state == blocked)
            return nullptr;

        if(current_state == terminated)
            return nullptr;

        if(current_state == err && !async_read && !async_write)
        {
            printf("Got networking websockets error %s with value %s:%i\n", last_ec.message().c_str(), last_ec.category().name(), last_ec.value());

            current_state = terminated;
            return nullptr;
        }

        ///the callbacks for both async_write and async_read both cause this to wake
        if(current_state == err && (async_write || async_read))
        {
            if(can_cancel && !has_cancelled)
            {
                boost::system::error_code ec;
                boost::beast::get_lowest_layer(ws).socket().cancel(ec);
                has_cancelled = true;
            }

            return nullptr;
        }

    try
    {
        if(current_state == start)
        {
            create_stream();

            can_cancel = true;
            current_state = blocked;

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

            write_queue_ptr = &websocket_write_queue[id];
            read_queue_ptr = &websocket_read_queue[id];

            {
                std::unique_lock guard(conn.mut);
                conn.new_clients.push_back(id);
            }
            ///no need to wake
        }

        if(current_state == read_write)
        {
            ///so, theoretically if we don't have any writes, according to this state machine, we'll never wake up if a read doesn't hit us
            ///the server is responsible for fixing this
            if(!async_write)
            {
                connection_queue_type<write_data>& write_queue = *write_queue_ptr;

                for(auto it = write_queue.begin(); it != write_queue.end();)
                {
                    const write_data& next = *it;

                    if(next.id != id)
                    {
                        current_state = err;
                        wake_queue.push_back(id);
                        return nullptr;
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

                    write_queue.pop_front();
                    break;
                }
            }

            if(!async_read)
            {
                std::vector<write_data>& read_queue = *read_queue_ptr;

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

                        read_queue.push_back(std::move(ndata));
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

        return nullptr;
    }
};

template<typename T>
struct http_session_data : session_data
{
    uint64_t id = -1;
    connection_settings sett;

    T stream;

    bool can_cancel = false;
    bool has_cancelled = false;

    boost::beast::flat_buffer rbuffer;
    boost::beast::flat_buffer wbuffer;

    bool async_read = false;
    bool async_write = false;

    connection_queue_type<http_write_info>* write_queue_ptr = nullptr;
    std::vector<http_read_info>* read_queue_ptr = nullptr;

    boost::system::error_code last_ec{};

    std::optional<boost::beast::http::request_parser<boost::beast::http::string_body>> parser;

    enum state
    {
        start,
        has_handshake,
        read_write,
        blocked,
        err,
        terminated,
        upgrade,
    };

    state current_state = start;

    boost::beast::http::response<boost::beast::http::span_body<char>> response_storage;

    http_session_data(tcp::socket&& _sock, connection_settings _sett, ssl::context& ctx) requires std::is_same_v<T, boost::beast::tcp_stream> : sett(_sett), stream(std::move(_sock)) {}
    http_session_data(tcp::socket&& _sock, connection_settings _sett, ssl::context& ctx) requires std::is_same_v<T, ssl::stream<boost::beast::tcp_stream>> : sett(_sett), stream(std::move(_sock), ctx) {}

    virtual bool can_terminate() override
    {
        return current_state != state::err && current_state != state::terminated;
    }

    virtual bool has_terminated() override
    {
        return current_state == state::terminated;
    }

    virtual bool has_errored() override
    {
        return current_state == state::err;
    }

    virtual void terminate() override
    {
        current_state = err;
    }

    virtual bool perform_upgrade() override
    {
        return current_state == upgrade;
    }

    auto get_base_response(http_write_info::status_code code)
    {
        boost::beast::http::response<boost::beast::http::span_body<char>> response;

        response.version(11);

        if(code == http_write_info::status_code::ok)
        {
            response.result(boost::beast::http::status::ok);
        }
        else if(code == http_write_info::status_code::bad_request)
        {
            response.result(boost::beast::http::status::bad_request);
        }
        else if(code == http_write_info::status_code::not_found)
        {
            response.result(boost::beast::http::status::not_found);
        }
        else
        {
            response.result(boost::beast::http::status::bad_request);
        }

        response.set(boost::beast::http::field::server, "net_code_");
        response.set(boost::beast::http::field::content_type, "text/plain");

        response.keep_alive(false);

        return response;
    }

    virtual session_data* tick(connection& conn, std::vector<uint64_t>& wake_queue,
                               std::map<uint64_t, connection_queue_type<write_data>>& websocket_write_queue,
                               std::map<uint64_t, connection_queue_type<http_write_info>>& http_write_queue,
                               std::map<uint64_t, std::vector<write_data>>& websocket_read_queue,
                               std::map<uint64_t, std::vector<http_read_info>>& http_read_queue) override
    {
        if(current_state == blocked)
            return nullptr;

        if(current_state == terminated)
            return nullptr;

        if(current_state == err && !async_read && !async_write)
        {
            //printf("Got networking error %s with value %s:%i\n", last_ec.message().c_str(), last_ec.category().name(), last_ec.value());

            current_state = terminated;
            return nullptr;
        }

        ///the callbacks for both async_write and async_read both cause this to wake
        if(current_state == err && (async_write || async_read))
        {
            if(can_cancel && !has_cancelled)
            {
                boost::system::error_code ec;
                boost::beast::get_lowest_layer(stream).socket().cancel(ec);
                has_cancelled = true;
            }

            return nullptr;
        }

        if(current_state == upgrade)
        {
            has_cancelled = true;
            current_state = terminated;

            boost::beast::get_lowest_layer(stream).expires_never();

            websocket_session_data<T>* next_session = new websocket_session_data<T>(std::move(stream), sett);
            next_session->id = id;

            next_session->perform_upgrade_from_accept(parser->release(), wake_queue);

            rbuffer.clear();
            wake_queue.push_back(id);
            async_read = false;

            return next_session;
        }

    try
    {
        if(current_state == start)
        {
            current_state = blocked;
            boost::asio::ip::tcp::no_delay nagle(true);

            if constexpr(std::is_same_v<T, boost::beast::tcp_stream>)
            {
                stream.expires_after(std::chrono::seconds(15));

                stream.socket().set_option(nagle);
                current_state = has_handshake;
                ///don't need to awake, as we didn't sleep
            }

            if constexpr(std::is_same_v<T, ssl::stream<boost::beast::tcp_stream>>)
            {
                stream.next_layer().expires_after(std::chrono::seconds(15));
                stream.next_layer().socket().set_option(nagle);

                stream.async_handshake(ssl::stream_base::server, [&](auto ec)
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
            //printf("Connection %" PRIu64 " is negotiated\n", id);
            current_state = read_write;

            write_queue_ptr = &http_write_queue[id];
            read_queue_ptr = &http_read_queue[id];

            {
                std::unique_lock guard(conn.mut);
                conn.new_http_clients.push_back(id);
            }
            ///no need to wake
        }

        if(current_state == read_write)
        {
            T& ws = stream;

            connection_queue_type<http_write_info>& write_queue = *write_queue_ptr;
            std::vector<http_read_info>& read_queue = *read_queue_ptr;

            ///so, theoretically if we don't have any writes, according to this state machine, we'll never wake up if a read doesn't hit us
            ///the server is responsible for fixing this
            if(!async_write)
            {
                for(auto it = write_queue.begin(); it != write_queue.end();)
                {
                    http_write_info& next = *it;

                    if(next.id != id)
                    {
                        current_state = err;
                        wake_queue.push_back(id);
                        return nullptr;
                    }

                    async_write = true;

                    response_storage = get_base_response(next.code);

                    response_storage.keep_alive(next.keep_alive);

                    response_storage.body() = boost::beast::http::span_body<char>::value_type((char*)next.body.data(), next.body.size());
                    response_storage.content_length(response_storage.body().size());

                    response_storage.set(boost::beast::http::field::content_type, next.mime_type);

                    boost::beast::http::async_write(stream, response_storage, [&](boost::system::error_code ec, std::size_t)
                    {
                        write_queue.pop_front();

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

                    break;
                }
            }

            if(!async_read)
            {
                parser.emplace();

                boost::beast::http::async_read(ws, rbuffer, *parser, [&](boost::system::error_code ec, std::size_t)
                {
                    if(ec.failed())
                    {
                        last_ec = ec;
                        current_state = err;
                        wake_queue.push_back(id);
                        async_read = false;
                        return;
                    }

                    if(boost::beast::websocket::is_upgrade(parser->get()))
                    {
                        current_state = upgrade;

                        ///not sure if i can clear rbuffer for parser
                        async_read = false;
                        wake_queue.push_back(id);
                        return;
                    }
                    else
                    {
                        const auto& req = parser->get();

                        if(req.method() == boost::beast::http::verb::get)
                        {
                            auto boost_string_view = req.target();

                            std::string_view target(boost_string_view.data(), boost_string_view.size());

                            http_read_info ndata;
                            ndata.path = std::string(target);
                            ndata.keep_alive = req.keep_alive();

                            read_queue.push_back(std::move(ndata));
                        }
                        else
                        {
                            printf("Method not supported\n");
                        }
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

        return nullptr;
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
    std::vector<session_data*> all_session_data;

    std::vector<uint64_t> wake_queue;
    std::vector<uint64_t> next_wake_queue;

    ///todo: erasing these buffers
    std::map<uint64_t, connection_queue_type<write_data>> websocket_write_queue;
    std::map<uint64_t, connection_queue_type<http_write_info>> http_write_queue;

    std::map<uint64_t, std::vector<write_data>> websocket_read_queue;
    std::map<uint64_t, std::vector<http_read_info>> http_read_queue;

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

            http_session_data<T>* dat = new http_session_data<T>(std::move(sock_opt.value()), sett, acceptor_ctx.ctx);
            //websocket_session_data<T>* dat = new websocket_session_data<T>(std::move(sock_opt.value()), sett);

            all_session_data[id] = dat;

            dat->id = id;

            wake_queue.push_back(id);
        }

        {
            std::lock_guard guard(conn.fat_readwrite_mutex);

            for(auto& i : conn.pending_websocket_write_queue)
            {
                move_append(websocket_write_queue[i.first], std::move(i.second));
            }

            for(auto& i : conn.pending_http_write_queue)
            {
                move_append(http_write_queue[i.first], std::move(i.second));
            }

            for(auto& i : websocket_read_queue)
            {
                move_append(conn.pending_websocket_read_queue[i.first], std::move(i.second));

                i.second.clear();
            }

            for(auto& i : http_read_queue)
            {
                move_append(conn.pending_http_read_queue[i.first], std::move(i.second));

                i.second.clear();
            }

            conn.pending_websocket_write_queue.clear();
            conn.pending_http_write_queue.clear();
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

                session_data* pdata = all_session_data[i];

                ///if we're not terminated, and we're not already in an error, transition
                if(pdata->can_terminate())
                {
                    pdata->terminate();
                    wake_queue.push_back(i);
                }
            }

            conn.force_disconnection_queue.clear();
        }

        for(auto& id : wake_queue)
        {
            if(id >= all_session_data.size())
                continue;

            session_data* pdata = all_session_data[id];

            ///spurious wakeup
            if(pdata == nullptr)
                continue;

            ///doesn't matter if we get a spurious wakeup
            session_data* next = pdata->tick(conn, next_wake_queue,
                                             websocket_write_queue, http_write_queue,
                                             websocket_read_queue, http_read_queue);

            if(next)
            {
                delete pdata;

                all_session_data[id] = next;
                pdata = next;

                {
                    std::lock_guard guard(conn.mut);
                    conn.upgraded_to_websocket.push_back(id);
                }
            }

            if(pdata->has_terminated())
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

                {
                    std::lock_guard guard(conn.mut);

                    for(int i=0; i < (int)conn.new_http_clients.size(); i++)
                    {
                        if(conn.new_http_clients[i] == id)
                        {
                            conn.new_http_clients.erase(conn.new_http_clients.begin() + i);
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
        std::vector<write_data>* read_queue_ptr = nullptr;

        {
            std::unique_lock guard(conn.fat_readwrite_mutex);

            write_queue_ptr = &conn.pending_websocket_write_queue[-1];
            read_queue_ptr = &conn.pending_websocket_read_queue[-1];
        }

        std::vector<write_data>& write_queue = *write_queue_ptr;
        std::vector<write_data>& read_queue = *read_queue_ptr;

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
                    std::lock_guard guard(conn.fat_readwrite_mutex);

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

                                      std::lock_guard guard(conn.fat_readwrite_mutex);

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
        std::lock_guard g2(conn.fat_readwrite_mutex);

        conn.pending_websocket_write_queue.clear();
        conn.pending_websocket_read_queue.clear();
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

            write_queue_ptr = &conn.directed_websocket_write_queue[-1];
            write_mutex_ptr = &conn.directed_write_lock[-1];

            read_queue_ptr = &conn.fine_websocket_read_queue[-1];
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

            conn.directed_websocket_write_queue.clear();
        }

        {
            std::lock_guard g3(conn.fine_read_lock[-1]);

            conn.fine_websocket_read_queue.clear();
        }
    }

    conn.client_connected_to_server = 0;
}
}
#endif // __EMSCRIPTEN__


http_data::http_data(std::string_view view)
{
    ptr = view.begin();
    len = view.size();
    owned = false;
}

http_data::http_data(const char* str, size_t _size)
{
    ptr = str;
    len = _size;
    owned = true;
}

http_data::~http_data()
{
    if(ptr == nullptr)
        return;

    if(owned)
    {
        delete [] ptr;
    }
}

const char* http_data::data() const
{
    return ptr;
}

size_t http_data::size() const
{
    return len;
}

connection_send_data::connection_send_data(const connection_settings& _sett) : sett(_sett)
{

}

void connection_send_data::disconnect(uint64_t id)
{
    force_disconnection_list.insert(id);
}

bool connection_send_data::write_to_websocket(const write_data& dat)
{
    if(dat.data.size() > sett.max_write_size)
        return false;

    websocket_write_queue[dat.id].push_back(dat);

    return true;
}

bool connection_send_data::write_to_websocket(write_data&& dat)
{
    if(dat.data.size() > sett.max_write_size)
        return false;

    websocket_write_queue[dat.id].push_back(std::move(dat));

    return true;
}

bool connection_send_data::write_to_http(const http_write_info& info)
{
    if(info.body.size() > sett.max_write_size || info.mime_type.size() > sett.max_write_size)
        return false;

    http_write_queue[info.id].push_back(info);

    return true;
}

bool connection_send_data::write_to_http_unchecked(const http_write_info& info)
{
    http_write_queue[info.id].push_back(info);

    return true;
}
bool connection_send_data::write_to_http_unchecked(http_write_info&& info)
{
    http_write_queue[info.id].push_back(std::move(info));

    return true;
}

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
        thrd.emplace_back(server_thread<boost::beast::tcp_stream>, std::ref(*this), address, port, sett);
    #endif // SUPPORT_NO_SSL_SERVER

    if(type == connection_type::SSL)
        thrd.emplace_back(server_thread<ssl::stream<boost::beast::tcp_stream>>, std::ref(*this), address, port, sett);
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

void connection::send_bulk(connection_send_data& in)
{
    {
        std::scoped_lock guard(force_disconnection_lock);

        for(auto id : in.force_disconnection_list)
        {
            force_disconnection_queue.insert(id);
        }
    }

    in.force_disconnection_list.clear();

    for(auto& i : in.websocket_write_queue)
    {
        std::lock_guard guard(fat_readwrite_mutex);

        move_append(pending_websocket_write_queue[i.first], std::move(i.second));
    }

    for(auto& i : in.http_write_queue)
    {
        std::lock_guard guard(fat_readwrite_mutex);

        move_append(pending_http_write_queue[i.first], std::move(i.second));
    }

    {
        std::lock_guard guard(wake_lock);

        ///this is a change: instead of waking repeatedly per write, this only wakes once even if there are multiple writes pending
        ///the underlying implementation already re-wakes, so this is a perf improvement
        for(auto& i : in.websocket_write_queue)
        {
            wake_queue.push_back(i.first);
        }

        for(auto& i : in.http_write_queue)
        {
            wake_queue.push_back(i.first);
        }
    }

    in.websocket_write_queue.clear();
    in.http_write_queue.clear();
}

void connection::receive_bulk(connection_received_data& in)
{
    in = connection_received_data();

    {
        std::scoped_lock guard(mut);

        in.new_clients = std::move(new_clients);
        new_clients.clear();

        in.upgraded_to_websocket = std::move(upgraded_to_websocket);
        upgraded_to_websocket.clear();

        in.new_http_clients = std::move(new_http_clients);
        new_http_clients.clear();
    }

    for(uint64_t id : in.upgraded_to_websocket)
    {
        std::scoped_lock guard(fat_readwrite_mutex);
        conditional_erase(pending_http_write_queue, id);
        conditional_erase(pending_http_read_queue, id);
    }

    {
        {
            std::scoped_lock guard(disconnected_lock);

            in.disconnected_clients = std::move(disconnected_clients);

            disconnected_clients.clear();

            for(uint64_t id : in.disconnected_clients)
            {
                std::scoped_lock guard(fat_readwrite_mutex);

                conditional_erase(pending_websocket_write_queue, id);
                conditional_erase(pending_http_write_queue, id);
                conditional_erase(pending_websocket_read_queue, id);
                conditional_erase(pending_http_read_queue, id);
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
        std::scoped_lock guard(fat_readwrite_mutex);

        in.websocket_read_queue = std::move(pending_websocket_read_queue);
        in.http_read_queue = std::move(pending_http_read_queue);

        pending_websocket_read_queue.clear();
        pending_http_read_queue.clear();
    }
}

void connection::set_client_sleep_interval(uint64_t time_ms)
{
    client_sleep_interval_ms = time_ms;
}
