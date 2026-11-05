// Copyright Azahar Emulator Project / AzaharPlus
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include "core/hle/service/nwm/mhgu_hub.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "azl/bridge_sleep.hpp"
#include "azl/once.hpp"
#include "azl/safe_thread.hpp"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/hle/service/nwm/mhgu_hub_proto.h"
#include "core/hle/service/nwm/nwm_uds.h"
#include "core/hle/service/nwm/uds_beacon.h"
#include "core/hle/service/nwm/uds_connection.h"
#include "core/hle/service/nwm/uds_data.h"
#include "network/network.h"
#include "network/room_member.h"

namespace MmjHub {
bool InRoom();
}

namespace Service::NWM::MhguBridge {

namespace {

namespace H = ::MhguHub;
using Network::MacAddress;
using Network::WifiPacket;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr u8 HeartbeatChannel = 0xF;  // the UDS service's own keep-alive
constexpr u8 DefaultDataChannel = 0xF3; // the channel MHXX's network library uses
constexpr u16 BroadcastNode = 0xFFFF;
constexpr u16 MhxxAttributes = 0x8000; // what MHXX itself sets on the networks it hosts
constexpr u32 MhxxCommId = 0x00197110;
constexpr u8 HubChannel = DefaultNetworkChannel;
constexpr std::size_t MaxBeacons = 14;

// (No std::random_device on the console: its start-up can throw and the plugin has no exceptions; the clock and an address do. And no
// function-local static with a constructor: its guard throws when two threads meet, see azl/once.hpp.)
struct RngBox {
    std::mt19937_64 gen{[] {
        const u64 now = static_cast<u64>(Clock::now().time_since_epoch().count());
        int marker = 0;
#ifdef __3DS__
        return now ^ (now << 29) ^ static_cast<u64>(reinterpret_cast<std::uintptr_t>(&marker));
#else
        return now ^ static_cast<u64>(std::random_device{}()) ^ static_cast<u64>(reinterpret_cast<std::uintptr_t>(&marker));
#endif
    }()};
};

std::mt19937_64& Rng() {
    static azl::LazyNew<RngBox> box;
    return box.Get([] { azl::BridgeSleepMs(1); }).gen;
}
u32 Rand32() {
    return static_cast<u32>(Rng()());
}

std::string HexOf(std::span<const u8> d, std::size_t max = 160) {
    static const char* x = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < d.size() && i < max; i++) {
        s += x[d[i] >> 4];
        s += x[d[i] & 15];
    }
    if (d.size() > max) {
        s += "...";
    }
    return s;
}

std::string IpString(u32 ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." +
           std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255);
}

MacAddress MacFrom(const u8* p) {
    MacAddress m;
    std::memcpy(m.data(), p, 6);
    return m;
}

void SetUsername(NodeInfo& node, const std::string& utf8) {
    std::u16string w = H::Utf8ToUtf16(utf8);
    node.username.fill(0);
    for (std::size_t i = 0; i < w.size() && i < node.username.size(); i++) {
        node.username[i] = static_cast<u16>(w[i]);
    }
}

std::string UsernameOf(const NodeInfo& node) {
    std::u16string w;
    for (const u16 c : node.username) {
        if (!c) {
            break;
        }
        w += static_cast<char16_t>(c);
    }
    std::string s = H::Utf16ToUtf8(w);
    if (s.size() > 0x20) {
        s.resize(0x20);
    }
    return s.empty() ? "3DS Hunter" : s;
}

std::string NodeName(const H::NetInfo& info, int i) {
    const u8* nd = H::NodeAt(info, i);
    return std::string(reinterpret_cast<const char*>(nd + H::ND::Name),
                       strnlen(reinterpret_cast<const char*>(nd + H::ND::Name), 0x20));
}

std::string Servers() {
    const std::string s = Settings::values.mhgu_hub_servers.GetValue();
    return s.empty() ? H::DefaultServers : s;
}
std::string Key() {
    return Settings::values.mhgu_hub_key.GetValue();
}

class Bridge {
public:
    bool Attach(std::function<void(const WifiPacket&)> deliver, std::function<void()> lost,
                std::function<MacAddress()> mac) {
        Detach();
        {
            std::scoped_lock lock{attach_mutex};
            deliver_fn = std::move(deliver);
            lost_fn = std::move(lost);
            mac_fn = std::move(mac);
            own_mac = mac_fn ? mac_fn() : MacAddress{};
        }
        H::SetLogger([](const std::string& s) { LOG_INFO(Service_NWM, "{}", s); });
        {
            std::scoped_lock lock{task_mutex};
            tasks.clear();
            running = true;
        }
        // (azl/safe_thread.hpp: a std::thread that cannot start throws, and the plugin has no exceptions: the game died here)
        // A refusal can be momentary (the game is starting its own threads): three tries, 100 ms apart.
        for (int attempt = 0; attempt < 3 && !worker.joinable(); attempt++) {
            if (attempt > 0) {
                azl::BridgeSleepMs(100);
            }
            worker = azl::SafeThread([this] { Run(); });
        }
        if (!worker.joinable()) {
            LOG_WARNING(Service_NWM, "[hub] could not start the bridge thread: crossplay stays off");
            std::scoped_lock lock{task_mutex};
            running = false;
            return false;
        }
        return true;
    }

    void Detach() {
        if (!worker.joinable()) {
            return;
        }
        Post([this] {
            StationStop(false);
            HostStop();
            if (list_thread.joinable()) {
                list_thread.join();
            }
            std::scoped_lock lock{task_mutex};
            running = false;
        });
        worker.join();
        std::scoped_lock lock{attach_mutex};
        deliver_fn = nullptr;
        lost_fn = nullptr;
        mac_fn = nullptr;
    }

    void Send(const WifiPacket& packet) {
        WifiPacket p = packet;
        {
            std::scoped_lock lock{attach_mutex};
            p.transmitter_address = own_mac;
        }
        Post([this, p] { OnUdsOut(p); });
    }

    void Scan(u32 comm_id, u8 id) {
        RefreshMac();
        if (comm_id && comm_id != MhxxCommId) {
            return; // another game's local play: MHGU rooms are only for MHXX
        }
        Post([this, comm_id, id] {
            scan_comm = comm_id ? comm_id : MhxxCommId;
            scan_id = id;
            if (sta != Sta::Idle || hosting) {
                return;
            }
            if (!list_pending && Clock::now() - last_list > 3s) {
                RequestList();
            }
            DeliverBeacons();
        });
    }

    void HostUpdate(const NetworkInfo& info, const std::vector<NodeInfo>& nodes) {
        if (static_cast<u32>(info.wlan_comm_id) != MhxxCommId) {
            return; // another game's network: not listed for MHGU players
        }
        Post([this, info, nodes] {
            if (!hosting) {
                HostStart(info, nodes);
            } else {
                host_ni = info;
                host_nodes = nodes;
                HostRebuild(-1);
            }
        });
    }

    void HostStopAsync() {
        Post([this] { HostStop(); });
    }

