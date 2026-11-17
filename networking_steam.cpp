#include "networking_steam.hpp"
#include <steamworks_sdk_163/sdk/public/steam/steam_api_flat.h>
#include <steamworks_sdk_163/sdk/public/steam/steam_api.h>
#include <steamworks_sdk_163/sdk/public/steam/steam_api_common.h>
#include <steamworks_sdk_163/sdk/public/steam/steam_gameserver.h>
#include <steamworks_sdk_163/sdk/public/steam/isteamnetworkingsockets.h>
#include <steamworks_sdk_163/sdk/public/steam/isteamnetworkingutils.h>
#include <assert.h>
#include <flat_set>
#include <string_view>
#include <string>
#include <mutex>
#include <stdint.h>
#include <thread>
#include <atomic>
#include <iostream>

void networking2::connection_send_data::disconnect(uint64_t id)
{
    force_disconnection_list.push_back(id);
}

void networking2::connection_send_data::write_to(write_data&& dat)
{
    write_queue.push_back(std::move(dat));
}

namespace
{
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
}

using namespace networking2;

void global_on_status_changed(SteamNetConnectionStatusChangedCallback_t* info);

struct steam_network_server;

//this is such a capi-ism
steam_network_server* net = nullptr;

struct network_base
{
    network_base(ISteamNetworkingSockets* sock) : sns(sock){}

    HSteamNetPollGroup poll_group;
    ISteamNetworkingSockets* sns = nullptr;

    //std::flat_set<HSteamNetConnection> clients;

    std::mutex write_lock;
    std::vector<write_data> write_msgs;

    std::mutex read_lock;
    std::vector<write_data> read_msgs;

    std::mutex disconnect_lock;
    std::vector<uint64_t> force_disconnect;

    std::mutex new_disconnected_lock;
    std::vector<uint64_t> new_disconnected;

    std::mutex new_clients_lock;
    std::vector<uint64_t> new_clients;

    std::atomic_bool shutdown{false};
    std::jthread thrd;

