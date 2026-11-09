// MHXX <-> MHGU crossplay. When it is on, local play is not carried to an Azahar room: the plugin
// speaks MHGU Recomp's hub protocol (ldn-over-IP) straight from the console's Wi-Fi, so a real 3DS
// plays Monster Hunter XX with MHGU (Switch/Android) players, with nothing else running anywhere.
//
// Only azl types cross this header. The MHGU bridge (Network::WifiPacket, NetworkInfo, ...) stays in
// crossplay.cpp. The frames the game sends on local wireless go in through SendFrame (a beacon is the
// console hosting -> the hub lists it; the rest is match traffic -> the hub); the hub's frames come
// back through the deliver callback the plugin hands to Start.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "azl/uds_frames.hpp" // azl::WifiPacket, azl::Mac

namespace azlx {

class CrossplayBridge {
public:
    CrossplayBridge();
    ~CrossplayBridge();

    using DeliverFn = std::function<void(const azl::WifiPacket&)>; // a frame from the hub, to inject into the game
    using MacFn = std::function<azl::Mac()>;                       // this console's station MAC
    using TickFn = std::function<void(std::uint64_t now_ms)>;      // called ~every 200 ms (room's tick had this job)

    // The hub servers ("a b,c", the emulator's second Web API URL) and the matchmaking key (empty =
    // public). deliver and tick run on the bridge's own thread. Returns false if it could not start.
    bool Start(const std::string& servers, const std::string& key, DeliverFn deliver, MacFn own_mac,
               TickFn tick = {});
    void Stop();
    bool Running() const;

    // A frame the game just sent on local wireless (what the room path passes to RoomClient::SendWifiPacket).
    // Beacons advertise a hosted network -> the bridge lists it as an MHGU hub; data/auth/etc. -> the hub.
    void SendFrame(const azl::WifiPacket& packet);

    // The game is scanning for networks (RecvBeaconBroadcastData): the MHGU hubs show up as beacons via
    // deliver. wlan_comm_id/id are the UDS scan parameters (MHXX's comm id, the scan's id byte).
    void Scan(std::uint32_t wlan_comm_id, std::uint8_t id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace azlx
