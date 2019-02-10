#include "networking.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

#include <boost/asio/spawn.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/strand.hpp>
#include <algorithm>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <future>
#include <boost/asio/use_future.hpp>


using tcp = boost::asio::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>

void
fail(boost::system::error_code ec, char const* what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

void on_read(
        boost::system::error_code ec,
        std::size_t bytes_transferred, bool& in)
{
    std::cout << "async read\n";
}

#if 1
#if 1
void
server_session(connection& conn, boost::asio::io_context& ioc, tcp::socket& socket)
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
        std::string lstr;

        std::atomic_int in_flight{0};

        bool once = false;


        bool async_read = false;
        bool async_write = false;

        while(1)
        {
            try
            {
                /*{
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
                }*/

                #if 0
                {
                    std::lock_guard guard(conn.mut);

                    std::cout << "available " << ws.next_layer().available() << std::endl;

                    while(ws.next_layer().available() > 0)
                    {
                        boost::system::error_code ec;

                        ws.read(buffer, ec);

                        std::cout << "now available " << ws.next_layer().available() << std::endl;

                        if(ec)
                        {
                            printf("failed\n");
                        }

                        //std::ostringstream os;
                        //os << boost::beast::buffers(buffer.data());

                        //std::string next = os.str();

                        std::string next = boost::beast::buffers_to_string(buffer.data());

                        std::cout << "bsize " << buffer.size() << std::endl;

                        buffer.consume(buffer.size());

                        std::cout << buffer.size() << std::endl;

                        std::cout << "RDATA " << next << std::endl;

                        write_data ndata;
                        ndata.data = next;
                        ndata.id = id;

                        conn.read_queue.push_back(ndata);

                        /*if(!once)
                        {
                            ws.async_read(buffer, [&](boost::system::error_code ec,
                                                        std::size_t bytes_transferred)
                                          {
                                              on_read(ec, bytes_transferred, once);
                                          });
                        }*/


                        //ws.async_read(buffer, std::bind(on_read, std::placeholders::_1, std::placeholders::_2, std::ref(in_flight)));
                    }
                }
                #endif // 0

                /*if(!async_read)
                {
                    next_read = ws.async_read(buffer, boost::asio::use_future);

                    async_read = true;
                }

                std::cout << "future\n";

                while(!next_read.valid()){}

                next_read.wait();

                std::string next = boost::beast::buffers_to_string(buffer.data());

                std::cout << "next " << next << std::endl;*/

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
                            //ws.write(boost::asio::buffer(next.data));

                            //wbuffer = boost::asio::buffer(next.data);

                            async_write = true;

                            //wbuffer = boost::asio::dynamic_buffer(next.data);

                            wbuffer.consume(wbuffer.size());

                            size_t n = buffer_copy(wbuffer.prepare(next.data.size()), boost::asio::buffer(next.data));
                            wbuffer.commit(n);

                            //std::cout << "WRITING " << next.data << std::endl;

                            ws.async_write(wbuffer.data(), [&](boost::system::error_code, std::size_t)
                                           {
                                                async_write = false;
                                           });

                            conn.write_queue.erase(it);
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

                //ioc.poll();
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
    boost::asio::io_context ioc{1};
    auto const address = boost::asio::ip::make_address(saddress);

    bool has_async = false;

    while(1)
    {
        tcp::acceptor acceptor{ioc, {address, port}};
        for(;;)
        {
            try
            {
                // This will receive the new connection

                if(!has_async)
                {
                    tcp::socket* socket = new tcp::socket{ioc};

                    has_async = true;

                    // Block until we get a connection
                    acceptor.async_accept(*socket, [&](boost::system::error_code)
                    {
                        std::thread(std::bind(
                        &server_session,
                        std::ref(conn),
                        std::ref(ioc),
                        std::ref(*socket))).detach();

                        has_async = false;
                    });
                }


                ioc.run();

                // Launch the session, transferring ownership of the socket
                /*std::thread{std::bind(
                    &server_session,
                    std::ref(conn),
                    std::ref(ioc),
                    std::move(socket))}.detach();*/


            }
            catch(...)
            {
                std::cout << "Received exception in server thread";
            }
        }
    }
}

#endif // 0

#if 0
void
fail(boost::system::error_code ec, char const* what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

// Echoes back all received WebSocket messages
class server_session : public std::enable_shared_from_this<server_session>
{
    websocket::stream<tcp::socket> ws_;
    boost::asio::strand<
        boost::asio::io_context::executor_type> strand_;
    boost::beast::multi_buffer buffer_;

public:
    // Take ownership of the socket
    explicit
    server_session(tcp::socket socket)
        : ws_(std::move(socket))
        , strand_(ws_.get_executor())
    {
    }

    // Start the asynchronous operation
    void
    run()
    {
        // Accept the websocket handshake
        ws_.async_accept(
            boost::asio::bind_executor(
                strand_,
                std::bind(
                    &server_session::on_accept,
                    shared_from_this(),
                    std::placeholders::_1)));
    }

    void
    on_accept(boost::system::error_code ec)
    {
        if(ec)
            return fail(ec, "accept");

        // Read a message
        do_read();
    }

    void
    do_read()
    {
        // Read a message into our buffer
        ws_.async_read(
            buffer_,
            boost::asio::bind_executor(
                strand_,
                std::bind(
                    &server_session::on_read,
                    shared_from_this(),
                    std::placeholders::_1,
                    std::placeholders::_2)));
    }

    void
    on_read(
        boost::system::error_code ec,
        std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        // This indicates that the session was closed
        if(ec == websocket::error::closed)
            return;

        if(ec)
            fail(ec, "read");


        std::cout << "got a message!!!\n";

        // Echo the message
        ws_.text(ws_.got_text());
        ws_.async_write(
            buffer_.data(),
            boost::asio::bind_executor(
                strand_,
                std::bind(
                    &server_session::on_write,
                    shared_from_this(),
                    std::placeholders::_1,
                    std::placeholders::_2)));
    }

    void
    on_write(
        boost::system::error_code ec,
        std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if(ec)
            return fail(ec, "write");

        // Clear the buffer
        buffer_.consume(buffer_.size());

        // Do another read
        do_read();
    }
};

//------------------------------------------------------------------------------

// Accepts incoming connections and launches the sessions
class listener : public std::enable_shared_from_this<listener>
{
    connection& conn;
    tcp::acceptor acceptor_;
    tcp::socket socket_;

public:
    listener(connection& _conn,
        boost::asio::io_context& ioc,
        tcp::endpoint endpoint)
        : conn(_conn),
        acceptor_(ioc),
        socket_(ioc)
    {
        boost::system::error_code ec;

        // Open the acceptor
        acceptor_.open(endpoint.protocol(), ec);
        if(ec)
        {
            fail(ec, "open");
            return;
        }

        // Bind to the server address
        acceptor_.bind(endpoint, ec);
        if(ec)
        {
            fail(ec, "bind");
            return;
        }

        // Start listening for connections
        acceptor_.listen(
            boost::asio::socket_base::max_listen_connections, ec);
        if(ec)
        {
            fail(ec, "listen");
            return;
        }
    }

    // Start accepting incoming connections
    void
    run()
    {
        if(! acceptor_.is_open())
            return;
        do_accept();
    }

    void
    do_accept()
    {
        acceptor_.async_accept(
            socket_,
            std::bind(
                &listener::on_accept,
                shared_from_this(),
                std::placeholders::_1));
    }

    void
    on_accept(boost::system::error_code ec)
    {
        if(ec)
        {
            fail(ec, "accept");
        }
        else
        {
            // Create the session and run it
            server_session* ptr = std::make_shared<server_session>(std::move(socket_))->run();

            std::lock_guard guard(conn.map_lock);

            sessions[conn.id++] = ptr;
        }

        // Accept another connection
        do_accept();
    }
};

void server_thread(connection& conn, std::string saddress, uint16_t port)
{
    std::thread([=, &conn]
    {
        try
        {
            auto const address = boost::asio::ip::make_address(saddress);
            auto const threads = 1;

            // The io_context is required for all I/O
            boost::asio::io_context ioc{threads};

            // Create and launch a listening port
            std::make_shared<listener>(conn, ioc, tcp::endpoint{address, port})->run();

            // Run the I/O service on the requested number of threads
            //conn.thrd.reserve(threads);
            for(auto i = threads; i > 0; --i)
            {
                //conn.thrd.emplace_back(
                std::thread(
                [&ioc]
                {
                    try{
                    ioc.run();
                    }
                    catch(...)
                    {
                        printf("hello\n");
                    }
                }).detach();
            }


            while(1);
        }
        catch(...)
        {
            printf("in catch\n");
        }
    }).detach();
}
#endif // 0

#if 1
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

        boost::beast::multi_buffer buffer;

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

                        buffer.consume(buffer.size());

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
#endif // 0

#if 0
class client_session : public std::enable_shared_from_this<client_session>
{
    connection& conn;

    tcp::resolver resolver_;
    websocket::stream<tcp::socket> ws_;
    boost::beast::multi_buffer buffer_;
    std::string host_;

    bool can_write = true;

public:
    // Resolver and socket require an io_context
    explicit
    client_session(connection& _conn, boost::asio::io_context& ioc)
        : conn(_conn),
          resolver_(ioc),
          ws_(ioc)
    {
    }

    // Start the asynchronous operation
    void
    run(
        char const* host,
        char const* port)
    {
        // Save these for later
        host_ = host;

        // Look up the domain name
        resolver_.async_resolve(
            host,
            port,
            std::bind(
                &client_session::on_resolve,
                shared_from_this(),
                std::placeholders::_1,
                std::placeholders::_2));
    }

    void
    on_resolve(
        boost::system::error_code ec,
        tcp::resolver::results_type results)
    {
        if(ec)
            return fail(ec, "resolve");

        // Make the connection on the IP address we get from a lookup
        boost::asio::async_connect(
            ws_.next_layer(),
            results.begin(),
            results.end(),
            std::bind(
                &client_session::on_connect,
                shared_from_this(),
                std::placeholders::_1));
    }

    void
    on_connect(boost::system::error_code ec)
    {
        if(ec)
            return fail(ec, "connect");

        // Perform the websocket handshake
        ws_.async_handshake(host_, "/",
            std::bind(
                &client_session::on_handshake,
                shared_from_this(),
                std::placeholders::_1));
    }

    void
    on_handshake(boost::system::error_code ec)
    {
        if(ec)
            return fail(ec, "handshake");

        // Send the message
        /*ws_.async_write(
            boost::asio::buffer(text_),
            std::bind(
                &client_session::on_write,
                shared_from_this(),
                std::placeholders::_1,
                std::placeholders::_2));*/
    }

    void
    on_write(
        boost::system::error_code ec,
        std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if(ec)
            return fail(ec, "write");

        // Read a message into our buffer
        ws_.async_read(
            buffer_,
            std::bind(
                &client_session::on_read,
                shared_from_this(),
                std::placeholders::_1,
                std::placeholders::_2));
    }

    void
    on_read(
        boost::system::error_code ec,
        std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if(ec)
            return fail(ec, "read");

        // Close the WebSocket connection
        ws_.async_close(websocket::close_code::normal,
            std::bind(
                &client_session::on_close,
                shared_from_this(),
                std::placeholders::_1));
    }

    void
    on_close(boost::system::error_code ec)
    {
        if(ec)
            return fail(ec, "close");

        // If we get here then the connection is closed gracefully

        // The buffers() function helps print a ConstBufferSequence
        std::cout << boost::beast::buffers(buffer_.data()) << std::endl;
    }
};
#endif // 0

//------------------------------------------------------------------------------

/*void client_thread(connection& conn, std::string address, uint16_t port)
{
    // The io_context is required for all I/O
    boost::asio::io_context ioc;

    // Launch the asynchronous operation
    auto ptr = std::make_shared<client_session>(conn, ioc)->run(address.c_str(), std::to_string(port).c_str());

    conn.client = *ptr;

    // Run the I/O service. The call will return when
    // the get operation is complete.
    ioc.run();
}*/
#endif // 0

/*// Echoes back all received WebSocket messages


//------------------------------------------------------------------------------

// Accepts incoming connections and launches the sessions
*/

#if 0
void
do_session(connection& conn, boost::asio::io_context& ioc, tcp::socket& socket, boost::asio::yield_context yield)
{
    boost::system::error_code ec;

    // Construct the stream by moving in the socket
    websocket::stream<tcp::socket> ws{std::move(socket)};

    // Accept the websocket handshake
    ws.async_accept(yield[ec]);
    if(ec)
        return fail(ec, "accept");

    uint64_t id = conn.id++;


    boost::asio::post(ioc.get_executor(), [id]
             {
                std::cout << "test id " << id << std::endl;
             });

    static std::mutex lck;


    boost::beast::multi_buffer buffer;

    bool going_write = false;
    bool going_read = false;

    std::future<size_t> next_read;
    std::future<size_t> next_write;


    for(;;)
    {
        // This buffer will hold the incoming message

        if(!going_read)
        {
            next_read = ws.async_read(buffer, boost::asio::use_future);

            going_read = true;
        }

        std::cout <<" hello\n";

        next_read.wait();

        std::cout << "read " << boost::beast::buffers_to_string(buffer.data()) << std::endl;

        /*if(next_read.valid())
        {
            std::cout << "READ " <<  boost::beast::buffers_to_string(buffer.data()) << std::endl;

            next_read = std::future<size_t>();
            going_read = false;
        }*/

        // Read a message
        /*ws.async_read(buffer, yield[ec]);

        // This indicates that the session was closed
        if(ec == websocket::error::closed)
            break;

        if(ec)
            return fail(ec, "read");

        // Echo the message back
        ws.text(ws.got_text());
        ws.async_write(buffer.data(), yield[ec]);
        if(ec)
            return fail(ec, "write");*/

        //ioc.poll();

        /*if(!going_write)
        {
            bool has_data = false;
            std::string data;

            {
                std::lock_guard guard(conn.mut);

                if(conn.write_queue.size() > 0)
                {
                    has_data = true;
                    data = conn.write_queue[0].data;

                    conn.write_queue.erase(conn.write_queue.begin());
                }
            }

            if(has_data)
            {
                going_write = true;

                boost::asio::post(ioc.get_executor(),
                         [&, data]
                         {
                            ws.async_write(boost::asio::buffer(data), yield[ec]);

                            going_write = false;
                         });
            }
        }

        if(!going_read)
        {
            going_read = true;

            std::cout << "going read\n";

            boost::asio::post(ioc.get_executor(),[&]
                     {
                         std::cout << "post\n";

                        boost::beast::multi_buffer rbuffer;

                        ws.async_read(rbuffer, yield[ec]);

                        std::string next = boost::beast::buffers_to_string(rbuffer.data());

                        write_data ndata;
                        ndata.data = next;
                        ndata.id = id;

                        {
                            std::lock_guard guard(conn.mut);
                            conn.read_queue.push_back(ndata);
                        }

                        going_read = false;
                     });
        }*/

        //ioc.poll();
        //ioc.post(yield[ec]);
    }
}

void
do_listen(connection& conn,
    boost::asio::io_context& ioc,
    tcp::endpoint endpoint,
    boost::asio::yield_context yield)
{
    boost::system::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(ioc);
    acceptor.open(endpoint.protocol(), ec);
    if(ec)
        return fail(ec, "open");

    // Bind to the server address
    acceptor.bind(endpoint, ec);
    if(ec)
        return fail(ec, "bind");

    // Start listening for connections
    acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
    if(ec)
        return fail(ec, "listen");

    for(;;)
    {
        tcp::socket socket(ioc);
        acceptor.async_accept(socket, yield[ec]);
        if(ec)
            fail(ec, "accept");
        else
            boost::asio::spawn(
                acceptor.get_executor().context(),
                std::bind(
                    &do_session,
                    std::ref(conn),
                    std::ref(ioc),
                    std::move(socket),
                    std::placeholders::_1));
    }
}

void server_thread(connection& conn, std::string saddress, uint16_t port)
{
    std::thread([&, saddress, port]
    {
        boost::asio::io_context ioc{1};

        auto address = boost::asio::ip::make_address(saddress);

        int threads = 1;

        // Spawn a listening port
        boost::asio::spawn(ioc,
            std::bind(
                &do_listen,
                std::ref(conn),
                std::ref(ioc),
                tcp::endpoint{address, port},
                std::placeholders::_1));

        // Run the I/O service on the requested number of threads
        conn.thrd.reserve(threads-1);
        for(auto i = threads-1; i > 0; --i)
            conn.thrd.emplace_back(
            [&ioc]
            {
                ioc.run();
            });
            ioc.run();
    }).detach();
}

void
do_client_session(
    connection& conn,
    std::string const& host,
    std::string const& port,
    std::string const& text,
    boost::asio::io_context& ioc,
    boost::asio::yield_context yield)
{
    boost::system::error_code ec;

    // These objects perform our I/O
    tcp::resolver resolver{ioc};
    websocket::stream<tcp::socket> ws{ioc};

    // Look up the domain name
    auto const results = resolver.async_resolve(host, port, yield[ec]);
    if(ec)
        return fail(ec, "resolve");

    // Make the connection on the IP address we get from a lookup
    boost::asio::async_connect(ws.next_layer(), results.begin(), results.end(), yield[ec]);
    if(ec)
        return fail(ec, "connect");

    // Perform the websocket handshake
    ws.async_handshake(host, "/", yield[ec]);
    if(ec)
        return fail(ec, "handshake");

    // Send the message
    ws.async_write(boost::asio::buffer(std::string(text)), yield[ec]);
    if(ec)
        return fail(ec, "write");

    // This buffer will hold the incoming message
    boost::beast::multi_buffer b;

    // Read a message into our buffer
    ws.async_read(b, yield[ec]);
    if(ec)
        return fail(ec, "read");

    // Close the WebSocket connection
    ws.async_close(websocket::close_code::normal, yield[ec]);
    if(ec)
        return fail(ec, "close");

    // If we get here then the connection is closed gracefully

    // The buffers() function helps print a ConstBufferSequence
    std::cout << boost::beast::buffers(b.data()) << std::endl;
}

void client_thread(connection& conn, std::string address, uint16_t port)
{
    std::thread([&, address, port]
    {
        boost::asio::io_context ioc;

        // Launch the asynchronous operation
        boost::asio::spawn(ioc, std::bind(
            &do_client_session,
            std::ref(conn),
            address,
            std::string(std::to_string(port)),
            std::string("poop"),
            std::ref(ioc),
            std::placeholders::_1));

        // Run the I/O service. The call will return when
        // the get operation is complete.
        ioc.run();
    }).detach();
}
#endif // 0

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