    void start()
    {
        poll_group = SteamAPI_ISteamNetworkingSockets_CreatePollGroup(sns);
        assert(poll_group != k_HSteamNetPollGroup_Invalid);

        thrd = std::jthread([this]{
            while(!shutdown)
            {
                poll();
                bool any = send_all();
                any = receive_messages() || any;
                handle_disconnects();

                if(!any)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    void send_to(HSteamNetConnection conn, std::string_view data)
    {
        SteamAPI_ISteamNetworkingSockets_SendMessageToConnection(sns, conn, data.data(), data.size(), k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
    }

    void add_disconnected(uint64_t id)
    {
        std::lock_guard guard(disconnect_lock);
        new_disconnected.push_back(id);
    }

    void handle_disconnects()
    {
        std::vector<uint64_t> disconnects;

        {
            std::lock_guard guard(disconnect_lock);
            disconnects = std::move(force_disconnect);
            force_disconnect.clear();
        }

        for(uint64_t id : disconnects)
        {
            HSteamNetConnection conn = (HSteamNetConnection)id;
            SteamAPI_ISteamNetworkingSockets_CloseConnection(sns, conn, 0, nullptr, false);

            //clients.erase((HSteamNetConnection)id);
        }
    }

    bool send_all()
    {
        std::vector<write_data> to_write;

        {
            std::lock_guard guard(write_lock);
            to_write = std::move(write_msgs);
            write_msgs.clear();
        }

        for(const write_data& d : to_write)
        {
            send_to((HSteamNetConnection)d.id, d.data);
        }

        return to_write.size() > 0;
    }

    void send_bulk(networking2::connection_send_data&& to_send)
    {
        {
            std::lock_guard guard(write_lock);
            move_append(write_msgs, std::move(to_send.write_queue));
            to_send.write_queue.clear();
        }

        {
            std::lock_guard guard(disconnect_lock);
            move_append(force_disconnect, std::move(to_send.force_disconnection_list));
            to_send.force_disconnection_list.clear();
        }
    }

    void receive_bulk(networking2::connection_received_data& to_receive)
    {
        {
            std::lock_guard guard(read_lock);
            to_receive.read_queue = std::move(read_msgs);
            read_msgs.clear();
        }

        {
            std::lock_guard guard(new_disconnected_lock);
            to_receive.disconnected_clients = std::move(new_disconnected);
            new_disconnected.clear();
        }

        {
            std::lock_guard guard(new_clients_lock);
            to_receive.new_clients = std::move(new_clients);
            new_clients.clear();
        }
    }


    bool receive_messages()
    {
        constexpr int max_reads = 100;
        std::array<ISteamNetworkingMessage*, max_reads> messages;

        int num = SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup(sns, poll_group, messages.data(), max_reads);

        assert(num >= 0);

        if(num == 0)
            return false;

        for(int i=0; i < num; i++)
        {
            ISteamNetworkingMessage* message = messages[i];

            //assert(clients.find(message->m_conn) != clients.end());

            std::string data((char*)message->m_pData, message->m_cbSize);
            message->Release();

            {
                std::lock_guard guard(read_lock);

                write_data& wd = read_msgs.emplace_back();
                wd.data = std::move(data);
                wd.id = (uint64_t)message->m_conn;
            }
        }

        return true;
    }

    void poll()
    {
        SteamAPI_ISteamNetworkingSockets_RunCallbacks(sns);
    }

    void on_status_changed(SteamNetConnectionStatusChangedCallback_t* info)
    {
        printf("Status change\n");

        switch(info->m_info.m_eState)
        {
            case k_ESteamNetworkingConnectionState_None:
                break;

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                //if(auto it = clients.find(info->m_hConn); it != clients.end())
                //    clients.erase(it);

                printf("Disconnected\n");

                add_disconnected((uint64_t)info->m_hConn);

                SteamAPI_ISteamNetworkingSockets_CloseConnection(sns, info->m_hConn, 0, nullptr, false);
                break;

            case k_ESteamNetworkingConnectionState_Connecting:
                //assert(clients.find(info->m_hConn) == clients.end());

                printf("Connecting\n");

                if(SteamAPI_ISteamNetworkingSockets_AcceptConnection(sns, info->m_hConn) != k_EResultOK)
                {
                    SteamAPI_ISteamNetworkingSockets_CloseConnection(sns, info->m_hConn, 0, nullptr, false);
                    break;
                }

                if(!SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup(sns, info->m_hConn, poll_group))
                {
                    SteamAPI_ISteamNetworkingSockets_CloseConnection(sns, info->m_hConn, 0, nullptr, false);
                    break;
                }

                //clients.insert(info->m_hConn);

                {
                    std::lock_guard guard(new_clients_lock);
                    new_clients.push_back((uint64_t)info->m_hConn);
                }

                break;

            case k_ESteamNetworkingConnectionState_Connected:
                printf("Connected\n");
                break;

            default:
                break;
        }
    }

    void stop()
    {
        shutdown = true;
        thrd.join();
    }
};

#define USE_IP

struct steam_network_server : networking2::network_server
{
    network_base* base = nullptr;
    ISteamNetworkingSockets* sns = nullptr;
    HSteamListenSocket sock;

    steam_network_server(){}

    void host(const std::string& address, uint16_t port, const std::string& steamid_token) override
    {
        printf("Preinit\n");

        #if 1
        assert(SteamGameServer_Init(0, port, 1300, eServerModeNoAuthentication, "1.0.0.0"));

        ISteamGameServer* sgs = SteamAPI_SteamGameServer();

        SteamAPI_ISteamGameServer_LogOn(sgs, steamid_token.c_str());
        //SteamAPI_ISteamGameServer_LogOnAnonymous(sgs);

        std::cout << "Server hosted steam ID " << SteamAPI_ISteamGameServer_GetSteamID(sgs) << " on port " << port << std::endl;

        sns = SteamAPI_SteamGameServerNetworkingSockets_SteamAPI();
        assert(sns);
        #else
        SteamAPI_Init();

        sns = SteamAPI_SteamNetworkingSockets_SteamAPI();
        #endif

        base = new network_base(sns);

        ISteamNetworkingUtils* snu = SteamAPI_SteamNetworkingUtils_SteamAPI();
        assert(snu);

        std::vector<SteamNetworkingConfigValue_t> cfgs;

        {
            SteamNetworkingConfigValue_t& opt = cfgs.emplace_back();
            opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)global_on_status_changed);
        }

        {
            SteamNetworkingConfigValue_t& opt = cfgs.emplace_back();
            opt.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 2);
        }


        #ifdef USE_IP
        SteamNetworkingIPAddr addr;
        addr.Clear();
        addr.SetIPv4(0, port);
        sock = SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP(sns, addr, cfgs.size(), cfgs.data());
        #else
        sock = SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P(sns, 0, cfgs.size(), cfgs.data());
        #endif

        assert(sock != k_HSteamListenSocket_Invalid);

        base->start();

        printf("Started Networking Steam\n");
    }

    void send_bulk(networking2::connection_send_data&& to_send) override
    {
        base->send_bulk(std::move(to_send));
    }

    void receive_bulk(networking2::connection_received_data& to_receive) override
    {
        base->receive_bulk(to_receive);
    }

    ~steam_network_server()
    {
        base->stop();

        ISteamNetworkingSockets* sns = SteamAPI_SteamGameServerNetworkingSockets_SteamAPI();
        assert(sns);

        ISteamGameServer* sgs = SteamAPI_SteamGameServer();
        assert(sgs);

        //follow destruct order from valve's example chat
        //https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/examples/example_chat.cpp
        SteamAPI_ISteamNetworkingSockets_CloseListenSocket(sns, sock);
        SteamAPI_ISteamNetworkingSockets_DestroyPollGroup(sns, base->poll_group);

        SteamAPI_ISteamGameServer_LogOff(sgs);
        SteamGameServer_Shutdown();

        delete base;
        base = nullptr;
    }
};

void global_on_status_changed(SteamNetConnectionStatusChangedCallback_t* info)
{
    printf("Global Network Status Changea\n");

    net->base->on_status_changed(info);
}

void client_global_on_status_changed(SteamNetConnectionStatusChangedCallback_t* info);

struct steam_network_client;

steam_network_client* client_net = nullptr;

struct steam_network_client : networking2::network_client
{
    std::atomic_bool client_connected{false};
    std::atomic_bool in_progress{false};
    ISteamNetworkingSockets* sns = nullptr;
    HSteamNetConnection conn = k_HSteamNetConnection_Invalid;

    network_base* base = nullptr;

    bool client_connected_to_server() override
    {
        return client_connected;
    }

    bool connection_in_progress() override
    {
        return in_progress;
    }

    steam_network_client()
    {
        assert(SteamAPI_Init());

        sns = SteamAPI_SteamNetworkingSockets_SteamAPI();
        assert(sns);

        base = new network_base(sns);
    }

    void connect(uint64_t csteam_id, uint16_t port, const std::string& address) override
    {
        SteamNetworkingIdentity sni;
        sni.Clear();

        std::cout << "Internal connecting to " << csteam_id << " address " << address << std::endl;

        sni.SetSteamID64(csteam_id);

        std::vector<SteamNetworkingConfigValue_t> cfgs;

        {
            SteamNetworkingConfigValue_t& opt = cfgs.emplace_back();
            opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)client_global_on_status_changed);
        }

        {
            SteamNetworkingConfigValue_t& opt = cfgs.emplace_back();
            opt.SetInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, 0);
        }

