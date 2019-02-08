#include "networking.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

using tcp = boost::asio::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>

void
server_session(connection& conn, tcp::socket& socket)
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

        while(1)
        {
            try
            {
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
                            ws.write(boost::asio::buffer(next.data));
                            conn.write_queue.erase(it);
                        }
                    }
                }

                boost::beast::multi_buffer buffer;

                {
                    std::lock_guard guard(conn.mut);

                    while(ws.next_layer().available() > 0)
                    {
                        ws.read(buffer);

                        std::ostringstream os;
                        os << boost::beast::buffers(buffer.data());

                        std::string next = os.str();

                        write_data ndata;
                        ndata.data = next;
                        ndata.id = id;

                        conn.read_queue.push_back(ndata);
                    }
                }

            }
            catch(...)
            {

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

connection::connection()
{

}

void server_thread(connection& conn, std::string saddress, uint16_t port)
{
    boost::asio::io_context ioc{1};
    auto const address = boost::asio::ip::make_address(saddress);

    while(1)
    {
        tcp::acceptor acceptor{ioc, {address, port}};
        for(;;)
        {
            try
            {
                // This will receive the new connection
                tcp::socket socket{ioc};

                // Block until we get a connection
                acceptor.accept(socket);

                // Launch the session, transferring ownership of the socket
                std::thread{std::bind(
                    &server_session,
                    std::ref(conn),
                    std::move(socket))}.detach();
            }
            catch(...)
            {
                std::cout << "Received exception in server thread";
            }
        }
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

        while(1)
        {
            try
            {
                {
                    std::lock_guard guard(conn.mut);

                    while(conn.write_queue.size() > 0)
                    {
                        write_data next = conn.write_queue.front();

                        ws.write(boost::asio::buffer(next.data));

                        conn.write_queue.erase(conn.write_queue.begin());
                    }
                }

                boost::beast::multi_buffer buffer;

                {
                    std::lock_guard guard(conn.mut);

                    while(ws.next_layer().available() > 0)
                    {
                        ws.read(buffer);

                        std::ostringstream os;
                        os << boost::beast::buffers(buffer.data());

                        std::string next = os.str();

                        write_data ndata;
                        ndata.data = next;
                        ndata.id = -1;

                        conn.read_queue.push_back(ndata);
                    }
                }
            }
            catch(...)
            {
                std::cout << "exception in client write\n";
            }

            Sleep(1);
        }
    }
    catch(...)
    {
        std::cout << "exception in client write outer\n";
    }
}

void connection::host(const std::string& address, uint16_t port)
{
    thrd.emplace_back(server_thread, std::ref(*this), address, port);
}

void connection::connect(const std::string& address, uint16_t port)
{
    thrd.emplace_back(client_thread, std::ref(*this), address, port);
}

void connection::write(const std::string& data)
{
    std::lock_guard guard(mut);

    write_data ndata;
    ndata.data = data;
    ndata.id = -1;

    write_queue.push_back(ndata);
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
