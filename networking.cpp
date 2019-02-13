#include "networking.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

#include <boost/asio/buffer.hpp>
#include <memory>
#include <thread>
#include <vector>

using tcp = boost::asio::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>

void
fail(boost::system::error_code ec, char const* what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

void
server_session(connection& conn, boost::asio::io_context& socket_ioc, tcp::socket& socket)
{
    try
    {
        websocket::stream<tcp::socket> ws{std::move(socket)};

        boost::asio::ip::tcp::no_delay nagle(true);
        ws.next_layer().set_option(nagle);

        boost::beast::websocket::permessage_deflate opt;

        opt.server_enable = true;
        ws.set_option(opt);

        ws.text(false);

        ws.accept();

        uint64_t id = conn.id++;

        {
            std::lock_guard guard(conn.mut);

            conn.new_clients.push_back(id);
        }

        boost::beast::multi_buffer rbuffer;
        boost::beast::multi_buffer wbuffer;

        bool async_read = false;
        bool async_write = false;

        while(1)
        {
            try
            {
                if(!async_write)
                {
                    std::lock_guard guard(conn.mut);

                    for(auto it = conn.write_queue.begin(); it != conn.write_queue.end();)
                    {
                        write_data next = *it;

                        if(next.id != id)
                        {
                            it++;
                            continue;
                        }
                        else
                        {
                            async_write = true;

                            wbuffer.consume(wbuffer.size());

                            size_t n = buffer_copy(wbuffer.prepare(next.data.size()), boost::asio::buffer(next.data));
                            wbuffer.commit(n);

                            ws.async_write(wbuffer.data(), [&](boost::system::error_code, std::size_t)
                                           {
                                                async_write = false;
                                           });

                            conn.write_queue.erase(it);
                            break;
                        }
                    }
                }

                if(!async_read)
                {
                    ws.async_read(rbuffer, [&](boost::system::error_code, std::size_t)
                                  {
                                      std::string next = boost::beast::buffers_to_string(rbuffer.data());

                                      std::lock_guard guard(conn.mut);

                                      write_data ndata;
                                      ndata.data = next;
                                      ndata.id = id;

                                      conn.read_queue.push_back(ndata);

                                      rbuffer = decltype(rbuffer)();

                                      async_read = false;

                                  });

                    async_read = true;
                }

                socket_ioc.poll();
            }
            catch(...)
            {
                std::cout << "exception\n";
            }

            Sleep(1);
        }
    }
    catch(boost::system::system_error const& se)
    {
        // This indicates that the session was closed
        if(se.code() != websocket::error::closed)
            std::cerr << "Error: " << se.code().message() << std::endl;
    }
    catch(std::exception const& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

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

        std::thread(server_session, std::ref(conn), std::ref(*next_context), std::ref(*socket)).detach();

        Sleep(1);
    }
}

void client_thread(connection& conn, std::string address, uint16_t port)
{
    try
    {
        boost::asio::io_context ioc;

        // These objects perform our I/O
        tcp::resolver resolver{ioc};
        websocket::stream<tcp::socket> ws{ioc};

        // Look up the domain name
        auto const results = resolver.resolve(address, std::to_string(port));

        // Make the connection on the IP address we get from a lookup
        boost::asio::connect(ws.next_layer(), results.begin(), results.end());

        boost::asio::ip::tcp::no_delay nagle(true);
        ws.next_layer().set_option(nagle);

        boost::beast::websocket::permessage_deflate opt;
        opt.client_enable = true; // for clients

        ws.set_option(opt);

        ws.handshake(address, "/");

        ws.text(false);

        boost::beast::multi_buffer rbuffer;
        boost::beast::multi_buffer wbuffer;

        bool async_write = false;
        bool async_read = false;

        while(1)
        {
            try
            {
                {
                    std::lock_guard guard(conn.mut);

                    if(!async_write)
                    {
                        while(conn.write_queue.size() > 0)
                        {
                            write_data next = conn.write_queue.front();

                            wbuffer.consume(wbuffer.size());

                            size_t n = buffer_copy(wbuffer.prepare(next.data.size()), boost::asio::buffer(next.data));
                            wbuffer.commit(n);

                            conn.write_queue.erase(conn.write_queue.begin());

                            ws.async_write(wbuffer.data(), [&](boost::system::error_code, std::size_t)
                                           {
                                                async_write = false;
                                           });

                            async_write = true;
                            break;
                        }
                    }
                }

                if(!async_read)
                {
                    ws.async_read(rbuffer, [&](boost::system::error_code, std::size_t)
                                  {
                                      std::string next = boost::beast::buffers_to_string(rbuffer.data());

                                      std::lock_guard guard(conn.mut);

                                      write_data ndata;
                                      ndata.data = next;
                                      ndata.id = -1;

                                      conn.read_queue.push_back(ndata);

                                      rbuffer = decltype(rbuffer)();

                                      async_read = false;
                                  });

                    async_read = true;
                }
            }
            catch(...)
            {
                std::cout << "exception in client write\n";
            }

            ioc.poll();

            Sleep(1);
        }
    }
    catch(std::exception& e)
    {
        std::cout << "exception in client write outer " << e.what() << std::endl;
    }
}

void connection::host(const std::string& address, uint16_t port)
{
    thread_is_server = true;

    thrd.emplace_back(server_thread, std::ref(*this), address, port);
}

void connection::connect(const std::string& address, uint16_t port)
{
    thread_is_client = true;

    thrd.emplace_back(client_thread, std::ref(*this), address, port);
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

    return read_queue.size() > 0;
}

std::string connection::read()
{
    std::lock_guard guard(mut);

    if(read_queue.size() == 0)
        throw std::runtime_error("Bad queue");

    return read_queue.front().data;
}

write_data connection::read_from()
{
    std::lock_guard guard(mut);

    if(read_queue.size() == 0)
        throw std::runtime_error("Bad queue");

    return read_queue.front();
}

void connection::pop_read()
{
    std::lock_guard guard(mut);

    if(read_queue.size() == 0)
        throw std::runtime_error("Bad queue");

    read_queue.erase(read_queue.begin());
}

void connection::write_to(const write_data& data)
{
    std::lock_guard guard(mut);

    write_queue.push_back(data);
}

std::optional<uint64_t> connection::has_new_client()
{
    for(auto& i : new_clients)
    {
        return i;
    }

    return std::nullopt;
}

void connection::pop_new_client()
{
    if(new_clients.size() > 0)
    {
        new_clients.erase(new_clients.begin());
    }
}
