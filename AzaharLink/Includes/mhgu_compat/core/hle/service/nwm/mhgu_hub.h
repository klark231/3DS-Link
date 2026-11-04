// Copyright Azahar Emulator Project / AzaharPlus
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// MHGU hub crossplay: Monster Hunter XX (3DS) local play through MHGU Recomp's hub servers.
//
// With [Multiplayer] mhgu_hub_enabled on and no Azahar room joined, the UDS service's 802.11 frames go to this
// bridge instead of a room. The bridge plays the other consoles:
//  - scanning: the hubs MHGU players host (listed on the hub servers) show up as UDS networks, and joining one
//    joins the hub (the bridge plays the network's host, the hub's players are the other nodes);
//  - hosting: the network the game hosts is listed on the hub servers as an MHGU hub, and each MHGU player who joins
//    it is played to the game as a 3DS connecting to it.
// UDS node n is the hub's station n - 1 (address 169.254.x.n) on both sides, so the games' player numbers agree.
// The game's packets are converted where the two games' network libraries differ (mhgu_hub_proto.h).
#pragma once

#include <functional>
#include <vector>
#include "common/common_types.h"
#include "network/room.h"

namespace Network {
struct WifiPacket;
}

namespace Service::NWM {

struct NetworkInfo;
struct NodeInfo;

namespace MhguBridge {

/// The bridge takes the UDS service's traffic: the setting is on and no Azahar room is joined.
bool Enabled();

/// The UDS service registers how a frame reaches it, what to do when the hub it joined is gone, and its MAC.
void Attach(std::function<void(const Network::WifiPacket&)> deliver, std::function<void()> lost,
            std::function<Network::MacAddress()> own_mac);
void Detach();

/// A frame the UDS service sends (instead of sending it to a room).
void Send(const Network::WifiPacket& packet);

/// The game scans for networks (RecvBeaconBroadcastData): the hubs found are delivered as beacons.
void Scan(u32 wlan_comm_id, u8 id);

/// The game hosts a network: its current state (sent with every beacon), and its end.
void HostUpdate(const NetworkInfo& info, const std::vector<NodeInfo>& nodes);
void HostStop();

/// The game leaves the network it joined.
void ClientLeave();

} // namespace MhguBridge

} // namespace Service::NWM