    void ClientLeave() {
        Post([this] { StationStop(false); });
    }

private:
    // ---- worker
    void Post(std::function<void()> f) {
        std::scoped_lock lock{task_mutex};
        if (!running) {
            return;
        }
        tasks.push_back(std::move(f));
        task_cv.notify_one();
    }

    void Run() {
        std::unique_lock lock{task_mutex};
        auto next_tick = Clock::now();
        while (running) {
            if (tasks.empty()) {
                lock.unlock();
                azl::BridgeSleepMs(10); // (was task_cv.wait_for(100ms): returned at once on the console -> busy loop)
                lock.lock();
            }
            while (!tasks.empty()) {
                auto t = std::move(tasks.front());
                tasks.pop_front();
                lock.unlock();
                t();
                lock.lock();
            }
            if (running && Clock::now() >= next_tick) {
                lock.unlock();
                Tick();
                lock.lock();
                next_tick = Clock::now() + 200ms;
            }
        }
        tasks.clear();
    }

    void RefreshMac() {
        std::scoped_lock lock{attach_mutex};
        if (mac_fn) {
            own_mac = mac_fn();
        }
    }

    MacAddress OwnMac() {
        std::scoped_lock lock{attach_mutex};
        return own_mac;
    }

    void Deliver(const WifiPacket& p) {
        std::function<void(const WifiPacket&)> fn;
        {
            std::scoped_lock lock{attach_mutex};
            fn = deliver_fn;
        }
        if (fn) {
            fn(p);
        }
    }

    void DeliverData(const MacAddress& from, const MacAddress& to, u8 channel, std::vector<u8> data) {
        WifiPacket w;
        w.type = WifiPacket::PacketType::Data;
        w.transmitter_address = from;
        w.destination_address = to;
        w.channel = channel;
        w.data = std::move(data);
        Deliver(w);
    }

    void Capture(const char* dir, int src, int dst, std::span<const u8> in, std::span<const u8> out) {
        if (!Settings::values.mhgu_hub_log.GetValue() || captures >= 4000) {
            return;
        }
        captures++;
        const bool same = in.size() == out.size() && !std::memcmp(in.data(), out.data(), in.size());
        LOG_INFO(Service_NWM, "[HUBCAP] {} {}>{} len={} {} data={}", dir, src, dst, in.size(),
                 same ? "same" : ("-> " + std::to_string(out.size()) + " " + HexOf(out)).c_str(), HexOf(in));
    }

