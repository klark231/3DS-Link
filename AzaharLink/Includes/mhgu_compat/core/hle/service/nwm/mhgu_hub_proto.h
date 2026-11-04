// Copyright Azahar Emulator Project / AzaharPlus
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// MHGU hub crossplay: the protocol side, with no emulator types in it (so it can be tested on its own).
//
// MHGU Recomp (the native Android port of Monster Hunter Generations Ultimate) plays "local wireless" (nn::ldn) over
// IP: one game hosts a hub, the others join it, and hub servers list hubs and splice players to hosts. This file
// speaks that protocol (lobby "MHGS", link "MHGL", datagrams "MHGD" / relay "MHGR"; see gu-native runtime/net/ldn.c),
// and converts what has to be converted between MHXX on the 3DS (UDS local play) and MHGU:
//  - the room description (UDS application data <-> LDN advertise data, both 200 bytes, a few fields apart),
//  - the game's packets (Pia): the 3DS's own station address <-> its IP on the hub, and three Pia system messages
//    whose layout differs between the 3DS's Pia and MHGU's (connection request, connection response, session info).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace MhguHub {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---- logging (set by the emulator side)
void SetLogger(std::function<void(const std::string&)> logger);
void Log(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// ---- nn::ldn NetworkInfo (0x480 bytes, little endian)
inline constexpr std::size_t NetInfoSize = 0x480;
using NetInfo = std::array<u8, NetInfoSize>;
namespace NI {
inline constexpr std::size_t LocalCommId = 0x00, SceneId = 0x0A, SessionId = 0x10, Bssid = 0x20, Ssid = 0x26,
                             Channel = 0x48, LinkLevel = 0x4A, NetworkType = 0x4B, SecParam = 0x50,
                             SecMode = 0x60, AcceptPolicy = 0x62, NodeMax = 0x66, NodeCount = 0x67,
                             Nodes = 0x68, AdvSize = 0x26A, Adv = 0x26C, AuthId = 0x478;
}
namespace ND {
inline constexpr std::size_t Size = 0x40, Ip = 0x00, Mac = 0x04, Id = 0x0A, Connected = 0x0B, Name = 0x0C,
                             Lcv = 0x2E;
}
inline constexpr int MaxNodes = 8;
inline constexpr std::size_t AdvMax = 0x180;
inline constexpr u64 MhguLocalCommId = 0x0100770008DD8000ULL;
inline constexpr u16 MhguLcv = 1;     // local communication version MHGU uses
inline constexpr u16 PiaPort = 12345; // the UDP port the game's packets use

u16 Rd16(const u8* p);
u32 Rd32(const u8* p);
u64 Rd64(const u8* p);
void Wr16(u8* p, u16 v);
void Wr32(u8* p, u32 v);
void Wr64(u8* p, u64 v);

// Range checks and terminations as gu-native's info_sane (false: don't use this info).
bool InfoSane(NetInfo& info);
inline u8* NodeAt(NetInfo& info, int i) {
    return info.data() + NI::Nodes + i * ND::Size;
}
inline const u8* NodeAt(const NetInfo& info, int i) {
    return info.data() + NI::Nodes + i * ND::Size;
}

// ---- link protocol (TCP between a station and the host, possibly spliced through a hub server)
inline constexpr u16 LinkVersion = 2;
enum : u8 { F_HELLO = 1, F_INFO, F_WELCOME, F_REFUSE, F_PACKET, F_DESTROY, F_AUX };
inline constexpr std::size_t MaxFrame = 0x10000 + 64;

// nn::ldn results for F_REFUSE (module 203)
inline constexpr u32 LdnResult(u32 d) {
    return 203 | (d << 9);
}
inline constexpr u32 R_CONNECT_REJECTED = LdnResult(67);
inline constexpr u32 R_VERSION_TOO_LOW = LdnResult(113);
inline constexpr u32 R_VERSION_TOO_HIGH = LdnResult(114);
inline constexpr u32 R_TOO_MANY_PLAYERS = LdnResult(144);
// disconnect reasons for F_DESTROY
inline constexpr u32 DR_DESTROYED_BY_USER = 3, DR_REJECTED = 5, DR_SIGNAL_LOST = 6;

struct Hello {
    bool scan_only = false;
    u16 lcv = MhguLcv;
    std::array<u8, 6> mac{};
    std::array<char, 0x21> name{}; // UTF-8, NUL terminated
};
inline constexpr std::size_t HelloSize = 52;
std::vector<u8> EncodeHello(const Hello& h);
bool DecodeHello(std::span<const u8> in, Hello& h); // false: not a hello of our link version

// A packet of the hub's virtual network (host byte order, little endian on every platform we run on).
enum : u8 { VPROTO_UDP = 0, VPROTO_TCP = 1 };
struct VPacket {
    u32 src_ip = 0, dst_ip = 0;
    u16 src_port = 0, dst_port = 0;
    u8 proto = VPROTO_UDP, kind = 0;
};
inline constexpr std::size_t VPacketSize = 16;
void EncodeVPacket(u8* out, const VPacket& p);
VPacket DecodeVPacket(const u8* in);

// Datagrams: [relay prefix "MHGR" u32 pair u64 secret] "MHGD" u32 token u8 kind pad[3] [VPacket data]
enum : u8 { U_DATA = 0, U_HELLO, U_HELLO_ACK };
inline constexpr std::size_t RelayPrefixSize = 16, UHdrSize = 12, UdpMaxPayload = 8192;
std::vector<u8> EncodeDatagram(u32 relay_pair, u64 relay_secret, u32 token, u8 kind, const VPacket* p,
                               std::span<const u8> data);
std::vector<u8> EncodeRelayRegister(u32 relay_pair, u64 relay_secret);
// in: a datagram as received (the server takes the relay prefix off)
bool DecodeDatagram(std::span<const u8> in, u32& token, u8& kind, VPacket& p, std::span<const u8>& data);

// ---- sockets (small portable layer; a socket is an intptr_t, -1 = none)
using Sock = std::intptr_t;
inline constexpr Sock BadSock = -1;
struct Endpoint {
    u32 addr = 0; // IPv4, network byte order
    u16 port = 0; // host byte order
    bool operator==(const Endpoint& o) const {
        return addr == o.addr && port == o.port;
    }
};
std::string EndpointString(const Endpoint& e);
void NetInit();
std::optional<Endpoint> Resolve(const std::string& host_port, u16 default_port);
Sock TcpConnect(const Endpoint& to, int timeout_ms);
void CloseSock(Sock s);
void ShutdownSock(Sock s); // wakes a thread blocked on it
void SetRecvTimeout(Sock s, int ms); // 0: none
bool SendAll(Sock s, const void* data, std::size_t size);
bool RecvAll(Sock s, void* data, std::size_t size);
int WaitReadable(Sock s, int timeout_ms); // 1 readable, 0 timeout, -1 error
bool FrameSend(Sock s, u8 type, std::span<const u8> a, std::span<const u8> b = {});
int FrameRecv(Sock s, u8& type, std::vector<u8>& buf); // payload size, -1 closed / error
Sock UdpOpen();                                        // bound to a port of the system's choice
Sock UdpOpenConnected(const Endpoint& to);
bool UdpSendTo(Sock s, const Endpoint& to, std::span<const u8> data);
bool UdpSend(Sock s, std::span<const u8> data); // connected socket
int UdpRecv(Sock s, std::vector<u8>& buf, Endpoint* from, int timeout_ms); // size, 0 timeout, -1 error

// ---- hub servers ("MHGS")
inline constexpr u16 LobbyPort = 39510;
inline constexpr u16 LobbyProto = 1;
inline constexpr std::size_t LobbyHeaderSize = 24;
enum : u8 { S_LIST = 1, S_HOST, S_ATTACH, S_JOIN, S_UPDATE, S_PING, S_HUBS = 16, S_HOSTED, S_INCOMING, S_SPLICED,
            S_ERROR };
enum : u32 { SE_NOT_FOUND = 1, SE_NO_ANSWER, SE_BUSY, SE_VERSION, SE_NONE = 0xFFFFFFFFu };
inline constexpr const char* DefaultServers =
    "hunterhubid-gu-hub.duckdns.org morerokk-mhgurecomp-ldnserver.duckdns.org";

// "a b,c": up to 4 names; "http://name/" is "name"; a name starting with '-' is switched off; "Off" = none.
std::vector<std::string> ParseServerList(const std::string& text);
// MHGS, proto, link version, room = first 16 bytes of SHA-256("MHGURecomp matchmaking key:" + key) (0: public)
std::array<u8, LobbyHeaderSize> LobbyHeader(const std::string& key);
const char* LobbyErrorText(u32 code);

struct ListedHub {
    u64 id = 0;
    NetInfo info{};
    Endpoint server;
    std::string server_name;
};
enum class ServerState { Online, NotFound, Unreachable, NoAnswer, Refused };
ServerState ListHubs(const std::string& server, const std::string& key, int connect_ms,
                     std::vector<ListedHub>& out);

struct Spliced {
    Sock sock = BadSock;
    u32 pair = 0;
    u64 secret = 0;
};
// S_JOIN through the server: 0 and out filled, else an SE_* code (SE_NONE: no answer / unreachable)
u32 JoinHub(const Endpoint& server, const std::string& key, u64 hub, Spliced& out);
// S_ATTACH: the host's end of a player's connection the server announced with S_INCOMING
bool AttachTicket(const Endpoint& server, const std::string& key, u64 ticket, Spliced& out);

// A hosted hub's listing on every server of a list: S_HOST, then S_UPDATE when the info changes and S_PING; a
// player the server announces (S_INCOMING) is attached and handed to on_player (in a thread of its own).
class HubListing {
public:
    using InfoFn = std::function<NetInfo()>;
    using PlayerFn = std::function<void(Spliced spliced, Endpoint server)>;
    HubListing() = default;
    ~HubListing();
    void Start(const std::vector<std::string>& servers, const std::string& key, InfoFn info, PlayerFn on_player);
    void Stop();
    void MarkChanged(); // the info changed: send it again
    bool Running() const {
        return running.load();
    }

private:
    void Run(std::string server);
    std::atomic<bool> running{false};
    std::atomic<u32> changes{0};
    std::string key;
    InfoFn info_fn;
    PlayerFn player_fn;
    std::vector<std::thread> threads;
    std::mutex socks_mutex;
    std::vector<Sock> socks; // open server connections (closed by Stop)
};

// ---- text
std::u16string Utf8ToUtf16(const std::string& s);
std::string Utf16ToUtf8(const std::u16string& s);

// ---- room description: 3DS UDS application data <-> MHGU advertise data (both 200 bytes)
inline constexpr std::size_t AppDataSize = 200;
std::vector<u8> AdvToUdsAppData(std::span<const u8> adv, u32 session_param);
std::vector<u8> UdsAppDataToAdv(std::span<const u8> app);

// ---- Pia (the game's network library) packets
struct PiaMessage {
    u8 flags = 0, src = 0;
    u32 dest = 0;
    std::array<u8, 4> cid{}; // source constant id: the sender's station address
    u16 proto_type = 0, proto_port = 0;
    std::array<u8, 4> reserved{};
    std::vector<u8> payload;
};
struct PiaPacket {
    std::array<u8, 12> header{};
    std::vector<PiaMessage> messages;
};
bool ParsePia(std::span<const u8> in, PiaPacket& out);
std::vector<u8> BuildPia(const PiaPacket& p);

class PiaTranslator {
public:
    // formats: convert the three Pia messages whose layout differs; off: only swap the address
    void Reset(bool formats);
    void SetSelfIp(u32 ip) {
        self_ip = ip;
    } // the 3DS side's IP on the hub (host byte order)
    void SetSubnet(u32 subnet) {
        this->subnet = subnet;
    }
    bool HasSelfAddr() const {
        return has_self;
    }
    std::array<u8, 4> SelfAddr() const {
        return self_addr;
    }
    // A packet from the 3DS to MHGU / from MHGU to the 3DS; dest_index: the 3DS-side destination station index
    // (UDS node - 1) of a unicast packet, -1 for broadcast. Not a Pia packet: returned as it is.
    std::vector<u8> ToMhgu(std::span<const u8> in, int dest_index);
    std::vector<u8> To3ds(std::span<const u8> in);
    u64 Translated() const {
        return translated;
    }

private:
    void Learn(const PiaMessage& m, bool from_3ds);
    void SwapAddr(PiaMessage& m, const std::array<u8, 4>& from, const std::array<u8, 4>& to);
    std::array<u8, 8> PidOf(u32 ip) const;
    bool formats = true;
    u32 self_ip = 0, subnet = 0;
    bool has_self = false;
    std::array<u8, 4> self_addr{};
    std::map<u32, std::array<u8, 8>> pids; // station IP -> its 8-byte constant id (PID) as seen in MHGU messages
    std::map<int, u32> index_ips;          // Pia station index -> station IP, from the host's session info
    u64 translated = 0;
};

// ---- the 3DS's station addresses
// On the 3DS, Pia's address of a station is the CRC-32 of its UDS friend code seed (8 bytes, little endian); that is
// how the 3DS finds the UDS node to send a station's packets to. A hub player's seed is made so that its CRC-32 is
// the player's hub IP: then the 3DS finds MHGU players by the addresses MHGU uses.
u32 Crc32(std::span<const u8> data);
u64 SeedForAddress(u32 address);

// ---- SHA-256 (for the matchmaking key)
std::array<u8, 32> Sha256(std::span<const u8> data);

} // namespace MhguHub
