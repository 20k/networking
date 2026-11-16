#include "networking_steam.hpp"
#include <steamworks_sdk_163/sdk/public/steam/steam_api_flat.h>
#include <steamworks_sdk_163/sdk/public/steam/steam_api.h>
#include <steamworks_sdk_163/sdk/public/steam/steam_api_common.h>
#include <steamworks_sdk_163/sdk/public/steam/steam_gameserver.h>
#include <steamworks_sdk_163/sdk/public/steam/isteamnetworkingsockets.h>
#include <steamworks_sdk_163/sdk/public/steam/isteamnetworkingutils.h>
#include <assert.h>
#include <flat_set>

void global_on_status_changed(SteamNetConnectionStatusChangedCallback_t* info);

struct steam_network;

//this is such a capi-ism
steam_network* net = nullptr;

struct steam_network
{
    HSteamListenSocket sock;
    HSteamNetPollGroup poll_group;
    ISteamNetworkingSockets* sns;

    std::flat_set<HSteamNetConnection> clients;

    steam_network()
    {
        assert(SteamGameServer_Init(0, 1286, 1233, eServerModeNoAuthentication, "0.0.0.0"));

        ISteamGameServer* sgs = SteamAPI_SteamGameServer();
        SteamAPI_ISteamGameServer_LogOnAnonymous(sgs);

        sns = SteamAPI_SteamGameServerNetworkingSockets_SteamAPI();
        assert(sns);

        ISteamNetworkingUtils* snu = SteamAPI_SteamNetworkingUtils_SteamAPI();
        assert(snu);

        SteamNetworkingIPAddr addr;
        addr.SetIPv4(0, 1286);

        //HSteamListenSocket sock = SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP(sns, addr, 0, nullptr);

        SteamNetworkingConfigValue_t opt;
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)::global_on_status_changed);

        sock = SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P(sns, 1286, 0, nullptr);
        assert(sock != k_HSteamListenSocket_Invalid);

        poll_group = SteamAPI_ISteamNetworkingSockets_CreatePollGroup(sns);
        assert(poll_group != k_HSteamNetPollGroup_Invalid);

    }

    void poll()
    {

    }

    void on_status_changed(SteamNetConnectionStatusChangedCallback_t* info)
    {
        switch(info->m_info.m_eState)
        {
            case k_ESteamNetworkingConnectionState_None:
                break;

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                if(auto it = clients.find(info->m_hConn); it != clients.end())
                    clients.erase(it);

                SteamAPI_ISteamNetworkingSockets_CloseConnection(sns, info->m_hConn, 0, nullptr, false);
                break;

            case k_ESteamNetworkingConnectionState_Connecting:
                assert(clients.find(info->m_hConn) == clients.end());

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

                clients.insert(info->m_hConn);

                break;

            default:
                break;
        }
    }

    ~steam_network()
    {
        ISteamNetworkingSockets* sns = SteamAPI_SteamGameServerNetworkingSockets_SteamAPI();
        assert(sns);

        ISteamGameServer* sgs = SteamAPI_SteamGameServer();
        assert(sgs);

        //follow destruct order from valve's example chat
        //https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/examples/example_chat.cpp
        SteamAPI_ISteamNetworkingSockets_CloseListenSocket(sns, sock);
        SteamAPI_ISteamNetworkingSockets_DestroyPollGroup(sns, poll_group);

        SteamAPI_ISteamGameServer_LogOff(sgs);
        SteamGameServer_Shutdown();
    }
};

void on_status_changed(SteamNetConnectionStatusChangedCallback_t* info)
{
    net->on_status_changed(info);
}

void test_steam_networking(int appid)
{
    net = new steam_network;

    /*SteamErrMsg msg = {};
    ESteamAPIInitResult result = SteamAPI_InitFlat(&msg);
    printf("%s\n", msg);
    assert(result == k_ESteamAPIInitResult_OK);

    ISteamNetworkingSockets* sns = SteamAPI_SteamNetworkingSockets_SteamAPI();
    assert(sns);

    ESteamNetworkingAvailability avail = SteamAPI_ISteamNetworkingSockets_InitAuthentication(sns);

    printf("Availability %i\n", avail);*/


    while(1){}

    assert(false);
}