    void Tick() {
        const auto now = Clock::now();
        if (sta == Sta::Associating && now - assoc_started > 8s) {
            LOG_WARNING(Service_NWM, "[hub] the game did not finish connecting to the hub");
            sta = Sta::Idle;
        }
        // host: players announced by the server whose UDS join didn't finish
        for (auto it = pending.begin(); it != pending.end();) {
            if (now - it->start > 4s) {
                LOG_WARNING(Service_NWM, "[hub] {} could not join the room (the game did not accept them)",
                            it->hello.name.data());
                Refuse(it->sp.sock, H::R_CONNECT_REJECTED);
                SendDeauth(it->mac);
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
        // host: relayed links, until the player's datagrams come through
        if (hosting && host_udp != H::BadSock && now >= next_register) {
            next_register = now + 1s;
            for (auto& s : st) {
                if (s.alive && s.pair && !UdpUsable(s.udp_ok, s.udp_rx)) {
                    H::UdpSendTo(host_udp, s.uaddr, H::EncodeRelayRegister(s.pair, s.secret));
                }
            }
        }
    }

    static bool UdpUsable(bool ok, Clock::time_point rx) {
        return ok && Clock::now() - rx < 15s;
    }

    // ---- frames from the UDS service
    void OnUdsOut(const WifiPacket& p) {
        using PT = WifiPacket::PacketType;
        switch (p.type) {
        case PT::Authentication:
            if (p.data.size() >= sizeof(AuthenticationFrame) &&
                GetAuthenticationSeqNumber(p.data) == AuthenticationSeq::SEQ1) {
                StationAuth(p);
            }
            return;
        case PT::AssociationResponse:
            HostAssociated(p);
            return;
        case PT::Deauthentication:
            if (sta != Sta::Idle && p.destination_address == sta_hub.mac) {
                LOG_INFO(Service_NWM, "[hub] left the hub");
                StationStop(false);
            } else if (hosting) {
                for (int i = 1; i < H::MaxNodes; i++) {
                    if (st[i].alive && (p.destination_address == st[i].mac ||
                                        p.destination_address == Network::BroadcastMac)) {
                        const u32 r = H::DR_REJECTED;
                        H::FrameSend(st[i].sock, H::F_DESTROY, {reinterpret_cast<const u8*>(&r), 4});
                        H::ShutdownSock(st[i].sock);
                    }
                }
            }
            return;
        case PT::Data:
            break;
        default: // beacons, node maps, net sync: the bridge has its own
            return;
        }
        if (p.data.size() < sizeof(LLCHeader) + 2) {
            return;
        }
        if (GetFrameEtherType(p.data) == EtherType::EAPoL) {
            const u16 t = GetEAPoLFrameType(p.data);
            if (t == EAPoLStartMagic) {
                StationEapolStart(p);
            } else if (t == EAPoLLogoffMagic) {
                HostLogoff(p);
            }
            return;
        }
        if (GetFrameEtherType(p.data) != EtherType::SecureData ||
            p.data.size() < sizeof(LLCHeader) + sizeof(SecureDataHeader)) {
            return;
        }
        const auto hdr = ParseSecureDataHeader(p.data);
        if (hdr.data_channel == HeartbeatChannel) {
            return;
        }
        const std::size_t off = sizeof(LLCHeader) + sizeof(SecureDataHeader);
        const std::size_t size = std::min<std::size_t>(hdr.GetActualDataSize(), p.data.size() - off);
        const std::span<const u8> payload(p.data.data() + off, size);
        const u16 dest = hdr.dest_node_id, src = hdr.src_node_id;
        if (hosting) {
            if (src != 1) {
                return; // (the UDS host passing on a hub player's packet: the bridge routes those itself)
            }
            data_channel = hdr.data_channel;
            const auto out = pia.ToMhgu(payload, dest == BroadcastNode ? -1 : dest - 1);
            Capture("3DS>MHGU", src, dest, payload, out);
            H::VPacket vp{host_subnet | 1, dest == BroadcastNode ? host_subnet | 0xFF : host_subnet | dest,
                          H::PiaPort, H::PiaPort, H::VPROTO_UDP, 0};
            for (int i = 1; i < H::MaxNodes; i++) {
                if (st[i].alive && (dest == BroadcastNode || dest == i + 1)) {
                    HostSendTo(i, vp, out);
                }
            }
        } else if (sta == Sta::Connected) {
            if (src != my_slot + 1) {
                return;
            }
            data_channel = hdr.data_channel;
            const auto out = pia.ToMhgu(payload, dest == BroadcastNode ? -1 : dest - 1);
            Capture("3DS>MHGU", src, dest, payload, out);
            H::VPacket vp{my_ip, dest == BroadcastNode ? sta_subnet | 0xFF : sta_subnet | dest, H::PiaPort,
                          H::PiaPort, H::VPROTO_UDP, 0};
            StationSend(vp, out);
        }
    }

    // ================= scanning and joining a hub (the 3DS is a client, an MHGU player hosts)
    struct Listed {
        H::ListedHub hub;
        MacAddress mac{};
        u32 network_id = 0;
        u32 session_param = 0;
    };

    void RequestList() {
        list_pending = true;
        last_list = Clock::now();
        if (list_thread.joinable()) {
            list_thread.join();
        }
        const auto servers = H::ParseServerList(Servers());
        const std::string key = Key();
        list_thread = azl::SafeThread([this, servers, key] {
            std::vector<std::vector<H::ListedHub>> found(servers.size());
            std::vector<H::ServerState> states(servers.size(), H::ServerState::NoAnswer);
            std::vector<azl::SafeThread> ts;
            for (std::size_t i = 0; i < servers.size(); i++) {
                const std::function<void()> ask = [&, i] { states[i] = H::ListHubs(servers[i], key, 1500, found[i]); };
                ts.emplace_back(ask);
                if (!ts.back().joinable()) {
                    ask(); // no thread for this server: ask it here (slower, but the list still comes)
                }
            }
            for (auto& t : ts) {
                t.join();
            }
            std::vector<H::ListedHub> all;
            for (std::size_t i = 0; i < servers.size(); i++) {
                if (states[i] != H::ServerState::Online) {
                    LOG_DEBUG(Service_NWM, "[hub] server {}: no list", servers[i]);
                }
                for (auto& h : found[i]) {
                    const bool dup = std::any_of(all.begin(), all.end(), [&](const H::ListedHub& o) {
                        return !std::memcmp(&o.info[H::NI::Bssid], &h.info[H::NI::Bssid], 6);
                    });
                    if (!dup) {
                        all.push_back(h);
                    }
                }
            }
            Post([this, all] { OnList(all); });
        });
        if (!list_thread.joinable()) {
            LOG_WARNING(Service_NWM, "[hub] could not start the list thread: no hub list this time");
        }
    }

    void OnList(std::vector<H::ListedHub> hubs) {
        list_pending = false;
        std::vector<Listed> next;
        for (auto& h : hubs) {
            if (H::Rd64(&h.info[H::NI::LocalCommId]) != H::MhguLocalCommId || !H::InfoSane(h.info) ||
                h.info[H::NI::AcceptPolicy] == 1 || h.info[H::NI::NodeCount] >= h.info[H::NI::NodeMax]) {
                continue;
            }
            Listed l;
            l.hub = h;
            l.mac = MacFrom(&h.info[H::NI::Bssid]);
            const auto old = std::find_if(listed.begin(), listed.end(), [&](const Listed& o) {
                return o.hub.id == h.id && o.hub.server_name == h.server_name;
            });
            l.network_id = old != listed.end() ? old->network_id : (Rand32() | 1);
            l.session_param = old != listed.end() ? old->session_param : (Rand32() | 1);
            next.push_back(l);
        }
        if (next.size() != listed.size()) {
            LOG_INFO(Service_NWM, "[hub] {} MHGU room(s) on the hub servers", next.size());
        }
        listed = std::move(next);
    }

    std::vector<NodeInfo> NodesOf(const H::NetInfo& info, int count) {
        std::vector<NodeInfo> nodes(count);
        for (int i = 0; i < count && i < H::MaxNodes; i++) {
            const u8* nd = H::NodeAt(info, i);
            if (!nd[H::ND::Connected]) {
                continue;
            }
            nodes[i].friend_code_seed = H::SeedForAddress(H::Rd32(nd + H::ND::Ip));
            SetUsername(nodes[i], NodeName(info, i));
            nodes[i].network_node_id = static_cast<u16>(i + 1);
        }
        return nodes;
    }

    NetworkInfo UdsInfoOf(const Listed& l) {
        NetworkInfo ni{};
        ni.host_mac_address = l.mac;
        ni.channel = HubChannel;
        ni.initialized = 1;
        ni.oui_value = NintendoOUI;
        ni.oui_type = static_cast<u8>(NintendoTagId::NetworkInfo);
        ni.wlan_comm_id = scan_comm;
        ni.id = scan_id;
        ni.attributes = MhxxAttributes;
        ni.network_id = l.network_id;
        ni.total_nodes = l.hub.info[H::NI::NodeCount];
        ni.max_nodes = l.hub.info[H::NI::NodeMax];
        const u16 adv_size = H::Rd16(&l.hub.info[H::NI::AdvSize]);
        const auto app = H::AdvToUdsAppData({&l.hub.info[H::NI::Adv], adv_size}, l.session_param);
        ni.application_data_size = static_cast<u8>(std::min<std::size_t>(app.size(), ApplicationDataSize));
        std::memcpy(ni.application_data.data(), app.data(), ni.application_data_size);
        return ni;
    }

    void DeliverBeacons() {
        std::size_t n = 0;
        for (const auto& l : listed) {
            if (n++ >= MaxBeacons) {
                break;
            }
            const NetworkInfo ni = UdsInfoOf(l);
            WifiPacket w;
            w.type = WifiPacket::PacketType::Beacon;
            w.data = GenerateBeaconFrame(ni, NodesOf(l.hub.info, ni.max_nodes));
            w.transmitter_address = l.mac;
            w.destination_address = Network::BroadcastMac;
            w.channel = HubChannel;
            Deliver(w);
        }
    }

    // The game connects to a hub: it is accepted at once (UDS-wise), and joined for real when its EAPoL-Start
    // tells who is joining.
    void StationAuth(const WifiPacket& p) {
        if (sta != Sta::Idle || hosting) {
            return;
        }
        const auto it = std::find_if(listed.begin(), listed.end(),
                                     [&](const Listed& l) { return l.mac == p.destination_address; });
        if (it == listed.end()) {
            return;
        }
        sta = Sta::Associating;
        sta_hub = *it;
        my_mac = p.transmitter_address;
        assoc_started = Clock::now();
        WifiPacket auth;
        auth.type = WifiPacket::PacketType::Authentication;
        auth.data = GenerateAuthenticationFrame(AuthenticationSeq::SEQ2);
        auth.transmitter_address = sta_hub.mac;
        auth.destination_address = my_mac;
        auth.channel = HubChannel;
        Deliver(auth);
        WifiPacket assoc;
        assoc.type = WifiPacket::PacketType::AssociationResponse;
        assoc.data = GenerateAssocResponseFrame(AssocStatus::Successful, 1, sta_hub.network_id);
        assoc.transmitter_address = sta_hub.mac;
        assoc.destination_address = my_mac;
        assoc.channel = HubChannel;
        Deliver(assoc);
    }

    void StationEapolStart(const WifiPacket& p) {
        if (sta != Sta::Associating || p.destination_address != sta_hub.mac) {
            return;
        }
        my_node = DeserializeNodeInfoFromFrame(p.data);
        sta = Sta::Joining;
        const u64 gen = ++station_gen;
        H::Hello hello;
        hello.lcv = H::Rd16(H::NodeAt(sta_hub.hub.info, 0) + H::ND::Lcv);
        if (!hello.lcv) {
            hello.lcv = H::MhguLcv;
        }
        std::memcpy(hello.mac.data(), my_mac.data(), 6);
        const std::string name = UsernameOf(my_node);
        std::memcpy(hello.name.data(), name.data(), std::min<std::size_t>(name.size(), 0x20));
        const H::ListedHub hub = sta_hub.hub;
        const std::string key = Key();
        LOG_INFO(Service_NWM, "[hub] joining {}'s room through {}", NodeName(hub.info, 0), hub.server_name);
        if (join_thread.joinable()) {
            join_thread.join();
        }
        join_thread = azl::SafeThread([this, gen, hub, key, hello] {
            H::Spliced sp;
            const u32 err = H::JoinHub(hub.server, key, hub.id, sp);
            if (err) {
                Post([this, gen, err] { StationJoinFailed(gen, H::LobbyErrorText(err)); });
                return;
            }
            const auto hb = H::EncodeHello(hello);
            H::SetRecvTimeout(sp.sock, 5000);
            u8 type = 0;
            std::vector<u8> buf;
            const int n = H::FrameSend(sp.sock, H::F_HELLO, hb) ? H::FrameRecv(sp.sock, type, buf) : -1;
            H::SetRecvTimeout(sp.sock, 0);
            if (n < 4) {
                H::CloseSock(sp.sock);
                Post([this, gen] { StationJoinFailed(gen, "the host did not answer"); });
                return;
            }
            Post([this, gen, sp, type, buf] { StationJoined(gen, sp, type, buf); });
        });
        if (!join_thread.joinable()) {
            Post([this, gen] { StationJoinFailed(gen, "no thread to join with"); }); // (the game gets an answer, not a hang)
        }
    }

    static const char* RefuseReason(u32 r) {
        switch (r) {
        case H::R_TOO_MANY_PLAYERS:
            return "the room is full";
        case H::R_CONNECT_REJECTED:
            return "the room is closed to new players";
        case H::R_VERSION_TOO_LOW:
        case H::R_VERSION_TOO_HIGH:
            return "different game version";
        default:
            return "unknown reason";
        }
    }

    void StationJoinFailed(u64 gen, const char* why) {
        if (gen != station_gen) {
            return;
        }
        LOG_WARNING(Service_NWM, "[hub] could not join the room: {}", why);
        sta = Sta::Idle;
    }

    void StationJoined(u64 gen, H::Spliced sp, u8 type, std::vector<u8> buf) {
        if (gen != station_gen || sta != Sta::Joining) {
            H::CloseSock(sp.sock);
            return;
        }
        if (type == H::F_REFUSE) {
            H::CloseSock(sp.sock);
            const u32 r = buf.size() >= 4 ? H::Rd32(buf.data()) : 0;
            LOG_WARNING(Service_NWM, "[hub] the host refused: {} ({:#x})", RefuseReason(r), r);
            sta = Sta::Idle;
            if (!list_pending) {
                RequestList(); // the listing was out of date: get a fresh one for the next search
            }
            return;
        }
        H::NetInfo info{};
        const int node = buf.empty() ? 0 : buf[0];
        bool ok = type == H::F_WELCOME && buf.size() >= 8 + H::NetInfoSize && node >= 1 && node < H::MaxNodes;
        if (ok) {
            std::memcpy(info.data(), buf.data() + 8, H::NetInfoSize);
            ok = H::InfoSane(info) && H::NodeAt(info, node)[H::ND::Connected];
        }
        if (!ok) {
            H::CloseSock(sp.sock);
            LOG_WARNING(Service_NWM, "[hub] unexpected answer from the host (frame {}, {} bytes)", type, buf.size());
            sta = Sta::Idle;
            return;
        }
        up = sp.sock;
        up_pair = sp.pair;
        up_secret = sp.secret;
        up_token = H::Rd32(buf.data() + 4);
        sta_info = info;
        my_slot = node;
        my_ip = H::Rd32(H::NodeAt(info, node) + H::ND::Ip);
        sta_subnet = my_ip & 0xFFFFFF00u;
        pia.Reset(Settings::values.mhgu_hub_convert.GetValue());
        pia.SetSelfIp(my_ip);
        pia.SetSubnet(sta_subnet);
        captures = 0;
        sta = Sta::Connected;
        up_reader = azl::SafeThread([this, gen, s = up] {
            u8 t = 0;
            std::vector<u8> b;
            int n;
            while ((n = H::FrameRecv(s, t, b)) >= 0) {
                if (t == H::F_PACKET && n >= static_cast<int>(H::VPacketSize)) {
                    const auto vp = H::DecodeVPacket(b.data());
                    std::vector<u8> d(b.begin() + H::VPacketSize, b.begin() + n);
                    Post([this, gen, vp, d] { StationPacket(gen, vp, d); });
                } else if (t == H::F_INFO && n >= static_cast<int>(H::NetInfoSize)) {
                    H::NetInfo i;
                    std::memcpy(i.data(), b.data(), H::NetInfoSize);
                    Post([this, gen, i] { StationInfo(gen, i); });
                } else if (t == H::F_DESTROY && n >= 4) {
                    LOG_INFO(Service_NWM, "[hub] the host closed the room (reason {})", H::Rd32(b.data()));
                }
            }
            Post([this, gen] {
                if (gen == station_gen && sta == Sta::Connected) {
                    LOG_INFO(Service_NWM, "[hub] the connection to the room's host ended");
                    StationStop(true);
                }
            });
        });
        // datagrams to the host through the server's relay (else the packets use the link)
        up_udp = H::UdpOpenConnected(sta_hub.hub.server);
        if (up_udp != H::BadSock) {
            up_udp_run = true;
            up_udp_ok = false;
            up_udp_thread = azl::SafeThread([this, gen, s = up_udp, pair = up_pair, secret = up_secret,
                                         token = up_token] {
                auto next_hello = Clock::now();
                std::vector<u8> buf;
                while (up_udp_run) {
                    if (Clock::now() >= next_hello) {
                        H::UdpSend(s, H::EncodeDatagram(pair, secret, token, H::U_HELLO, nullptr, {}));
                        next_hello = Clock::now() + (up_udp_ok ? 2s : 500ms);
                    }
                    const int n = H::UdpRecv(s, buf, nullptr, 250);
                    if (n <= 0) {
                        continue;
                    }
                    u32 tok = 0;
                    u8 kind = 0;
                    H::VPacket vp;
                    std::span<const u8> d;
                    if (!H::DecodeDatagram(buf, tok, kind, vp, d) || tok != token) {
                        continue;
                    }
                    up_udp_rx_ms = Clock::now().time_since_epoch() / 1ms;
                    if (kind == H::U_HELLO_ACK && !up_udp_ok) {
                        up_udp_ok = true;
                        LOG_INFO(Service_NWM, "[hub] UDP to the host works: game packets go by UDP");
                    } else if (kind == H::U_DATA) {
                        std::vector<u8> data(d.begin(), d.end());
                        Post([this, gen, vp, data] { StationPacket(gen, vp, data); });
                    }
                }
            });
        }
        LOG_INFO(Service_NWM, "[hub] joined {}'s room through {} as {} (node {})", NodeName(info, 0),
                 sta_hub.hub.server_name, IpString(my_ip), my_slot + 1);
        // the UDS side: the host accepts the game as node my_slot + 1, with the hub's players as the others
        my_node.network_node_id = static_cast<u16>(my_slot + 1);
        StationLogoff();
        StationNodeMap();
    }

    std::vector<NodeInfo> StationNodes() {
        const int max = sta_info[H::NI::NodeMax];
        auto nodes = NodesOf(sta_info, max);
        if (my_slot < max) {
            nodes[my_slot] = my_node;
            nodes[my_slot].network_node_id = static_cast<u16>(my_slot + 1);
        }
        return nodes;
    }

    void StationLogoff() {
        const int max = sta_info[H::NI::NodeMax];
        const auto nodes = StationNodes();
        DeliverData(sta_hub.mac, my_mac, HubChannel,
                    GenerateEAPoLLogoffFrame(my_mac, static_cast<u16>(my_slot + 1), nodes, static_cast<u8>(max),
                                             sta_info[H::NI::NodeCount]));
    }

    void StationNodeMap() {
        std::vector<std::pair<MacAddress, u16>> entries;
        for (int i = 0; i < H::MaxNodes; i++) {
            if (H::NodeAt(sta_info, i)[H::ND::Connected]) {
                entries.emplace_back(i == my_slot ? my_mac : MacFrom(H::NodeAt(sta_info, i) + H::ND::Mac),
                                     static_cast<u16>(i + 1));
            }
        }
        WifiPacket w;
        w.type = WifiPacket::PacketType::NodeMap;
        w.transmitter_address = sta_hub.mac;
        w.destination_address = Network::BroadcastMac;
        w.channel = HubChannel;
        const std::size_t count = entries.size();
        w.data.resize(sizeof(count) + entries.size() * (sizeof(MacAddress) + sizeof(u16)));
        std::memcpy(w.data.data(), &count, sizeof(count));
        std::size_t o = sizeof(count);
        for (const auto& [mac, id] : entries) {
            std::memcpy(w.data.data() + o, mac.data(), mac.size());
            std::memcpy(w.data.data() + o + mac.size(), &id, sizeof(id));
            o += mac.size() + sizeof(id);
        }
        Deliver(w);
    }

    MacAddress StationMacOf(int node) { // node: 1-based
        if (node == 1) {
            return sta_hub.mac;
        }
        return MacFrom(H::NodeAt(sta_info, node - 1) + H::ND::Mac);
    }

    void StationInfo(u64 gen, H::NetInfo info) {
        if (gen != station_gen || sta != Sta::Connected || !H::InfoSane(info)) {
            return;
        }
        bool changed = false;
        for (int i = 0; i < H::MaxNodes; i++) {
            const u8* a = H::NodeAt(info, i);
            const u8* b = H::NodeAt(sta_info, i);
            changed |= a[H::ND::Connected] != b[H::ND::Connected] || std::memcmp(a + H::ND::Mac, b + H::ND::Mac, 6);
        }
        sta_info = info;
        if (changed) {
            LOG_INFO(Service_NWM, "[hub] players in the room: {}", info[H::NI::NodeCount]);
            StationLogoff();
            StationNodeMap();
        }
    }

    void StationSend(const H::VPacket& vp, std::span<const u8> data) {
        const bool udp = up_udp != H::BadSock && up_udp_ok &&
                         Clock::now().time_since_epoch() / 1ms - up_udp_rx_ms.load() < 15000;
        if (udp && data.size() <= H::UdpMaxPayload &&
            H::UdpSend(up_udp, H::EncodeDatagram(up_pair, up_secret, up_token, H::U_DATA, &vp, data))) {
            return;
        }
        u8 v[H::VPacketSize];
        H::EncodeVPacket(v, vp);
        H::FrameSend(up, H::F_PACKET, v, data);
    }

    void StationPacket(u64 gen, H::VPacket vp, std::vector<u8> data) {
        if (gen != station_gen || sta != Sta::Connected || vp.proto != H::VPROTO_UDP || vp.dst_port != H::PiaPort) {
            return;
        }
        const bool bc = (vp.dst_ip & 0xFF) == 0xFF || vp.dst_ip == 0xFFFFFFFFu;
        if (!bc && vp.dst_ip != my_ip) {
            return;
        }
        const int src = static_cast<int>(vp.src_ip & 0xFF);
        if (src < 1 || src > H::MaxNodes || src == my_slot + 1) {
            return;
        }
        const auto out = pia.To3ds(data);
        Capture("MHGU>3DS", src, bc ? 0xFFFF : my_slot + 1, data, out);
        DeliverData(StationMacOf(src), bc ? Network::BroadcastMac : my_mac, HubChannel,
                    GenerateDataPayload(out, data_channel, bc ? BroadcastNode : static_cast<u16>(my_slot + 1),
                                        static_cast<u16>(src), 0));
    }

    void StationStop(bool lost) {
        station_gen++;
        const bool was = sta == Sta::Connected;
        sta = Sta::Idle;
        if (up != H::BadSock) {
            H::ShutdownSock(up);
        }
        if (up_reader.joinable()) {
            up_reader.join();
        }
        H::CloseSock(up);
        up = H::BadSock;
        up_udp_run = false;
        if (up_udp_thread.joinable()) {
            up_udp_thread.join();
        }
        H::CloseSock(up_udp);
        up_udp = H::BadSock;
        if (join_thread.joinable()) {
            join_thread.join();
        }
        if (was && lost) {
            std::function<void()> fn;
            {
                std::scoped_lock lock{attach_mutex};
                fn = lost_fn;
            }
            if (fn) {
                fn();
            }
        }
    }

    // ================= hosting (the 3DS hosts, MHGU players join)
    struct Station {
        bool alive = false;
        H::Sock sock = H::BadSock;
        azl::SafeThread reader;
        u32 ip = 0;
        MacAddress mac{};
        std::string name;
        u16 lcv = H::MhguLcv;
        u32 token = 0, pair = 0;
        u64 secret = 0;
        H::Endpoint uaddr; // where its datagrams go (the server's relay)
        bool udp_ok = false;
        Clock::time_point udp_rx{};
    };
    struct Pending {
        H::Spliced sp;
        H::Endpoint server;
        H::Hello hello;
        MacAddress mac{};
        Clock::time_point start;
        int node = 0; // the UDS node the game will give it (the lowest free one)
    };

    void HostStart(const NetworkInfo& info, const std::vector<NodeInfo>& nodes) {
        host_gen++;
        hosting = true;
        host_ni = info;
        host_nodes = nodes;
        host_subnet = 0xA9FE0000u | (1 + Rand32() % 254) << 8;
        for (auto& b : host_session) {
            b = static_cast<u8>(Rand32());
        }
        for (auto& b : host_sec) {
            b = static_cast<u8>(Rand32());
        }
        char ssid[33];
        std::snprintf(ssid, sizeof ssid, "MHGURecomp%08x%08x", static_cast<unsigned>(Rand32()), static_cast<unsigned>(Rand32()));
        host_ssid = ssid;
        host_auth = static_cast<u64>(Rand32()) << 32 | Rand32();
        pia.Reset(Settings::values.mhgu_hub_convert.GetValue());
        pia.SetSelfIp(host_subnet | 1);
        pia.SetSubnet(host_subnet);
        captures = 0;
        for (auto& s : st) {
            s.alive = false;
        }
        host_udp = H::UdpOpen();
        if (host_udp != H::BadSock) {
            host_udp_run = true;
            const u64 gen = host_gen;
            host_udp_thread = azl::SafeThread([this, gen, s = host_udp] {
                std::vector<u8> buf;
                while (host_udp_run) {
                    H::Endpoint from;
                    if (H::UdpRecv(s, buf, &from, 250) <= 0) {
                        continue;
                    }
                    u32 token = 0;
                    u8 kind = 0;
                    H::VPacket vp;
                    std::span<const u8> d;
                    if (!H::DecodeDatagram(buf, token, kind, vp, d)) {
                        continue;
                    }
                    std::vector<u8> data(d.begin(), d.end());
                    Post([this, gen, token, kind, vp, data, from] { HostDatagram(gen, token, kind, vp, data, from); });
                }
            });
        }
        HostRebuild(-1);
        const u64 gen = host_gen;
        listing.Start(
            H::ParseServerList(Servers()), Key(),
            [this] {
                std::scoped_lock lock{host_info_mutex};
                return host_info;
            },
            [this, gen](H::Spliced sp, H::Endpoint server) { HostIncoming(gen, sp, server); });
        LOG_INFO(Service_NWM, "[hub] hosting: the room is listed on the hub servers for MHGU players ({})",
                 IpString(host_subnet | 1));
    }

    void HostStop() {
        if (!hosting) {
            return;
        }
        host_gen++;
        hosting = false;
        listing.Stop();
        for (int i = 1; i < H::MaxNodes; i++) {
            auto& s = st[i];
            if (s.sock != H::BadSock) {
                const u32 r = H::DR_DESTROYED_BY_USER;
                H::FrameSend(s.sock, H::F_DESTROY, {reinterpret_cast<const u8*>(&r), 4});
                H::ShutdownSock(s.sock);
            }
            if (s.reader.joinable()) {
                s.reader.join();
            }
            H::CloseSock(s.sock);
            s.sock = H::BadSock;
            s.alive = false;
        }
        for (auto& p : pending) {
            Refuse(p.sp.sock, H::R_CONNECT_REJECTED);
        }
        pending.clear();
        host_udp_run = false;
        if (host_udp_thread.joinable()) {
            host_udp_thread.join();
        }
        H::CloseSock(host_udp);
        host_udp = H::BadSock;
        LOG_INFO(Service_NWM, "[hub] the room is closed");
    }

    // The hub's NetworkInfo for the hosted network: MHGU's ids, the 3DS as node 0, the joined players.
    H::NetInfo HostBuildInfo() {
        H::NetInfo info{};
        H::Wr64(&info[H::NI::LocalCommId], H::MhguLocalCommId);
        std::memcpy(&info[H::NI::SessionId], host_session.data(), 16);
        const MacAddress mac = OwnMac();
        std::memcpy(&info[H::NI::Bssid], mac.data(), 6);
        info[H::NI::Ssid] = static_cast<u8>(host_ssid.size());
        std::memcpy(&info[H::NI::Ssid + 1], host_ssid.data(), host_ssid.size());
        H::Wr16(&info[H::NI::Channel], 6);
        info[H::NI::LinkLevel] = 3;
        info[H::NI::NetworkType] = 2;
        std::memcpy(&info[H::NI::SecParam], host_sec.data(), 16);
        H::Wr16(&info[H::NI::SecMode], 1);
        info[H::NI::AcceptPolicy] = 0;
        const int max = std::clamp<int>(host_ni.max_nodes, 2, H::MaxNodes);
        info[H::NI::NodeMax] = static_cast<u8>(max);
        int count = 1;
        u8* nd = H::NodeAt(info, 0);
        H::Wr32(nd + H::ND::Ip, host_subnet | 1);
        std::memcpy(nd + H::ND::Mac, mac.data(), 6);
        nd[H::ND::Id] = 0;
        nd[H::ND::Connected] = 1;
        const std::string me = host_nodes.empty() ? "3DS Hunter" : UsernameOf(host_nodes[0]);
        std::memcpy(nd + H::ND::Name, me.data(), std::min<std::size_t>(me.size(), 0x20));
        H::Wr16(nd + H::ND::Lcv, H::MhguLcv);
        for (int i = 1; i < max; i++) {
            if (!st[i].alive) {
                continue;
            }
            nd = H::NodeAt(info, i);
            H::Wr32(nd + H::ND::Ip, st[i].ip);
            std::memcpy(nd + H::ND::Mac, st[i].mac.data(), 6);
            nd[H::ND::Id] = static_cast<u8>(i);
            nd[H::ND::Connected] = 1;
            std::memcpy(nd + H::ND::Name, st[i].name.data(), std::min<std::size_t>(st[i].name.size(), 0x20));
            H::Wr16(nd + H::ND::Lcv, st[i].lcv);
            count++;
        }
        info[H::NI::NodeCount] = static_cast<u8>(count);
        const auto adv = H::UdsAppDataToAdv({host_ni.application_data.data(), host_ni.application_data_size});
        const std::size_t adv_size = std::min<std::size_t>(adv.size(), H::AdvMax);
        H::Wr16(&info[H::NI::AdvSize], static_cast<u16>(adv_size));
        std::memcpy(&info[H::NI::Adv], adv.data(), adv_size);
        H::Wr64(&info[H::NI::AuthId], host_auth);
        return info;
    }

    // Rebuild the hub's NetworkInfo; when it changed, tell the players (except one) and the hub servers.
    void HostRebuild(int except) {
        const H::NetInfo info = HostBuildInfo();
        {
            std::scoped_lock lock{host_info_mutex};
            if (info == host_info) {
                return;
            }
            host_info = info;
        }
        listing.MarkChanged();
        for (int i = 1; i < H::MaxNodes; i++) {
            if (i != except && st[i].alive) {
                H::FrameSend(st[i].sock, H::F_INFO, info);
            }
        }
    }

    static void Refuse(H::Sock s, u32 result) {
        if (s == H::BadSock) {
            return;
        }
        H::FrameSend(s, H::F_REFUSE, {reinterpret_cast<const u8*>(&result), 4});
        H::CloseSock(s);
    }

    void SendDeauth(const MacAddress& from) {
        WifiPacket w;
        w.type = WifiPacket::PacketType::Deauthentication;
        w.transmitter_address = from;
        w.destination_address = OwnMac();
        w.channel = host_ni.channel ? host_ni.channel : HubChannel;
        Deliver(w);
    }

    // a player the server put through (attach thread): its hello, then the UDS join on the worker
    void HostIncoming(u64 gen, H::Spliced sp, H::Endpoint server) {
        H::SetRecvTimeout(sp.sock, 5000);
        u8 type = 0;
        std::vector<u8> buf;
        const int n = H::FrameRecv(sp.sock, type, buf);
        H::Hello hello;
        if (n < static_cast<int>(H::HelloSize) || type != H::F_HELLO) {
            H::CloseSock(sp.sock);
            return;
        }
        if (!H::DecodeHello(buf, hello)) {
            Refuse(sp.sock, H::R_CONNECT_REJECTED);
            return;
        }
        H::SetRecvTimeout(sp.sock, 0);
        if (hello.scan_only) {
            H::NetInfo info;
            {
                std::scoped_lock lock{host_info_mutex};
                info = host_info;
            }
            H::FrameSend(sp.sock, H::F_INFO, info);
            H::CloseSock(sp.sock);
            return;
        }
        Post([this, gen, sp, server, hello] { HostHello(gen, sp, server, hello); });
    }

    void HostHello(u64 gen, H::Spliced sp, H::Endpoint server, H::Hello hello) {
        if (gen != host_gen || !hosting) {
            Refuse(sp.sock, H::R_CONNECT_REJECTED);
            return;
        }
        int count = 1 + static_cast<int>(pending.size());
        for (const auto& s : st) {
            count += s.alive ? 1 : 0;
        }
        if (hello.lcv < H::MhguLcv || hello.lcv > H::MhguLcv) {
            Refuse(sp.sock, hello.lcv < H::MhguLcv ? H::R_VERSION_TOO_LOW : H::R_VERSION_TOO_HIGH);
            return;
        }
        if (count >= std::min<int>(host_ni.max_nodes, H::MaxNodes)) {
            Refuse(sp.sock, H::R_TOO_MANY_PLAYERS);
            return;
        }
        if (!pending.empty()) { // one at a time: the next one's UDS node depends on this one's
            Post([this, gen, sp, server, hello] {
                azl::BridgeSleepMs(50);
                HostHello(gen, sp, server, hello);
            });
            return;
        }
        int node = 2;
        while (node <= H::MaxNodes && st[node - 1].alive) {
            node++;
        }
        MacAddress mac;
        std::memcpy(mac.data(), hello.mac.data(), 6);
        auto taken = [&](const MacAddress& m) {
            if (m == OwnMac() || m == MacAddress{} || m == Network::BroadcastMac) {
                return true;
            }
            for (const auto& s : st) {
                if (s.alive && s.mac == m) {
                    return true;
                }
            }
            return std::any_of(pending.begin(), pending.end(), [&](const Pending& p) { return p.mac == m; });
        };
        while (taken(mac)) {
            for (auto& b : mac) {
                b = static_cast<u8>(Rand32());
            }
            mac[0] = static_cast<u8>((mac[0] & 0xFC) | 0x02);
        }
        pending.push_back({sp, server, hello, mac, Clock::now(), node});
        LOG_INFO(Service_NWM, "[hub] {} is joining the room", hello.name.data());
        // the UDS side: an 802.11 authentication from the player, as a 3DS joining would send
        WifiPacket auth;
        auth.type = WifiPacket::PacketType::Authentication;
        auth.data = GenerateAuthenticationFrame(AuthenticationSeq::SEQ1);
        auth.transmitter_address = mac;
        auth.destination_address = OwnMac();
        auth.channel = host_ni.channel ? host_ni.channel : HubChannel;
        Deliver(auth);
    }

    void HostAssociated(const WifiPacket& p) {
        const auto it = std::find_if(pending.begin(), pending.end(),
                                     [&](const Pending& x) { return x.mac == p.destination_address; });
        if (!hosting || it == pending.end() || p.data.size() < sizeof(AssociationResponseFrame)) {
            return;
        }
        const auto [status, assoc_id] = GetAssociationResult(p.data);
        if (status != AssocStatus::Successful) {
            return;
        }
        NodeInfo node{};
        node.friend_code_seed = H::SeedForAddress(host_subnet | static_cast<u32>(it->node));
        SetUsername(node, it->hello.name.data());
        DeliverData(it->mac, OwnMac(), host_ni.channel ? host_ni.channel : HubChannel,
                    GenerateEAPoLStartFrame(assoc_id, ConnectionType::Client, node));
    }

    void HostLogoff(const WifiPacket& p) {
        const auto it = std::find_if(pending.begin(), pending.end(),
                                     [&](const Pending& x) { return x.mac == p.destination_address; });
        if (!hosting || it == pending.end() || p.data.size() < sizeof(LLCHeader) + sizeof(EAPoLLogoffPacket)) {
            return; // (copies of the update for players already in: the hub tells them itself)
        }
        const auto logoff = ParseEAPoLLogoffFrame(p.data);
        const int id = static_cast<u16>(logoff.assigned_node_id);
        const int slot = id - 1;
        Pending pd = *it;
        pending.erase(it);
        if (slot < 1 || slot >= H::MaxNodes || st[slot].alive || st[slot].reader.joinable() || id != pd.node) {
            LOG_WARNING(Service_NWM, "[hub] the game gave {} node {}, which the hub can't use", pd.hello.name.data(),
                        id);
            Refuse(pd.sp.sock, H::R_TOO_MANY_PLAYERS);
            SendDeauth(pd.mac);
            return;
        }
        auto& s = st[slot];
        s.alive = true;
        s.sock = pd.sp.sock;
        s.ip = host_subnet | static_cast<u32>(id);
        s.mac = pd.mac;
        s.name = std::string(pd.hello.name.data(), strnlen(pd.hello.name.data(), 0x20));
        s.lcv = pd.hello.lcv;
        s.token = Rand32() | 1;
        s.pair = pd.sp.pair;
        s.secret = pd.sp.secret;
        s.uaddr = H::Endpoint{pd.server.addr, pd.server.port};
        s.udp_ok = false;
        HostRebuild(slot);
        H::NetInfo info;
        {
            std::scoped_lock lock{host_info_mutex};
            info = host_info;
        }
        std::vector<u8> welcome(8 + H::NetInfoSize, 0);
        welcome[0] = static_cast<u8>(slot);
        H::Wr32(&welcome[4], s.token);
        std::memcpy(&welcome[8], info.data(), H::NetInfoSize);
        H::FrameSend(s.sock, H::F_WELCOME, welcome);
        if (s.pair && host_udp != H::BadSock) {
            H::UdpSendTo(host_udp, s.uaddr, H::EncodeRelayRegister(s.pair, s.secret));
        }
        const u64 gen = host_gen;
        s.reader = azl::SafeThread([this, gen, slot, sock = s.sock] {
            u8 t = 0;
            std::vector<u8> b;
            int n;
            while ((n = H::FrameRecv(sock, t, b)) >= 0) {
                if (t == H::F_PACKET && n >= static_cast<int>(H::VPacketSize)) {
                    const auto vp = H::DecodeVPacket(b.data());
                    std::vector<u8> d(b.begin() + H::VPacketSize, b.begin() + n);
                    Post([this, gen, slot, vp, d] { HostPacket(gen, slot, vp, d); });
                }
            }
            Post([this, gen, slot] { HostStationGone(gen, slot); });
        });
        LOG_INFO(Service_NWM, "[hub] {} joined the room as {} (node {})", s.name, IpString(s.ip), id);
    }

    void HostStationGone(u64 gen, int slot) {
        if (gen != host_gen) {
            return;
        }
        auto& s = st[slot];
        const bool was = s.alive;
        s.alive = false;
        if (s.reader.joinable()) {
            s.reader.join();
        }
        H::CloseSock(s.sock);
        s.sock = H::BadSock;
        if (was) {
            LOG_INFO(Service_NWM, "[hub] {} left the room", s.name);
            SendDeauth(s.mac);
            HostRebuild(-1);
        }
    }

    void HostDatagram(u64 gen, u32 token, u8 kind, H::VPacket vp, std::vector<u8> data, H::Endpoint from) {
        if (gen != host_gen || !hosting) {
            return;
        }
        for (int i = 1; i < H::MaxNodes; i++) {
            auto& s = st[i];
            if (!s.alive || s.token != token) {
                continue;
            }
            s.uaddr = from;
            s.udp_rx = Clock::now();
            if (kind == H::U_HELLO) {
                if (!s.udp_ok) {
                    LOG_INFO(Service_NWM, "[hub] {}: game packets go by UDP", s.name);
                }
                s.udp_ok = true;
                H::UdpSendTo(host_udp, from, H::EncodeDatagram(s.pair, s.secret, s.token, H::U_HELLO_ACK, nullptr, {}));
            } else if (kind == H::U_DATA) {
                vp.src_ip = s.ip;
                HostPacket(gen, i, vp, data);
            }
            return;
        }
    }

    void HostSendTo(int i, const H::VPacket& vp, std::span<const u8> data) {
        auto& s = st[i];
        if (host_udp != H::BadSock && UdpUsable(s.udp_ok, s.udp_rx) && data.size() <= H::UdpMaxPayload &&
            H::UdpSendTo(host_udp, s.uaddr, H::EncodeDatagram(s.pair, s.secret, s.token, H::U_DATA, &vp, data))) {
            return;
        }
        u8 v[H::VPacketSize];
        H::EncodeVPacket(v, vp);
        H::FrameSend(s.sock, H::F_PACKET, v, data);
    }

    // a hub player's packet: to the 3DS (node 1) and / or on to the other players
    void HostPacket(u64 gen, int slot, H::VPacket vp, std::vector<u8> data) {
        if (gen != host_gen || !hosting || !st[slot].alive) {
            return;
        }
        vp.src_ip = st[slot].ip;
        const bool bc = (vp.dst_ip & 0xFF) == 0xFF || vp.dst_ip == 0xFFFFFFFFu;
        if (vp.proto == H::VPROTO_UDP && vp.dst_port == H::PiaPort && (bc || vp.dst_ip == (host_subnet | 1))) {
            const auto out = pia.To3ds(data);
            Capture("MHGU>3DS", slot + 1, bc ? 0xFFFF : 1, data, out);
            DeliverData(st[slot].mac, bc ? Network::BroadcastMac : OwnMac(),
                        host_ni.channel ? host_ni.channel : HubChannel,
                        GenerateDataPayload(out, data_channel, bc ? BroadcastNode : 1, static_cast<u16>(slot + 1), 0));
        }
        for (int i = 1; i < H::MaxNodes; i++) {
            if (i != slot && st[i].alive && (bc || st[i].ip == vp.dst_ip)) {
                HostSendTo(i, vp, data);
            }
        }
    }

    // ---- state
    std::mutex task_mutex;
    std::condition_variable task_cv;
    std::deque<std::function<void()>> tasks;
    bool running = false;
    azl::SafeThread worker;

    std::mutex attach_mutex;
    std::function<void(const WifiPacket&)> deliver_fn;
    std::function<void()> lost_fn;
    std::function<MacAddress()> mac_fn;
    MacAddress own_mac{};

    H::PiaTranslator pia;
    u8 data_channel = DefaultDataChannel;
    int captures = 0;

    // scanning
    std::vector<Listed> listed;
    Clock::time_point last_list{};
    bool list_pending = false;
    azl::SafeThread list_thread;
    u32 scan_comm = MhxxCommId;
    u8 scan_id = 0;

    // joined to a hub
    enum class Sta { Idle, Associating, Joining, Connected } sta = Sta::Idle;
    u64 station_gen = 0;
    Listed sta_hub;
    NodeInfo my_node{};
    MacAddress my_mac{};
    Clock::time_point assoc_started{};
    azl::SafeThread join_thread;
    H::Sock up = H::BadSock;
    azl::SafeThread up_reader;
    u32 up_token = 0, up_pair = 0;
    u64 up_secret = 0;
    H::NetInfo sta_info{};
    int my_slot = 0;
    u32 my_ip = 0, sta_subnet = 0;
    H::Sock up_udp = H::BadSock;
    azl::SafeThread up_udp_thread;
    std::atomic<bool> up_udp_run{false}, up_udp_ok{false};
    std::atomic<s64> up_udp_rx_ms{0};

    // hosting
    bool hosting = false;
    u64 host_gen = 0;
    NetworkInfo host_ni{};
    std::vector<NodeInfo> host_nodes;
    u32 host_subnet = 0;
    std::array<u8, 16> host_session{}, host_sec{};
    std::string host_ssid;
    u64 host_auth = 0;
    std::mutex host_info_mutex;
    H::NetInfo host_info{};
    H::HubListing listing;
    std::array<Station, H::MaxNodes> st;
    std::vector<Pending> pending;
    H::Sock host_udp = H::BadSock;
    azl::SafeThread host_udp_thread;
    std::atomic<bool> host_udp_run{false};
    Clock::time_point next_register{};
};

Bridge& Get() {
    // (never destroyed: its threads may still run while the process exits; not a function-local static with a constructor: see azl/once.hpp)
    static azl::LazyNew<Bridge> bridge;
    return bridge.Get([] { azl::BridgeSleepMs(1); });
}

} // namespace

bool Enabled() {
    if (!Settings::values.mhgu_hub_enabled.GetValue()) {
        return false;
    }
    return !MmjHub::InRoom(); // in a room, the room carries the traffic
}

bool Attach(std::function<void(const WifiPacket&)> deliver, std::function<void()> lost,
            std::function<MacAddress()> own_mac) {
    return Get().Attach(std::move(deliver), std::move(lost), std::move(own_mac));
}

void Detach() {
    Get().Detach();
}

void Send(const WifiPacket& packet) {
    Get().Send(packet);
}

void Scan(u32 wlan_comm_id, u8 id) {
    Get().Scan(wlan_comm_id, id);
}

void HostUpdate(const NetworkInfo& info, const std::vector<NodeInfo>& nodes) {
    Get().HostUpdate(info, nodes);
}

void HostStop() {
    Get().HostStopAsync();
}

void ClientLeave() {
    Get().ClientLeave();
}

} // namespace Service::NWM::MhguBridge
