// MHXX <-> MHGU crossplay (interface: Includes/ctr/crossplay.hpp). The MHGU bridge (mhgu_hub,
// mhgu_hub_proto) is the same code the emulator runs; here it is fed the console's own UDS frames.
// Its emulator-side types (Network::WifiPacket, NetworkInfo, the Settings store) are confined to
// this file; the plugin never sees them.

#include "ctr/crossplay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "azl/bridge_sleep.hpp"
#include "azl/log.hpp"
#include "azl/safe_thread.hpp"
#include "azl/uds_frames.hpp" // azl::FindBeaconTags, azl::BeaconTags

// ---- the kit bridge (emulator-side) --------------------------------------------------------
#include "common/settings.h"
#include "core/hle/service/nwm/mhgu_hub.h"
#include "core/hle/service/nwm/nwm_uds.h" // full NetworkInfo / NodeInfo definitions
#include "network/room_member.h"

// ---- shims the kit bridge expects from its host (hook.cpp provides these in the emulator) ---
// Settings storage. The bridge reads Settings::values; here we own it.
namespace Settings {
Values values;
}
// "am I in an Azahar room?" -- in the emulator the bridge avoids double-bridging. Here the plugin
// IS the bridge (crossplay mode never runs the room client at the same time), so never.
namespace MmjHub {
bool InRoom() {
    return false;
}
void Log(int level, const std::string& text) {
    if (level >= 4) {
        azl::Log("hub: %s", text.c_str()); // (the bridge's own messages, now in AzaharLink.log)
    }
}
} // namespace MmjHub

