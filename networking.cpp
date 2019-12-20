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

template<typename T>
void server_session(connection& conn, boost::asio::io_context& socket_ioc, tcp::socket& socket)
{
    uint64_t id = -1;
    T* wps = nullptr;
    ssl::context ctx{ssl::context::sslv23};

    try
    {
        boost::asio::ip::tcp::no_delay nagle(true);

        /*std::cout << "Pre flat\n";

        boost::beast::flat_buffer buffer;

        boost::beast::http::request<boost::beast::http::string_body> req;

        std::cout << "Pre read\n";

        boost::beast::http::read(socket, buffer, req);

        std::cout << "Post read\n";

        if(!websocket::is_upgrade(req))
            throw std::runtime_error("Tried to send http request");

        std::cout << boost::beast::buffers_to_string(buffer.data()) << std::endl;*/

        if constexpr(std::is_same_v<T, websocket::stream<tcp::socket>>)
        {
            wps = new T{std::move(socket)};
            wps->text(false);

            wps->next_layer().set_option(nagle);
        }

        if constexpr(std::is_same_v<T, websocket::stream<ssl::stream<tcp::socket>>>)
        {
            static std::string cert = read_file_bin("./deps/secret/cert/cert.crt");
            static std::string dh = read_file_bin("./deps/secret/cert/dh.pem");
            static std::string key = read_file_bin("./deps/secret/cert/key.pem");

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

            wps = new T{std::move(socket), ctx};
            wps->text(false);

            wps->next_layer().next_layer().set_option(nagle);

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
        ws.set_option(opt);

        ws.accept();

        id = conn.id++;

        {
            std::lock_guard guard(conn.mut);

            conn.new_clients.push_back(id);
            conn.connected_clients.push_back(id);
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
            std::lock_guard guard(conn.mut);

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

                                      rbuffer = decltype(rbuffer)();

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

    {
        std::lock_guard guard(conn.mut);

        for(int i=0; i < (int)conn.connected_clients.size(); i++)
        {
            if(conn.connected_clients[i] == id)
            {
                conn.connected_clients.erase(conn.connected_clients.begin() + i);
                i--;
                continue;
            }
        }
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
}

template<typename T>
void server_thread(connection& conn, std::string saddress, uint16_t port)
{
    auto const address = boost::asio::ip::make_address(saddress);

    std::atomic_bool accepted = true;
    boost::asio::io_context acceptor_context{1};

    tcp::acceptor acceptor{acceptor_context, {address, port}};

    while(1)
    {
        boost::asio::io_context* next_context = new boost::asio::io_context{1};

        tcp::socket* socket = new tcp::socket{*next_context};

        acceptor.accept(*socket);

        std::thread(server_session<T>, std::ref(conn), std::ref(*next_context), std::ref(*socket)).detach();

        sf::sleep(sf::milliseconds(1));;
    }
}

template<typename T>
void client_thread(connection& conn, std::string address, uint16_t port)
{
    T* wps = nullptr;
    boost::asio::io_context ioc;
    ssl::context ctx{ssl::context::sslv23_client};

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

            boost::asio::connect(wps->next_layer().next_layer(), results.begin(), results.end());

            wps->next_layer().next_layer().set_option(nagle);
            wps->next_layer().handshake(ssl::stream_base::client);
        }

        assert(wps != nullptr);

        T& ws = *wps;

        boost::beast::websocket::permessage_deflate opt;
        opt.client_enable = true; // for clients

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
            std::lock_guard guard(conn.mut);

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

        while(1)
        {
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

                                      rbuffer = decltype(rbuffer)();

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

            sf::sleep(sf::milliseconds(1));

            if(conn.should_terminate)
                break;
        }
    }
    catch(std::exception& e)
    {
        std::cout << "exception in client write outer " << e.what() << std::endl;
    }

    {
        std::lock_guard guard(conn.mut);

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
            std::lock_guard guard(conn.mut);

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
            {
                std::lock_guard guard(write_mutex);

                while(write_queue.size() > 0 && sock_writable(sock))
                {
                    write_data& next = write_queue.front();

                    std::string& to_send = next.data;

                    int num = send(sock, to_send.data(), to_send.size(), 0);

                    if(num < 0)
                        throw std::runtime_error("Broken write");

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
                    {
                        throw std::runtime_error("SOCK BROKEN");
                    }

                    buf[num] = '\0';

                    std::string ret(buf, buf + num);

                    std::lock_guard guard(read_mutex);

                    write_data ndata;
                    ndata.data = std::move(ret);
                    ndata.id = -1;

                    read_queue.push_back(ndata);
                }
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
        std::lock_guard guard(conn.mut);

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

bool connection::connection_pending()
{
    return connection_in_progress;
}

#ifndef __EMSCRIPTEN__
void connection::host(const std::string& address, uint16_t port, connection_type::type type)
{
    thread_is_server = true;

    if(type == connection_type::PLAIN)
        thrd.emplace_back(server_thread<websocket::stream<tcp::socket>>, std::ref(*this), address, port);

    if(type == connection_type::SSL)
        thrd.emplace_back(server_thread<websocket::stream<ssl::stream<tcp::socket>>>, std::ref(*this), address, port);
}
#endif // __EMSCRIPTEN__

void connection::connect(const std::string& address, uint16_t port, connection_type::type type)
{
    thread_is_client = true;

    #ifndef __EMSCRIPTEN__
    if(type == connection_type::PLAIN)
        thrd.emplace_back(client_thread<websocket::stream<tcp::socket>>, std::ref(*this), address, port);

    if(type == connection_type::SSL)
        thrd.emplace_back(client_thread<websocket::stream<ssl::stream<tcp::socket>>>, std::ref(*this), address, port);
    #else

    printf("In connect\n");

    if(type != connection_type::PLAIN)
        throw std::runtime_error("So, not sure this is possible");

    printf("Pre emplace\n");

    thrd.emplace_back(client_thread_tcp, std::ref(*this), address, port);

    printf("Post emplace\n");
    #endif
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
    std::lock_guard guard(mut);

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

    std::lock_guard guard(mut);

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

void connection::pop_read(uint64_t id)
{
    std::vector<write_data>* read_ptr = nullptr;
    std::mutex* mut_ptr = nullptr;

    {
        std::lock_guard guard(mut);

        read_ptr = &fine_read_queue[id];
        mut_ptr = &fine_read_lock[id];
    }

    std::lock_guard guard(*mut_ptr);

    if(read_ptr->size() == 0)
        throw std::runtime_error("Bad queue");

    read_ptr->erase(read_ptr->begin());

    /*if(read_queue.size() == 0)
        throw std::runtime_error("Bad queue");

    read_queue.erase(read_queue.begin());*/
}

void connection::write_to(const write_data& data)
{
    std::vector<write_data>* write_dat = nullptr;
    std::mutex* write_mutex = nullptr;

    {
        std::lock_guard guard(mut);

        write_dat = &directed_write_queue[data.id];
        write_mutex = &directed_write_lock[data.id];
    }

    std::lock_guard guard(*write_mutex);

    write_dat->push_back(data);
}

std::optional<uint64_t> connection::has_new_client()
{
    std::lock_guard guard(mut);

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
    std::lock_guard guard(disconnected_lock);

    if(disconnected_clients.size() == 0)
        throw std::runtime_error("No disconnected clients");

    disconnected_clients.erase(disconnected_clients.begin());
}

void connection::pop_new_client()
{
    std::lock_guard guard(mut);

    if(new_clients.size() > 0)
    {
        new_clients.erase(new_clients.begin());
    }
}

std::vector<uint64_t> connection::clients()
{
    std::lock_guard guard(mut);

    return connected_clients;
}
