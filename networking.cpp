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

#include <boost/fiber/all.hpp>
#include "fiber_round_robin.hpp"
#include "fiber_yield.hpp"

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

#ifdef CONNECTION_PER_THREAD
template<typename T>
void server_session(connection& conn, boost::asio::io_context* psocket_ioc, tcp::socket* psocket)
{
    auto& socket_ioc = *psocket_ioc;
    auto& socket = *psocket;

    uint64_t id = -1;
    T* wps = nullptr;
    ssl::context ctx{ssl::context::tls_server};

    try
    {
        boost::asio::ip::tcp::no_delay nagle(true);

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
        opt.client_enable = true;

        ws.set_option(opt);

        ws.accept();

        id = conn.id++;

        {
            std::unique_lock guard(conn.mut);

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

    {
        std::unique_lock guard(conn.mut);

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

        std::thread(server_session<T>, std::ref(conn), next_context, socket).detach();

        sf::sleep(sf::milliseconds(1));;
    }
}
#endif // CONNECTION_PER_THREAD

/*template<typename T>
void session(std::shared_ptr<tcp::socket> sock) {
    try {
        for (;;) {
            char data[1024*1024];
            boost::system::error_code ec;
            std::size_t length = sock->async_read_some(boost::asio::buffer(data), boost::fibers::asio::get_yield(ec));
            if(ec == boost::asio::error::eof) {
                break; //connection closed cleanly by peer
            } else if(ec) {
                throw boost::system::system_error(ec); //some other error
            }
            //print(tag(), ": handled: ", std::string(data, length));
            boost::asio::async_write(*sock, boost::asio::buffer(data, length), boost::fibers::asio::get_yield(ec));

            if(ec == boost::asio::error::eof) {
                break; //connection closed cleanly by peer
            } else if(ec) {
                throw boost::system::system_error(ec); //some other error
            }
        }
        //print(tag(), ": connection closed");
    } catch (std::exception const& ex) {
        //print(tag(), ": caught exception : ", ex.what());
    }
}*/

#define ONE_FIBER_THREAD
#ifdef ONE_FIBER_THREAD
template<typename T>
struct socket_data
{
    std::shared_ptr<T> wps;
    //std::shared_ptr<ssl::context> ctx;
};

struct async_fiber_state
{
    bool suspended = false;
    bool has_result = false;
    boost::fibers::detail::spinlock lock;
    std::unique_lock<boost::fibers::detail::spinlock> bmut;
    boost::fibers::context* ptr = nullptr;
    //boost::system::error_code _ec;
    std::atomic_bool failed = false;

    async_fiber_state() : bmut(lock, std::defer_lock)
    {

    }
};

void async_fiber_callback(async_fiber_state& state, const boost::system::error_code& _ec)
{
    state.bmut.lock();
    state.has_result = true;
    //state._ec = _ec;
    state.failed = (bool)_ec;

    if(state.suspended)
    {
        boost::fibers::context::active()->schedule(state.ptr);
    }

    state.bmut.unlock();
}

void async_try_suspend(async_fiber_state& state)
{
    state.bmut.lock();
    if(!state.has_result)
    {
        state.suspended = true;
        state.ptr = boost::fibers::context::active();
        boost::fibers::context::active()->suspend(state.bmut);
    }
    else
    {
        state.bmut.unlock();
    }
}

template<typename T>
socket_data<T> make_socket_data(tcp::socket& socket, boost::asio::ssl::context& ssl_context)
{
    socket_data<T> ret;
    std::shared_ptr<T> wps;

    boost::asio::ip::tcp::no_delay nagle(true);

    if constexpr(std::is_same_v<T, websocket::stream<boost::beast::tcp_stream>>)
    {
        wps = std::shared_ptr<T>(new T{std::move(socket)});
        wps->text(false);
        wps->set_option(websocket::stream_base::timeout::suggested(boost::beast::role_type::server));

        wps->next_layer().socket().set_option(nagle);
    }

    if constexpr(std::is_same_v<T, websocket::stream<ssl::stream<boost::beast::tcp_stream>>>)
    {
        wps = std::shared_ptr<T>(new T{std::move(socket), ssl_context});
        wps->text(false);
        wps->set_option(websocket::stream_base::timeout::suggested(boost::beast::role_type::server));

        wps->next_layer().next_layer().socket().set_option(nagle);

        async_fiber_state st;

        wps->next_layer().async_handshake(ssl::stream_base::server,
        [&](auto in)
        {
            async_fiber_callback(st, in);
        });

        async_try_suspend(st);

        //wps->next_layer().async_handshake(ssl::stream_base::server, boost::fibers::asio::get_yield(ec));

        if(st.failed)
            throw std::runtime_error("Bad 1");

        printf("Post handshake\n");
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

    //ws.accept();

    async_fiber_state st2;
    ws.async_accept([&](auto in)
    {
        async_fiber_callback(st2, in);
    });

    async_try_suspend(st2);

    if(st2.failed)
        throw std::runtime_error("Bad 2");

    printf("Post accept2");

    ret.wps = wps;

    return ret;
}

template<typename T>
void write_fiber(connection& conn, socket_data<T>& sock, int id, int& term)
{
    boost::beast::multi_buffer buffer;

    std::vector<write_data>* queue_ptr = nullptr;
    std::mutex* mutex_ptr = nullptr;

    {
        std::unique_lock guard(conn.mut);

        queue_ptr = &conn.directed_write_queue[id];
        mutex_ptr = &conn.directed_write_lock[id];
    }

    std::vector<write_data>& queue = *queue_ptr;
    std::mutex& mutex = *mutex_ptr;

    uint64_t empty_spins = 0;

    try
    {
        while(term == 0)
        {
            std::optional<write_data> next_data = std::nullopt;

            {
                std::lock_guard guard(mutex);

                if(queue.size() > 0)
                {
                    next_data = *queue.begin();
                    queue.erase(queue.begin());
                }
            }

            if(next_data.has_value())
            {
                buffer.consume(buffer.size());

                size_t n = buffer_copy(buffer.prepare(next_data.value().data.size()), boost::asio::buffer(next_data.value().data));
                buffer.commit(n);

                async_fiber_state st;
                sock.wps->async_write(buffer.data(), [&](auto in, auto _)
                {
                    async_fiber_callback(st, in);
                });

                async_try_suspend(st);

                /*if(st.failed && st._ec == boost::asio::error::eof)
                    break;
                else */if(st.failed)
                {
                    printf("Write failed\n");
                    break;
                }

                empty_spins = 0;
            }
            else
            {
                empty_spins++;
            }

            ///2 attempts at 1ms sleep
            if(empty_spins <= 1)
                boost::this_fiber::sleep_for(std::chrono::milliseconds(1));
            ///16 attempts at 4ms sleep
            else if(empty_spins <= 16)
                boost::this_fiber::sleep_for(std::chrono::milliseconds(4));
            ///else sleep for 32ms
            else
                boost::this_fiber::sleep_for(std::chrono::milliseconds(32));
        }
    }
    catch(...)
    {
        printf("Error in write fiber\n");
    }

    term++;
}

template<typename T>
void read_fiber(connection& conn, socket_data<T>& sock, int id, int& term)
{
    boost::beast::multi_buffer buffer;

    std::vector<write_data>* queue_ptr = nullptr;
    std::mutex* mutex_ptr = nullptr;

    {
        std::unique_lock guard(conn.mut);

        queue_ptr = &conn.fine_read_queue[id];
        mutex_ptr = &conn.fine_read_lock[id];
    }

    std::vector<write_data>& queue = *queue_ptr;
    std::mutex& mutex = *mutex_ptr;

    try
    {
        while(term == 0)
        {
            async_fiber_state st;
            sock.wps->async_read(buffer, [&](auto in, auto _)
            {
                async_fiber_callback(st, in);
            });

            async_try_suspend(st);

            /*if(st.failed && st._ec == boost::asio::error::eof)
                break;
            else */if(st.failed)
            {
                printf("Read failed\n");
                break;
            }

            std::string next = boost::beast::buffers_to_string(buffer.data());

            write_data ndata;
            ndata.data = std::move(next);
            ndata.id = id;

            {
                std::lock_guard guard(mutex);

                queue.push_back(ndata);

                buffer = decltype(buffer)();
            }

            ndata = write_data();

            boost::this_fiber::sleep_for(std::chrono::milliseconds(1));
        }
    }
    catch(...)
    {
        printf("Err in read fiber\n");
    }

    term++;
}

template<typename T>
void disconnect_fiber(connection& conn, socket_data<T>& sock, int id, int& term)
{
    while(term == 0)
    {
        {
            std::scoped_lock guard(conn.force_disconnection_lock);

            auto it = conn.force_disconnection_queue.find(id);

            if(it != conn.force_disconnection_queue.end())
            {
                conn.force_disconnection_queue.erase(it);
                break;
            }
        }

        boost::this_fiber::sleep_for(std::chrono::milliseconds(2000));
    }

    try
    {
        boost::system::error_code ec;
        //sock.wps->async_close(boost::beast::websocket::close_code::none, boost::fibers::asio::get_yield(ec));

        boost::beast::get_lowest_layer(*sock.wps).cancel(ec);
    }
    catch(...){}

    term++;
}

template<typename T>
void session(connection& conn, tcp::socket& in, int& session_count, boost::asio::ssl::context& ssl_context)
{
    socket_data<T> sock;

    try
    {
        sock = make_socket_data<T>(std::ref(in), std::ref(ssl_context));
    }
    catch(std::exception const& ex)
    {
        std::cout << "Failed to create socket in session: " << ex.what() << std::endl;
        return;
    }
    catch(...)
    {
        printf("Failed to create socket\n");
        return;
    }

    int64_t id = conn.id++;

    {
        std::scoped_lock guard(conn.mut);

        conn.new_clients.push_back(id);
        conn.connected_clients.push_back(id);
    }

    int should_term = 0;

    boost::fibers::fiber f1(read_fiber<T>, std::ref(conn), std::ref(sock), id, std::ref(should_term));
    boost::fibers::fiber f2(write_fiber<T>, std::ref(conn), std::ref(sock), id, std::ref(should_term));
    //boost::fibers::fiber f3(disconnect_fiber<T>, std::ref(conn), std::ref(sock), id, std::ref(should_term));

    /*while(should_term != 3)
    {
        boost::this_fiber::sleep_for(std::chrono::milliseconds(32));
    }*/

    f1.join();
    f2.join();
    //f3.join();

    {
        std::scoped_lock guard(conn.mut);

        for(int i=0; i < (int)conn.connected_clients.size(); i++)
        {
            if(conn.connected_clients[i] == (uint64_t)id)
            {
                conn.connected_clients.erase(conn.connected_clients.begin() + i);
                i--;
                continue;
            }
        }
    }

    {
        std::lock_guard guard(conn.disconnected_lock);
        conn.disconnected_clients.push_back(id);
    }

    session_count--;
}

template<typename T>
void server(connection& conn, std::shared_ptr<boost::asio::io_context> io_ctx, tcp::acceptor& a) {
    try {
        static std::string cert = read_file_bin("./deps/secret/cert/cert.crt");
        static std::string dh = read_file_bin("./deps/secret/cert/dh.pem");
        static std::string key = read_file_bin("./deps/secret/cert/key.pem");

        boost::asio::ssl::context ssl_context{ssl::context::tls_server};

        ssl_context.set_options(boost::asio::ssl::context::default_workarounds |
                        boost::asio::ssl::context::no_sslv2 |
                        boost::asio::ssl::context::single_dh_use |
                        boost::asio::ssl::context::no_sslv3);

        ssl_context.use_certificate_chain(
            boost::asio::buffer(cert.data(), cert.size()));

        ssl_context.use_private_key(
            boost::asio::buffer(key.data(), key.size()),
            boost::asio::ssl::context::file_format::pem);

        ssl_context.use_tmp_dh(
            boost::asio::buffer(dh.data(), dh.size()));

        int max_sessions = 256;
        int session_count = 0;

        for (;;) {

            /*while(session_count >= max_sessions)
            {
                boost::this_fiber::sleep_for(std::chrono::seconds(1));
            }*/

            //std::shared_ptr<tcp::socket> socket(new tcp::socket(*io_ctx));

            tcp::socket socket(*io_ctx);

            printf("Pre accept\n");

            async_fiber_state st;
            a.async_accept(socket, [&](auto in)
            {
                async_fiber_callback(st, in);
            });
            async_try_suspend(st);

            //a.async_accept(*socket, boost::fibers::asio::get_yield(ec));
            printf("Post accept\n");

            session_count++;

            if(st.failed) {
                //throw boost::system::system_error(ec); //some other error
                printf("Error in server async_accept\n");
            } else {
                boost::fibers::fiber(session<T>, std::ref(conn), std::ref(socket), std::ref(session_count), std::ref(ssl_context)).detach();
            }

            boost::this_fiber::sleep_for(std::chrono::milliseconds(16));
        }
    } catch (std::exception const& ex) {
        std::cout << "Server caught exception " << ex.what() << std::endl;
    }
    catch(...)
    {
        printf("Caught unknown exception in server\n");
    }

    printf("Terminated server\n");

    io_ctx->stop();
}

void sleeper()
{
    while(1)
    {
        sf::sleep(sf::milliseconds(1));
        boost::this_fiber::sleep_for(std::chrono::milliseconds(512));
    }
}

class round_robin : public boost::fibers::algo::algorithm {
private:
    std::shared_ptr< boost::asio::io_context >      io_ctx_;
    boost::fibers::scheduler::ready_queue_type      rqueue_{};
    std::size_t                                     counter_{ 0 };

public:
    struct service : public boost::asio::io_context::service {
        static inline boost::asio::io_context::id                  id;

        std::unique_ptr< boost::asio::io_context::work >    work_;

        service( boost::asio::io_context & io_ctx) :
            boost::asio::io_context::service( io_ctx),
            work_{ new boost::asio::io_context::work( io_ctx) } {
        }

        virtual ~service() {}

        service( service const&) = delete;
        service & operator=( service const&) = delete;

        void shutdown_service() override final {
            work_.reset();
        }
    };

    round_robin( std::shared_ptr< boost::asio::io_context > const& io_ctx) :
        io_ctx_( io_ctx){
        // We use add_service() very deliberately. This will throw
        // service_already_exists if you pass the same io_context instance to
        // more than one round_robin instance.
        boost::asio::add_service( * io_ctx_, new service( * io_ctx_) );
        boost::asio::post( * io_ctx_, [this]() mutable {
                while ( ! io_ctx_->stopped() ) {
                    if ( has_ready_fibers() ) {
                        // run all pending handlers in round_robin
                        while ( io_ctx_->poll() )
                        {

                        }
                        // finished work, yield
                        //
                        boost::this_fiber::yield();
                    } else {
                        // run one handler inside io_context
                        // if no handler available, block this thread
                        if ( ! io_ctx_->run_one() ) {
                            break;
                        }
                    }

                    boost::this_fiber::sleep_for(std::chrono::milliseconds(1));
                    //sf::sleep(sf::milliseconds(1));
               }

               printf("Reached the end of io post\n");
            });
    }

    void awakened(boost::fibers::context * ctx) noexcept {
        BOOST_ASSERT( nullptr != ctx);
        BOOST_ASSERT( ! ctx->ready_is_linked() );
        ctx->ready_link( rqueue_); /*< fiber, enqueue on ready queue >*/
        ++counter_;
    }

    boost::fibers::context * pick_next() noexcept {
        boost::fibers::context * ctx(nullptr);
        if ( ! rqueue_.empty() ) {
            ctx = & rqueue_.front();
            rqueue_.pop_front();
            BOOST_ASSERT( nullptr != ctx);
            BOOST_ASSERT( boost::fibers::context::active() != ctx);
            --counter_;
        }
        return ctx;
    }

    bool has_ready_fibers() const noexcept {
        return 0 < counter_;
    }

    void suspend_until( std::chrono::steady_clock::time_point const& abs_time) noexcept {
        sf::sleep(sf::milliseconds(1));
    }

    void notify() noexcept {
    }
};

template<typename T>
void server_thread(connection& conn, std::string saddress, uint16_t port)
{
    //auto const address = boost::asio::ip::make_address(saddress);

    std::shared_ptr< boost::asio::io_context > io_ctx = std::make_shared< boost::asio::io_context >();
    //boost::fibers::use_scheduling_algorithm< boost::fibers::asio::round_robin >(io_ctx);
    boost::fibers::use_scheduling_algorithm< round_robin >(io_ctx);
    //boost::fibers::use_scheduling_algorithm< network_round_robin >();

    //tcp::acceptor acceptor{*io_ctx, {address, port}};
    tcp::acceptor acceptor(*io_ctx, tcp::endpoint(tcp::v4(), port));
    acceptor.set_option(boost::asio::socket_base::reuse_address(true));

    boost::fibers::fiber(sleeper).detach();
    boost::fibers::fiber(server<T>, std::ref(conn), std::ref(io_ctx), std::ref(acceptor)).detach();

    io_ctx->run();

    printf("Run should not have exited\n");
}
#endif // 0


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

bool connection::connection_pending()
{
    return connection_in_progress;
}

#ifndef __EMSCRIPTEN__
void connection::host(const std::string& address, uint16_t port, connection_type::type type)
{
    thread_is_server = true;

    #ifdef SUPPORT_NO_SSL
    if(type == connection_type::PLAIN)
        thrd.emplace_back(server_thread<websocket::stream<boost::beast::tcp_stream>>, std::ref(*this), address, port);
    #endif // SUPPORT_NO_SSL

    if(type == connection_type::SSL)
        thrd.emplace_back(server_thread<websocket::stream<ssl::stream<boost::beast::tcp_stream>>>, std::ref(*this), address, port);
}
#endif // __EMSCRIPTEN__

void connection::connect(const std::string& address, uint16_t port, connection_type::type type, std::string sni_hostname)
{
    thread_is_client = true;

    #ifndef __EMSCRIPTEN__
    #ifdef SUPPORT_NO_SSL
    if(type == connection_type::PLAIN)
        thrd.emplace_back(client_thread<websocket::stream<tcp::socket>>, std::ref(*this), address, port, sni_hostname, client_sleep_interval_ms);
    #endif // SUPPORT_NO_SSL

    if(type == connection_type::SSL)
        thrd.emplace_back(client_thread<websocket::stream<ssl::stream<tcp::socket>>>, std::ref(*this), address, port, sni_hostname, client_sleep_interval_ms);
    #else
    ///-s WEBSOCKET_URL=wss://
    if(type != connection_type::EMSCRIPTEN_AUTOMATIC)
        throw std::runtime_error("emscripten uses compiler options for secure vs non secure websockets");

    thrd.emplace_back(client_thread_tcp, std::ref(*this), address, port);
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

void connection::pop_read(uint64_t id)
{
    std::vector<write_data>* read_ptr = nullptr;
    std::mutex* mut_ptr = nullptr;

    {
        std::unique_lock guard(mut);

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

void connection::force_disconnect(uint64_t id)
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

template<typename T>
inline
void conditional_erase(T& in, int id)
{
    auto it = in.find(id);

    if(it == in.end())
        return;

    in.erase(it);
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
}

void connection::pop_new_client()
{
    std::unique_lock guard(mut);

    if(new_clients.size() > 0)
    {
        new_clients.erase(new_clients.begin());
    }
}

std::vector<uint64_t> connection::clients()
{
    std::scoped_lock guard(mut);

    return connected_clients;
}