        {
            SteamNetworkingConfigValue_t& opt = cfgs.emplace_back();
            opt.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 2);
        }

        std::cout << "Connecting port " << port << std::endl;

        #ifdef USE_IP
        SteamNetworkingIPAddr addr;
        addr.Clear();
        assert(addr.ParseString(address.c_str()));
        addr.m_port = port;

        conn = SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress(sns, addr, cfgs.size(), cfgs.data());
        #else
        conn = SteamAPI_ISteamNetworkingSockets_ConnectP2P(sns, sni, port, cfgs.size(), cfgs.data());
        #endif
        in_progress = true;

        base->start();
    }

    void send_bulk(networking2::connection_send_data&& to_send) override
    {
        if(!client_connected)
            return;

        base->send_bulk(std::move(to_send));
    }

    void receive_bulk(networking2::connection_received_data& to_receive) override
    {
        if(!client_connected)
            return;

        base->receive_bulk(to_receive);
    }

    void on_status_changed(SteamNetConnectionStatusChangedCallback_t* info)
    {
        printf("StatusChange\n");

        assert(info->m_hConn == conn || conn == k_HSteamNetConnection_Invalid);

        switch(info->m_info.m_eState)
        {
            case k_ESteamNetworkingConnectionState_None:
                printf("None\n");
                break;

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                client_connected = false;
                in_progress = false;
                printf("Bad Connection\n");

                SteamAPI_ISteamNetworkingSockets_CloseConnection(sns, info->m_hConn, 0, nullptr, false);
                break;

            case k_ESteamNetworkingConnectionState_Connecting:
                printf("Connecting\n");
                break;

            case k_ESteamNetworkingConnectionState_Connected:
                printf("Connected\n");
                client_connected = true;
                in_progress = false;

                break;

            default:
                break;
        }
    }
};

void client_global_on_status_changed(SteamNetConnectionStatusChangedCallback_t* info)
{
    client_net->on_status_changed(info);
}

networking2::network_server* start_network_server()
{
    assert(!net);

    net = new steam_network_server;
    return net;
}

networking2::network_client* start_network_client()
{
    assert(!client_net);
    client_net = new steam_network_client;
    return client_net;
}
