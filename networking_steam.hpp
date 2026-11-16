#ifndef NETWORKING_STEAM_HPP_INCLUDED
#define NETWORKING_STEAM_HPP_INCLUDED

#include <string>
#include <stdint.h>
#include <vector>
#include <assert.h>

namespace networking2
{
    struct write_data
    {
        uint64_t id = 0;
        std::string data;
    };

    struct connection_received_data
    {
        std::vector<uint64_t> new_clients;
        std::vector<uint64_t> disconnected_clients;
        std::vector<write_data> read_queue;
    };

    struct connection_send_data
    {
        std::vector<write_data> write_queue;
        std::vector<uint64_t> force_disconnection_list;

        void disconnect(uint64_t id);
        void write_to(write_data&& dat);
    };

    struct network_server
    {
        network_server(){assert(false);}

        virtual void host(const std::string& address, uint16_t port){assert(false);}

        virtual void send_bulk(connection_send_data&& to_send){assert(false);}
        virtual void receive_bulk(connection_received_data& to_receive){assert(false);}
        virtual void terminate(){assert(false);}

        virtual ~network_server(){}
    };

    struct network_client
    {
        virtual bool client_connected_to_server();
        virtual bool connection_in_progress();

        virtual void connect(const std::string& address, uint16_t port){assert(false);}

        virtual void send_bulk(connection_send_data&& to_send){assert(false);}
        virtual void receive_bulk(connection_received_data& to_receive){assert(false);}
        virtual void terminate(){assert(false);}

        virtual ~network_client(){}
    };
}

networking2::network_server* start_network_server();
networking2::network_client* start_network_client();

#endif // NETWORKING_STEAM_HPP_INCLUDED