namespace azlx {

namespace Bridge = Service::NWM::MhguBridge;

// ---- azl::WifiPacket <-> Network::WifiPacket (identical wire fields, different structs) ------
static Network::WifiPacket ToNet(const azl::WifiPacket& p) {
    Network::WifiPacket n;
    n.type = static_cast<Network::WifiPacket::PacketType>(static_cast<int>(p.type));
    n.data = p.data;
    std::memcpy(n.transmitter_address.data(), p.transmitter_address.data(), 6);
    std::memcpy(n.destination_address.data(), p.destination_address.data(), 6);
    n.channel = p.channel;
    return n;
}
static azl::WifiPacket ToAzl(const Network::WifiPacket& n) {
    azl::WifiPacket p;
    p.type = static_cast<azl::WifiPacket::Type>(static_cast<int>(n.type));
    p.data = n.data;
    std::memcpy(p.transmitter_address.data(), n.transmitter_address.data(), 6);
    std::memcpy(p.destination_address.data(), n.destination_address.data(), 6);
    p.channel = n.channel;
    return p;
}

// ---- a beacon the game sent (it is hosting) -> NetworkInfo for the hub ----------------------
// The NetworkInfo vendor tag (id 21) carries NetworkInfo from oui_value (+0xC) on; host_mac/channel
// come from the frame. Enough for the bridge to list the console's hub for MHGU players.
static bool BeaconToNetworkInfo(const azl::WifiPacket& beacon, Service::NWM::NetworkInfo* out,
                                std::vector<Service::NWM::NodeInfo>* nodes) {
    azl::BeaconTags tags = azl::FindBeaconTags(beacon.data);
    if (!tags.network || tags.network_size < (0x108 - 0xC)) {
        return false;
    }
    std::memset(out, 0, sizeof(*out));
    std::memcpy(out->host_mac_address.data(), beacon.transmitter_address.data(), 6);
    out->channel = beacon.channel ? beacon.channel : 11;
    out->initialized = 1;
    const std::size_t copy = std::min<std::size_t>(tags.network_size, 0x108 - 0xC);
    std::memcpy(reinterpret_cast<u8*>(out) + 0xC, tags.network, copy);
    nodes->clear();
    if (out->total_nodes == 0) {
        out->total_nodes = 1;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
struct CrossplayBridge::Impl {
    std::atomic<bool> running{false};
    DeliverFn deliver;
    MacFn own_mac;
    TickFn tick;
    azl::SafeThread tick_thread;
};

CrossplayBridge::CrossplayBridge() : impl_(new Impl) {}
CrossplayBridge::~CrossplayBridge() {
    Stop();
}

bool CrossplayBridge::Start(const std::string& servers, const std::string& key, DeliverFn deliver,
                            MacFn own_mac, TickFn tick) {
    if (impl_->running) {
        return true;
    }
    azl::Log("crossplay: Start begins (hubs '%s')", servers.c_str());
    impl_->deliver = std::move(deliver);
    impl_->own_mac = std::move(own_mac);
    impl_->tick = std::move(tick);

    Settings::values.mhgu_hub_enabled.value = true;
    Settings::values.mhgu_hub_servers.value = servers;
    Settings::values.mhgu_hub_key.value = key;
    Settings::values.mhgu_hub_convert.value = true;
    Settings::values.mhgu_hub_log.value = false;

    const bool attached = Bridge::Attach(
        // hub -> game: a frame to inject into the game's UDS
        [this](const Network::WifiPacket& p) {
            if (impl_->deliver) {
                impl_->deliver(ToAzl(p));
            }
        },
        // the hub network was lost
        []() {},
        // this console's station MAC
        [this]() -> Network::MacAddress {
            Network::MacAddress m{};
            if (impl_->own_mac) {
                azl::Mac rm = impl_->own_mac();
                std::memcpy(m.data(), rm.data(), 6);
            }
            return m;
        });

    azl::Log("crossplay: Bridge::Attach -> %s", attached ? "ok" : "FAILED");
    if (!attached) {
        // The bridge thread did not start (Bridge::Attach says why in the log). Not fatal: the plugin goes on without
        // crossplay instead of the game dying in an uncaught std::system_error.
        Settings::values.mhgu_hub_enabled.value = false; // (the UDS side must not hand frames to a bridge that is not there)
        impl_->deliver = nullptr;
        impl_->own_mac = nullptr;
        impl_->tick = nullptr;
        return false;
    }
    impl_->running = true;

    // The room used to tick the engine from its network thread; in crossplay the room is not up, so
    // a thread of our own does it (and gives the client direction its periodic scan).
    if (impl_->tick) {
        impl_->tick_thread = azl::SafeThread([this] {
            using namespace std::chrono;
            while (impl_->running.load()) {
                const auto now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch());
                impl_->tick(static_cast<std::uint64_t>(now.count()));
                azl::BridgeSleepMs(200);
            }
        });
        if (!impl_->tick_thread.joinable()) {
            // Without it nothing ticks the engine: the bridge cannot work. Undo the attach and say so.
            impl_->running = false;
            Bridge::Detach();
            Settings::values.mhgu_hub_enabled.value = false;
            return false;
        }
    }
    return true;
}

void CrossplayBridge::Stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }
    if (impl_->tick_thread.joinable()) {
        impl_->tick_thread.join();
    }
    Bridge::Detach();
}

bool CrossplayBridge::Running() const {
    return impl_->running;
}

void CrossplayBridge::SendFrame(const azl::WifiPacket& packet) {
    if (!impl_->running) {
        return;
    }
    if (packet.type == azl::WifiPacket::Type::Beacon) {
        // The game is hosting: present its network to MHGU players on the hub.
        Service::NWM::NetworkInfo info;
        std::vector<Service::NWM::NodeInfo> nodes;
        if (BeaconToNetworkInfo(packet, &info, &nodes)) {
            Bridge::HostUpdate(info, nodes);
        }
    } else {
        // Data / auth / assoc / deauth / nodemap -> the hub.
        Bridge::Send(ToNet(packet));
    }
}

void CrossplayBridge::Scan(std::uint32_t wlan_comm_id, std::uint8_t id) {
    if (!impl_->running) {
        return;
    }
    // A game that scans for any network sends 0xFFFFFFFF / 0xFF: the bridge takes 0 (MHXX's own ids).
    if (wlan_comm_id == 0xFFFFFFFFu) {
        wlan_comm_id = 0;
    }
    if (id == 0xFF) {
        id = 0;
    }
    Bridge::Scan(wlan_comm_id, id);
}

} // namespace azlx
