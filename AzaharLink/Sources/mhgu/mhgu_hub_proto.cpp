// Copyright Azahar Emulator Project / AzaharPlus
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include "core/hle/service/nwm/mhgu_hub_proto.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace MhguHub {

// ---- logging
namespace {
std::mutex g_log_mutex;
std::function<void(const std::string&)> g_logger;
} // namespace

void SetLogger(std::function<void(const std::string&)> logger) {
    std::scoped_lock lock{g_log_mutex};
    g_logger = std::move(logger);
}

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::scoped_lock lock{g_log_mutex};
    if (g_logger) {
        g_logger(buf);
    }
}

// ---- little endian access
u16 Rd16(const u8* p) {
    return static_cast<u16>(p[0] | p[1] << 8);
}
u32 Rd32(const u8* p) {
    return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 |
           static_cast<u32>(p[3]) << 24;
}
u64 Rd64(const u8* p) {
    return static_cast<u64>(Rd32(p)) | static_cast<u64>(Rd32(p + 4)) << 32;
}
void Wr16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v), p[1] = static_cast<u8>(v >> 8);
}
void Wr32(u8* p, u32 v) {
    for (int i = 0; i < 4; i++) {
        p[i] = static_cast<u8>(v >> (8 * i));
    }
}
void Wr64(u8* p, u64 v) {
    Wr32(p, static_cast<u32>(v)), Wr32(p + 4, static_cast<u32>(v >> 32));
}
static u16 Rd16Be(const u8* p) {
    return static_cast<u16>(p[0] << 8 | p[1]);
}
static u32 Rd32Be(const u8* p) {
    return static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 | static_cast<u32>(p[2]) << 8 | p[3];
}
static void Wr16Be(u8* p, u16 v) {
    p[0] = static_cast<u8>(v >> 8), p[1] = static_cast<u8>(v);
}
static void Wr32Be(u8* p, u32 v) {
    p[0] = static_cast<u8>(v >> 24), p[1] = static_cast<u8>(v >> 16), p[2] = static_cast<u8>(v >> 8),
    p[3] = static_cast<u8>(v);
}
static std::array<u8, 4> IpBytes(u32 ip) { // as the IP appears in Pia packets (network order)
    return {static_cast<u8>(ip >> 24), static_cast<u8>(ip >> 16), static_cast<u8>(ip >> 8), static_cast<u8>(ip)};
}

bool InfoSane(NetInfo& info) {
    const u8 max = info[NI::NodeMax], n = info[NI::NodeCount];
    if (info[NI::Ssid] > 32 || max < 1 || max > MaxNodes || n < 1 || n > max || Rd16(&info[NI::AdvSize]) > AdvMax) {
        return false;
    }
    info[NI::Ssid + 0x21] = 0;
    for (int i = 0; i < MaxNodes; i++) {
        u8* nd = NodeAt(info, i);
        if (!nd[ND::Connected]) {
            std::memset(nd, 0, ND::Size);
            continue;
        }
        nd[ND::Connected] = 1;
        nd[ND::Id] = static_cast<u8>(i);
        nd[ND::Name + 0x20] = 0;
    }
    return true;
}

// ---- link messages
std::vector<u8> EncodeHello(const Hello& h) {
    std::vector<u8> b(HelloSize, 0);
    std::memcpy(b.data(), "MHGL", 4);
    Wr16(&b[4], LinkVersion);
    b[6] = h.scan_only ? 1 : 0;
    Wr16(&b[8], h.lcv);
    std::memcpy(&b[10], h.mac.data(), 6);
    std::memcpy(&b[16], h.name.data(), 0x20); // (the last byte stays 0)
    return b;
}

bool DecodeHello(std::span<const u8> in, Hello& h) {
    if (in.size() < HelloSize || std::memcmp(in.data(), "MHGL", 4) || Rd16(&in[4]) != LinkVersion) {
        return false;
    }
    h.scan_only = in[6] != 0;
    h.lcv = Rd16(&in[8]);
    std::memcpy(h.mac.data(), &in[10], 6);
    std::memcpy(h.name.data(), &in[16], 0x21);
    h.name[0x20] = 0;
    return true;
}

void EncodeVPacket(u8* out, const VPacket& p) {
    Wr32(out, p.src_ip);
    Wr32(out + 4, p.dst_ip);
    Wr16(out + 8, p.src_port);
    Wr16(out + 10, p.dst_port);
    out[12] = p.proto, out[13] = p.kind, out[14] = out[15] = 0;
}

VPacket DecodeVPacket(const u8* in) {
    VPacket p;
    p.src_ip = Rd32(in), p.dst_ip = Rd32(in + 4);
    p.src_port = Rd16(in + 8), p.dst_port = Rd16(in + 10);
    p.proto = in[12], p.kind = in[13];
    return p;
}

std::vector<u8> EncodeRelayRegister(u32 relay_pair, u64 relay_secret) {
    std::vector<u8> b(RelayPrefixSize);
    std::memcpy(b.data(), "MHGR", 4);
    Wr32(&b[4], relay_pair);
    Wr64(&b[8], relay_secret);
    return b;
}

std::vector<u8> EncodeDatagram(u32 relay_pair, u64 relay_secret, u32 token, u8 kind, const VPacket* p,
                               std::span<const u8> data) {
    std::vector<u8> b;
    b.reserve(RelayPrefixSize + UHdrSize + VPacketSize + data.size());
    if (relay_pair) {
        b = EncodeRelayRegister(relay_pair, relay_secret);
    }
    const std::size_t h = b.size();
    b.resize(h + UHdrSize, 0);
    std::memcpy(&b[h], "MHGD", 4);
    Wr32(&b[h + 4], token);
    b[h + 8] = kind;
    if (p) {
        const std::size_t v = b.size();
        b.resize(v + VPacketSize);
        EncodeVPacket(&b[v], *p);
        b.insert(b.end(), data.begin(), data.end());
    }
    return b;
}

bool DecodeDatagram(std::span<const u8> in, u32& token, u8& kind, VPacket& p, std::span<const u8>& data) {
    if (in.size() < UHdrSize || std::memcmp(in.data(), "MHGD", 4)) {
        return false;
    }
    token = Rd32(&in[4]);
    kind = in[8];
    data = {};
    if (kind == U_DATA) {
        if (in.size() < UHdrSize + VPacketSize) {
            return false;
        }
        p = DecodeVPacket(&in[UHdrSize]);
        data = in.subspan(UHdrSize + VPacketSize);
    }
    return true;
}

// ---- sockets
#ifdef _WIN32
using RawSock = SOCKET;
static RawSock Raw(Sock s) {
    return static_cast<RawSock>(s);
}
#define MHGU_SEND_FLAGS 0
#else
using RawSock = int;
static RawSock Raw(Sock s) {
    return static_cast<RawSock>(s);
}
#ifdef MSG_NOSIGNAL
#define MHGU_SEND_FLAGS MSG_NOSIGNAL
#else
#define MHGU_SEND_FLAGS 0
#endif
#endif

static Sock Wrap(RawSock s) {
#ifdef _WIN32
    return s == INVALID_SOCKET ? BadSock : static_cast<Sock>(s);
#else
    return s < 0 ? BadSock : static_cast<Sock>(s);
#endif
}

static sockaddr_in ToSockaddr(const Endpoint& e) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = e.addr;
    a.sin_port = htons(e.port);
    return a;
}

static Endpoint FromSockaddr(const sockaddr_in& a) {
    return Endpoint{a.sin_addr.s_addr, ntohs(a.sin_port)};
}

std::string EndpointString(const Endpoint& e) {
    const u8* b = reinterpret_cast<const u8*>(&e.addr);
    char s[32];
    std::snprintf(s, sizeof s, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], e.port);
    return s;
}

void NetInit() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
    });
#endif
}

static void SetNonblock(Sock s, bool on) {
#ifdef _WIN32
    u_long v = on ? 1 : 0;
    ioctlsocket(Raw(s), FIONBIO, &v);
#else
    const int f = fcntl(Raw(s), F_GETFL);
    fcntl(Raw(s), F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK);
#endif
}

void SetRecvTimeout(Sock s, int ms) {
#ifdef _WIN32
    DWORD v = static_cast<DWORD>(ms);
    setsockopt(Raw(s), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof v);
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(Raw(s), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

void CloseSock(Sock s) {
    if (s == BadSock) {
        return;
    }
#ifdef _WIN32
    closesocket(Raw(s));
#else
    close(Raw(s));
#endif
}

void ShutdownSock(Sock s) {
    if (s != BadSock) {
#ifdef _WIN32
        shutdown(Raw(s), SD_BOTH);
#else
        shutdown(Raw(s), SHUT_RDWR);
#endif
    }
}

std::optional<Endpoint> Resolve(const std::string& host_port, u16 default_port) {
    NetInit();
    std::string host = host_port;
    int port = default_port;
    const auto colon = host.rfind(':');
    if (colon != std::string::npos) {
        port = std::atoi(host.c_str() + colon + 1);
        host.resize(colon);
    }
    if (host.empty() || port < 1 || port > 65535) {
        return std::nullopt;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) || !res) {
        return std::nullopt;
    }
    Endpoint e;
    e.addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
    e.port = static_cast<u16>(port);
    freeaddrinfo(res);
    return e;
}

Sock TcpConnect(const Endpoint& to, int timeout_ms) {
    NetInit();
    const Sock s = Wrap(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (s == BadSock) {
        return BadSock;
    }
    SetNonblock(s, true);
    const sockaddr_in a = ToSockaddr(to);
    connect(Raw(s), reinterpret_cast<const sockaddr*>(&a), sizeof a);
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(Raw(s), &w);
    FD_SET(Raw(s), &e);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int err = 1;
    socklen_t el = sizeof err;
    if (select(static_cast<int>(Raw(s)) + 1, nullptr, &w, &e, &tv) <= 0 || !FD_ISSET(Raw(s), &w) ||
        getsockopt(Raw(s), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el) || err) {
        CloseSock(s);
        return BadSock;
    }
    SetNonblock(s, false);
    int one = 1;
    setsockopt(Raw(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(Raw(s), SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&one), sizeof one);
#endif
    return s;
}

bool SendAll(Sock s, const void* data, std::size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size) {
        const int k = send(Raw(s), p, static_cast<int>(size), MHGU_SEND_FLAGS);
        if (k <= 0) {
            return false;
        }
        p += k, size -= static_cast<std::size_t>(k);
    }
    return true;
}

bool RecvAll(Sock s, void* data, std::size_t size) {
    char* p = static_cast<char*>(data);
    while (size) {
        const int k = recv(Raw(s), p, static_cast<int>(size), 0);
        if (k <= 0) {
            return false;
        }
        p += k, size -= static_cast<std::size_t>(k);
    }
    return true;
}

int WaitReadable(Sock s, int timeout_ms) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(Raw(s), &r);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int k = select(static_cast<int>(Raw(s)) + 1, &r, nullptr, nullptr, &tv);
    return k < 0 ? -1 : k > 0 ? 1 : 0;
}

bool FrameSend(Sock s, u8 type, std::span<const u8> a, std::span<const u8> b) {
    std::vector<u8> f(5 + a.size() + b.size());
    Wr32(f.data(), static_cast<u32>(1 + a.size() + b.size()));
    f[4] = type;
    if (!a.empty()) {
        std::memcpy(&f[5], a.data(), a.size());
    }
    if (!b.empty()) {
        std::memcpy(&f[5 + a.size()], b.data(), b.size());
    }
    return SendAll(s, f.data(), f.size());
}

int FrameRecv(Sock s, u8& type, std::vector<u8>& buf) {
    u8 h[4];
    if (!RecvAll(s, h, 4)) {
        return -1;
    }
    const u32 size = Rd32(h);
    if (size < 1 || size > MaxFrame + 1 || !RecvAll(s, &type, 1)) {
        return -1;
    }
    buf.resize(size - 1);
    if (size > 1 && !RecvAll(s, buf.data(), size - 1)) {
        return -1;
    }
    return static_cast<int>(size - 1);
}

Sock UdpOpen() {
    NetInit();
    const Sock s = Wrap(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (s == BadSock) {
        return BadSock;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(Raw(s), reinterpret_cast<const sockaddr*>(&a), sizeof a)) {
        CloseSock(s);
        return BadSock;
    }
    return s;
}

Sock UdpOpenConnected(const Endpoint& to) {
    const Sock s = UdpOpen();
    if (s == BadSock) {
        return BadSock;
    }
    const sockaddr_in a = ToSockaddr(to);
    if (connect(Raw(s), reinterpret_cast<const sockaddr*>(&a), sizeof a)) {
        CloseSock(s);
        return BadSock;
    }
    return s;
}

bool UdpSendTo(Sock s, const Endpoint& to, std::span<const u8> data) {
    const sockaddr_in a = ToSockaddr(to);
    return sendto(Raw(s), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                  reinterpret_cast<const sockaddr*>(&a), sizeof a) == static_cast<int>(data.size());
}

bool UdpSend(Sock s, std::span<const u8> data) {
    return send(Raw(s), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()),
                MHGU_SEND_FLAGS) == static_cast<int>(data.size());
}

int UdpRecv(Sock s, std::vector<u8>& buf, Endpoint* from, int timeout_ms) {
    const int w = WaitReadable(s, timeout_ms);
    if (w <= 0) {
        return w;
    }
    buf.resize(RelayPrefixSize + UHdrSize + VPacketSize + UdpMaxPayload + 64);
    sockaddr_in a{};
    socklen_t al = sizeof a;
    const int n = recvfrom(Raw(s), reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                           reinterpret_cast<sockaddr*>(&a), &al);
    if (n < 0) {
        return 0; // (an ICMP error on a connected socket: keep going)
    }
    buf.resize(static_cast<std::size_t>(n));
    if (from) {
        *from = FromSockaddr(a);
    }
    return n > 0 ? n : 0;
}

// ---- hub servers
std::vector<std::string> ParseServerList(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        std::string s = cur;
        cur.clear();
        if (s.empty()) {
            return;
        }
        for (const char* scheme : {"http://", "https://"}) {
            if (s.rfind(scheme, 0) == 0) {
                s = s.substr(std::strlen(scheme));
            }
        }
        while (!s.empty() && s.back() == '/') {
            s.pop_back();
        }
        const auto slash = s.find('/');
        if (slash != std::string::npos) {
            s.resize(slash);
        }
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        if (s.empty() || s[0] == '-' || lower == "off") {
            return;
        }
        if (out.size() < 4 && std::find(out.begin(), out.end(), s) == out.end()) {
            out.push_back(s);
        }
    };
    for (const char c : text) {
        if (c == ' ' || c == ',' || c == ';' || c == '\t' || c == '\n' || c == '\r') {
            flush();
        } else {
            cur += c;
        }
    }
    flush();
    return out;
}

std::array<u8, LobbyHeaderSize> LobbyHeader(const std::string& key) {
    std::array<u8, LobbyHeaderSize> h{};
    std::memcpy(h.data(), "MHGS", 4);
    Wr16(&h[4], LobbyProto);
    Wr16(&h[6], LinkVersion);
    std::size_t b = 0, e = key.size();
    while (b < e && key[b] == ' ') {
        b++;
    }
    while (e > b && key[e - 1] == ' ') {
        e--;
    }
    if (b == e) {
        return h; // public
    }
    static const char salt[] = "MHGURecomp matchmaking key:";
    std::vector<u8> in(salt, salt + sizeof salt - 1);
    in.insert(in.end(), key.begin() + static_cast<std::ptrdiff_t>(b), key.begin() + static_cast<std::ptrdiff_t>(e));
    const auto d = Sha256(in);
    std::memcpy(&h[8], d.data(), 16);
    return h;
}

const char* LobbyErrorText(u32 code) {
    switch (code) {
    case SE_NOT_FOUND:
        return "the hub is gone";
    case SE_NO_ANSWER:
        return "the host did not answer";
    case SE_BUSY:
        return "the server is busy";
    case SE_VERSION:
        return "the server speaks another protocol version";
    case SE_NONE:
        return "no answer";
    default:
        return "refused";
    }
}

ServerState ListHubs(const std::string& server, const std::string& key, int connect_ms,
                     std::vector<ListedHub>& out) {
    const auto ep = Resolve(server, LobbyPort);
    if (!ep) {
        return ServerState::NotFound;
    }
    const Sock s = TcpConnect(*ep, connect_ms);
    if (s == BadSock) {
        return ServerState::Unreachable;
    }
    const auto header = LobbyHeader(key);
    SetRecvTimeout(s, 2500);
    u8 type = 0;
    std::vector<u8> buf;
    const int n = FrameSend(s, S_LIST, header) ? FrameRecv(s, type, buf) : -1;
    CloseSock(s);
    if (n >= 4 && type == S_ERROR) {
        return ServerState::Refused;
    }
    if (n < 4 || type != S_HUBS) {
        return ServerState::NoAnswer;
    }
    const u32 count = Rd32(buf.data());
    for (u32 i = 0; i < count && 4 + static_cast<std::size_t>(i + 1) * (8 + NetInfoSize) <= static_cast<std::size_t>(n);
         i++) {
        const u8* e = buf.data() + 4 + static_cast<std::size_t>(i) * (8 + NetInfoSize);
        ListedHub h;
        h.id = Rd64(e);
        std::memcpy(h.info.data(), e + 8, NetInfoSize);
        h.server = *ep;
        h.server_name = server;
        out.push_back(h);
    }
    return ServerState::Online;
}

u32 JoinHub(const Endpoint& server, const std::string& key, u64 hub, Spliced& out) {
    const Sock s = TcpConnect(server, 3000);
    if (s == BadSock) {
        return SE_NONE;
    }
    std::vector<u8> req(LobbyHeaderSize + 8);
    const auto header = LobbyHeader(key);
    std::memcpy(req.data(), header.data(), header.size());
    Wr64(&req[LobbyHeaderSize], hub);
    SetRecvTimeout(s, 12000); // (the server waits up to 10 s for the host)
    u8 type = 0;
    std::vector<u8> buf;
    const int n = FrameSend(s, S_JOIN, req) ? FrameRecv(s, type, buf) : -1;
    if (n >= 12 && type == S_SPLICED) {
        SetRecvTimeout(s, 0);
        out.sock = s, out.pair = Rd32(buf.data()), out.secret = Rd64(buf.data() + 4);
        return 0;
    }
    CloseSock(s);
    return n >= 4 && type == S_ERROR ? Rd32(buf.data()) : SE_NONE;
}

bool AttachTicket(const Endpoint& server, const std::string& key, u64 ticket, Spliced& out) {
    const Sock s = TcpConnect(server, 3000);
    if (s == BadSock) {
        return false;
    }
    std::vector<u8> req(LobbyHeaderSize + 8);
    const auto header = LobbyHeader(key);
    std::memcpy(req.data(), header.data(), header.size());
    Wr64(&req[LobbyHeaderSize], ticket);
    SetRecvTimeout(s, 5000);
    u8 type = 0;
    std::vector<u8> buf;
    const int n = FrameSend(s, S_ATTACH, req) ? FrameRecv(s, type, buf) : -1;
    if (n >= 12 && type == S_SPLICED) {
        out.sock = s, out.pair = Rd32(buf.data()), out.secret = Rd64(buf.data() + 4);
        return true;
    }
    CloseSock(s);
    return false;
}

// ---- hosted hub listing
HubListing::~HubListing() {
    Stop();
}

void HubListing::Start(const std::vector<std::string>& servers, const std::string& key_, InfoFn info,
                       PlayerFn on_player) {
    Stop();
    key = key_;
    info_fn = std::move(info);
    player_fn = std::move(on_player);
    running = true;
    for (const auto& s : servers) {
        threads.emplace_back([this, s] { Run(s); });
    }
}

void HubListing::Stop() {
    if (!running.exchange(false) && threads.empty()) {
        return;
    }
    {
        std::scoped_lock lock{socks_mutex};
        for (const Sock s : socks) {
            ShutdownSock(s);
        }
    }
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    threads.clear();
}

void HubListing::MarkChanged() {
    changes++;
}

void HubListing::Run(std::string server) {
    using Clock = std::chrono::steady_clock;
    auto wait = [this](int ms) {
        for (int t = 0; t < ms && running; t += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return running.load();
    };
    while (running) {
        const auto ep = Resolve(server, LobbyPort);
        const Sock s = ep ? TcpConnect(*ep, 3000) : BadSock;
        if (s == BadSock) {
            Log("[hub] server %s %s: the room is not listed there (trying again in 30 s)", server.c_str(),
                ep ? "unreachable" : "not found");
            if (!wait(30000)) {
                break;
            }
            continue;
        }
        {
            std::scoped_lock lock{socks_mutex};
            socks.push_back(s);
        }
        auto forget = [this, s] {
            std::scoped_lock lock{socks_mutex};
            socks.erase(std::remove(socks.begin(), socks.end(), s), socks.end());
            CloseSock(s);
        };
        const auto header = LobbyHeader(key);
        u32 seen = changes.load();
        NetInfo info = info_fn();
        std::vector<u8> req(header.begin(), header.end());
        req.insert(req.end(), info.begin(), info.end());
        SetRecvTimeout(s, 5000);
        u8 type = 0;
        std::vector<u8> buf;
        int n = FrameSend(s, S_HOST, req) ? FrameRecv(s, type, buf) : -1;
        if (n < 8 || type != S_HOSTED) {
            const bool error = n >= 4 && type == S_ERROR;
            Log("[hub] server %s: can't list the room (%s); trying again in 30 s", server.c_str(),
                error ? LobbyErrorText(Rd32(buf.data())) : "no answer");
            forget();
            if (!wait(30000)) {
                break;
            }
            continue;
        }
        Log("[hub] room listed on %s", server.c_str());
        SetRecvTimeout(s, 0);
        auto last_rx = Clock::now(), next_ping = last_rx + std::chrono::seconds(15);
        while (running) {
            const auto now = Clock::now();
            if (changes.load() != seen) {
                seen = changes.load();
                info = info_fn();
                if (!FrameSend(s, S_UPDATE, info)) {
                    break;
                }
            }
            if (now >= next_ping) {
                if (!FrameSend(s, S_PING, {})) {
                    break;
                }
                next_ping = now + std::chrono::seconds(15);
            }
            if (now - last_rx > std::chrono::seconds(45)) {
                break;
            }
            const int r = WaitReadable(s, 200);
            if (r < 0) {
                break;
            }
            if (r == 0) {
                continue;
            }
            if ((n = FrameRecv(s, type, buf)) < 0) {
                break;
            }
            last_rx = Clock::now();
            if (type == S_INCOMING && n >= 8) {
                const u64 ticket = Rd64(buf.data());
                const Endpoint server_ep = *ep;
                const std::string k = key;
                PlayerFn fn = player_fn;
                std::thread([server_ep, k, ticket, fn] {
                    Spliced sp;
                    if (AttachTicket(server_ep, k, ticket, sp)) {
                        fn(sp, server_ep);
                    } else {
                        Log("[hub] a player's connection through the hub server failed");
                    }
                }).detach();
            }
        }
        forget();
        if (!running) {
            break;
        }
        Log("[hub] lost the hub server %s (trying again in 10 s)", server.c_str());
        if (!wait(10000)) {
            break;
        }
    }
}

// ---- text
std::u16string Utf8ToUtf16(const std::string& s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        u32 cp = 0;
        int extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F, extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F, extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07, extra = 3;
        } else {
            i++;
            continue;
        }
        i++;
        for (int k = 0; k < extra && i < s.size(); k++, i++) {
            cp = cp << 6 | (static_cast<u8>(s[i]) & 0x3F);
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (cp >> 10));
            out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<char16_t>(cp);
        }
    }
    return out;
}

std::string Utf16ToUtf8(const std::u16string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); i++) {
        u32 cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size()) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | cp >> 6);
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | cp >> 12);
            out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | cp >> 18);
            out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
            out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// ---- room description
// Both are Pia's local-play application data: a header (network id, password CRC, system communication version)
// and the game's data. On the 3DS the header carries a session value at 12; in MHGU the game's data starts with
// the constant 0x1234567890ABCDEF and has 8 more bytes after the room's text, so everything after it sits 8 bytes
// later (seen in both games' rooms: text at 52, host name at 104 / 112, end marker at 184 / 192).
static constexpr u8 MhguMagic[8] = {0xEF, 0xCD, 0xAB, 0x90, 0x78, 0x56, 0x34, 0x12};

std::vector<u8> UdsAppDataToAdv(std::span<const u8> app) {
    if (app.size() != AppDataSize) {
        return {app.begin(), app.end()};
    }
    std::vector<u8> out(AppDataSize, 0);
    std::memcpy(&out[0], &app[0], 12);
    const bool zero = std::all_of(app.begin() + 20, app.begin() + 28, [](u8 b) { return b == 0; });
    std::memcpy(&out[20], zero ? MhguMagic : &app[20], 8);
    std::memcpy(&out[28], &app[28], 76 - 28);
    std::memcpy(&out[84], &app[76], AppDataSize - 84);
    return out;
}

std::vector<u8> AdvToUdsAppData(std::span<const u8> adv, u32 session_param) {
    if (adv.size() != AppDataSize) {
        return {adv.begin(), adv.end()};
    }
    std::vector<u8> out(AppDataSize, 0);
    std::memcpy(&out[0], &adv[0], 12);
    Wr32(&out[12], session_param ? session_param : 1);
    if (std::memcmp(&adv[20], MhguMagic, 8)) {
        std::memcpy(&out[20], &adv[20], 8);
    }
    std::memcpy(&out[28], &adv[28], 76 - 28);
    std::memcpy(&out[76], &adv[84], AppDataSize - 84);
    return out;
}

// ---- Pia
bool ParsePia(std::span<const u8> in, PiaPacket& out) {
    static constexpr u8 magic[4] = {0x32, 0xAB, 0x98, 0x64};
    if (in.size() < 12 || std::memcmp(in.data(), magic, 4) || in[4] != 1) { // 1: not encrypted (old header)
        return false;
    }
    out.messages.clear();
    std::memcpy(out.header.data(), in.data(), 12);
    std::size_t o = 12;
    while (o < in.size()) {
        if (o + 20 > in.size()) {
            return false;
        }
        PiaMessage m;
        m.flags = in[o], m.src = in[o + 1];
        const u16 size = Rd16Be(&in[o + 2]);
        m.dest = Rd32Be(&in[o + 4]);
        std::memcpy(m.cid.data(), &in[o + 8], 4);
        m.proto_type = Rd16Be(&in[o + 12]);
        m.proto_port = Rd16Be(&in[o + 14]);
        std::memcpy(m.reserved.data(), &in[o + 16], 4);
        if (o + 20 + size > in.size()) {
            return false;
        }
        m.payload.assign(in.begin() + static_cast<std::ptrdiff_t>(o + 20),
                         in.begin() + static_cast<std::ptrdiff_t>(o + 20 + size));
        out.messages.push_back(std::move(m));
        o += 20 + ((size + 3u) & ~3u);
    }
    return o == in.size();
}

std::vector<u8> BuildPia(const PiaPacket& p) {
    std::vector<u8> out(p.header.begin(), p.header.end());
    for (const auto& m : p.messages) {
        const std::size_t o = out.size();
        out.resize(o + 20 + ((m.payload.size() + 3) & ~std::size_t{3}), 0);
        out[o] = m.flags, out[o + 1] = m.src;
        Wr16Be(&out[o + 2], static_cast<u16>(m.payload.size()));
        Wr32Be(&out[o + 4], m.dest);
        std::memcpy(&out[o + 8], m.cid.data(), 4);
        Wr16Be(&out[o + 12], m.proto_type);
        Wr16Be(&out[o + 14], m.proto_port);
        std::memcpy(&out[o + 16], m.reserved.data(), 4);
        if (!m.payload.empty()) {
            std::memcpy(&out[o + 20], m.payload.data(), m.payload.size());
        }
    }
    return out;
}

namespace {
constexpr u16 ProtoStation = 0x0100, ProtoMesh = 0x0200, ProtoSession = 0x0300, ProtoGame = 0x2000;
constexpr std::size_t PlayerInfoSize = 187; // name[80] enc account[40] enc language key[64]
constexpr std::size_t ConnReq3ds = 78, ConnReqMhgu = 86;
constexpr std::size_t ConnRespHead = 44;
constexpr std::size_t SessionHead = 32, SessionEntry = 9;
constexpr int SessionSlots3ds = 12, SessionSlotsMhgu = 8;

void ReplaceAll(u8* p, std::size_t n, const std::array<u8, 4>& from, const std::array<u8, 4>& to) {
    for (std::size_t i = 0; i + 4 <= n; i++) {
        if (!std::memcmp(p + i, from.data(), 4)) {
            std::memcpy(p + i, to.data(), 4);
            i += 3;
        }
    }
}

// a player name field (bytes, max chars) between UTF-8 (encoding 1) and UTF-16 (encoding 2; big endian, as all of Pia)
void NameToUtf16(u8* field, std::size_t size, u8* enc) {
    if (*enc != 1) {
        return;
    }
    std::string s(reinterpret_cast<const char*>(field), strnlen(reinterpret_cast<const char*>(field), size));
    std::u16string w = Utf8ToUtf16(s);
    if (w.size() > size / 2 - 1) {
        w.resize(size / 2 - 1);
    }
    std::memset(field, 0, size);
    for (std::size_t i = 0; i < w.size(); i++) {
        Wr16Be(field + 2 * i, w[i]);
    }
    *enc = 2;
}

void NameToUtf8(u8* field, std::size_t size, u8* enc) {
    if (*enc != 2) {
        return;
    }
    std::u16string w;
    for (std::size_t i = 0; i + 1 < size; i += 2) {
        const u16 c = Rd16Be(field + i);
        if (!c) {
            break;
        }
        w += static_cast<char16_t>(c);
    }
    std::string s = Utf16ToUtf8(w);
    if (s.size() > size - 1) {
        s.resize(size - 1);
    }
    std::memset(field, 0, size);
    std::memcpy(field, s.data(), s.size());
    *enc = 1;
}
} // namespace

void PiaTranslator::Reset(bool formats_) {
    formats = formats_;
    has_self = false;
    self_addr = {};
    pids.clear();
    index_ips.clear();
    translated = 0;
}

std::array<u8, 8> PiaTranslator::PidOf(u32 ip) const {
    const auto it = pids.find(ip);
    if (it != pids.end()) {
        return it->second;
    }
    std::array<u8, 8> pid{};
    const auto b = IpBytes(ip);
    std::memcpy(pid.data(), b.data(), 4);
    Wr16Be(&pid[4], PiaPort);
    return pid;
}

void PiaTranslator::Learn(const PiaMessage& m, bool from_3ds) {
    if (from_3ds) {
        static constexpr std::array<u8, 4> zero{};
        std::array<u8, 4> addr = m.cid;
        // before it is in a mesh the 3DS sends 0 there; its connection / join request carries its address
        const auto& p = m.payload;
        if (addr == zero && p.size() >= 8 &&
            ((m.proto_type == ProtoStation && p[0] == 1 && p.size() == ConnReq3ds) ||
             (m.proto_type == ProtoMesh && p[0] == 1 && p[1] == 0xFD))) {
            std::memcpy(addr.data(), &p[4], 4);
        }
        if (!has_self && addr != zero) {
            self_addr = addr;
            has_self = true;
            Log("[hub] the 3DS's station address is %02x%02x%02x%02x; on the hub it is %u.%u.%u.%u", addr[0],
                addr[1], addr[2], addr[3], self_ip >> 24, (self_ip >> 16) & 255, (self_ip >> 8) & 255,
                self_ip & 255);
        }
        return;
    }
    const auto& p = m.payload;
    // MHGU side, session info from the host: its variable id (little endian at 20). A station's constant id (PID)
    // is its address, port and the low 16 bits of that id: what a connection request to the host must name.
    if (m.proto_type == ProtoSession && m.proto_port == PiaPort && p.size() >= 24 && p[0] == 1 && p[1] == 0x11 &&
        m.cid[0] == 0xA9 && m.cid[1] == 0xFE) {
        const u32 ip = Rd32Be(m.cid.data());
        const u32 var_id = Rd32(&p[20]);
        std::array<u8, 8> pid;
        std::memcpy(pid.data(), m.cid.data(), 4);
        Wr16Be(&pid[4], PiaPort);
        Wr16Be(&pid[6], static_cast<u16>(var_id));
        pids[ip] = pid;
        for (std::size_t o = SessionHead; o + SessionEntry <= p.size(); o += SessionEntry) {
            if (p[o + 8] != 0xFF && p[o] == 0xA9 && p[o + 1] == 0xFE) {
                index_ips[p[o + 8]] = Rd32Be(&p[o]);
            }
        }
        return;
    }
    // station locations (address, port, PID = address port + 2 bytes) of the hub's stations
    if (m.proto_type != ProtoStation && m.proto_type != ProtoMesh) {
        return;
    }
    for (std::size_t i = 0; i + 14 <= p.size(); i++) {
        if (p[i] != 0xA9 || p[i + 1] != 0xFE || std::memcmp(&p[i], &p[i + 6], 6)) {
            continue;
        }
        const u32 ip = Rd32Be(&p[i]);
        std::array<u8, 8> pid;
        std::memcpy(pid.data(), &p[i + 6], 8);
        pids[ip] = pid;
        i += 13;
    }
}

void PiaTranslator::SwapAddr(PiaMessage& m, const std::array<u8, 4>& from, const std::array<u8, 4>& to) {
    if (m.cid == from) {
        m.cid = to;
    }
    if (m.proto_type != ProtoGame && !m.payload.empty()) { // game data (0x2000) is passed on untouched
        ReplaceAll(m.payload.data(), m.payload.size(), from, to);
    }
}

std::vector<u8> PiaTranslator::ToMhgu(std::span<const u8> in, int dest_index) {
    PiaPacket pk;
    if (!ParsePia(in, pk)) {
        return {in.begin(), in.end()};
    }
    const auto ip_self = IpBytes(self_ip);
    for (auto& m : pk.messages) {
        Learn(m, true);
        if (has_self && self_ip) {
            SwapAddr(m, self_addr, ip_self);
        }
        if (!formats) {
            continue;
        }
        auto& p = m.payload;
        if (m.proto_type == ProtoStation && m.proto_port == 0 && !p.empty() && p[0] == 1 && p.size() == ConnReq3ds) {
            // connection request: MHGU's has the target's constant id (PID) after the first 4 bytes. The target
            // is the UDS node the packet goes to (node n is 169.254.x.n); Pia's station index is not tied to it.
            u32 target_ip = dest_index >= 0 ? subnet | static_cast<u32>(dest_index + 1) : 0;
            for (int i = 0; !target_ip && i < 32; i++) {
                if (m.dest == (1u << i) && index_ips.count(i)) {
                    target_ip = index_ips.at(i);
                }
            }
            const auto pid = PidOf(target_ip ? target_ip : subnet | 1);
            p.insert(p.begin() + 4, pid.begin(), pid.end());
            translated++;
        } else if (m.proto_type == ProtoStation && m.proto_port == 0 && p.size() >= ConnRespHead + 4 &&
                   p[0] == 2 && p[1] == 0 && (p.size() - ConnRespHead - 4) % PlayerInfoSize == 0) {
            // connection response: MHGU's platform id, token magic, always 4 player infos, UTF-8 names
            const std::size_t infos = (p.size() - ConnRespHead - 4) / PlayerInfoSize;
            if (infos >= 4) {
                continue;
            }
            p[3] = 4;
            if (std::all_of(p.begin() + 5, p.begin() + 13, [](u8 b) { return b == 0; })) {
                std::memcpy(&p[5], MhguMagic, 8);
            }
            const std::array<u8, 4> ack{p[p.size() - 4], p[p.size() - 3], p[p.size() - 2], p[p.size() - 1]};
            p.resize(p.size() - 4);
            for (std::size_t i = 0; i < infos; i++) {
                u8* pi = &p[ConnRespHead + i * PlayerInfoSize];
                NameToUtf8(pi, 80, pi + 0x50);
                NameToUtf8(pi + 0x51, 40, pi + 0x79);
            }
            p.resize(ConnRespHead + 4 * PlayerInfoSize, 0);
            p.insert(p.end(), ack.begin(), ack.end());
            translated++;
        } else if (m.proto_type == ProtoSession && m.proto_port == PiaPort && p.size() ==
                       SessionHead + SessionSlots3ds * SessionEntry + 1 && p[0] == 1 && p[1] == 0x11) {
            // session info: 12 station slots (with the UDS node id) -> 8 (node id 0)
            std::vector<u8> q(p.begin(), p.begin() + SessionHead);
            for (int i = 0; i < SessionSlotsMhgu; i++) {
                const u8* e = &p[SessionHead + i * SessionEntry];
                q.insert(q.end(), e, e + SessionEntry);
                q[q.size() - 3] = q[q.size() - 2] = 0;
            }
            q.push_back(p.back());
            q[2] = static_cast<u8>(SessionSlotsMhgu * SessionEntry + 1);
            p = std::move(q);
            translated++;
        }
    }
    return BuildPia(pk);
}

std::vector<u8> PiaTranslator::To3ds(std::span<const u8> in) {
    PiaPacket pk;
    if (!ParsePia(in, pk)) {
        return {in.begin(), in.end()};
    }
    const auto ip_self = IpBytes(self_ip);
    for (auto& m : pk.messages) {
        Learn(m, false);
        if (has_self && self_ip) {
            SwapAddr(m, ip_self, self_addr);
        }
        if (!formats) {
            continue;
        }
        auto& p = m.payload;
        if (m.proto_type == ProtoStation && m.proto_port == 0 && !p.empty() && p[0] == 1 &&
            p.size() == ConnReqMhgu) {
            p.erase(p.begin() + 4, p.begin() + 12);
            translated++;
        } else if (m.proto_type == ProtoStation && m.proto_port == 0 && p.size() == ConnRespHead + 4 * PlayerInfoSize + 4 &&
                   p[0] == 2 && p[1] == 0) {
            const std::size_t infos = std::clamp<std::size_t>(p[43], 1, 4);
            p[3] = 2;
            std::fill(p.begin() + 5, p.begin() + 37, u8{0});
            const std::array<u8, 4> ack{p[p.size() - 4], p[p.size() - 3], p[p.size() - 2], p[p.size() - 1]};
            p.resize(ConnRespHead + infos * PlayerInfoSize);
            for (std::size_t i = 0; i < infos; i++) {
                u8* pi = &p[ConnRespHead + i * PlayerInfoSize];
                NameToUtf16(pi, 80, pi + 0x50);
                NameToUtf16(pi + 0x51, 40, pi + 0x79);
            }
            p.insert(p.end(), ack.begin(), ack.end());
            translated++;
        } else if (m.proto_type == ProtoSession && m.proto_port == PiaPort &&
                   p.size() == SessionHead + SessionSlotsMhgu * SessionEntry + 1 && p[0] == 1 && p[1] == 0x11) {
            // session info: 8 slots -> 12, with the UDS node id the 3DS sends each station's packets to: the
            // last byte of its address on the hub (Pia's station index is the order stations joined, not that)
            std::vector<u8> q(p.begin(), p.begin() + SessionHead);
            for (int i = 0; i < SessionSlots3ds; i++) {
                u8 e[SessionEntry] = {0, 0, 0, 0, 0, 0, 0, 0, 0xFF};
                if (i < SessionSlotsMhgu) {
                    std::memcpy(e, &p[SessionHead + i * SessionEntry], SessionEntry);
                    if (e[8] != 0xFF) {
                        u16 node = 0;
                        if (has_self && !std::memcmp(e, self_addr.data(), 4)) {
                            node = static_cast<u16>(self_ip & 0xFF);
                        } else if (e[0] == 0xA9 && e[1] == 0xFE) {
                            node = e[3];
                        }
                        Wr16Be(&e[6], node);
                    }
                }
                q.insert(q.end(), e, e + SessionEntry);
            }
            q.push_back(p.back());
            q[2] = static_cast<u8>(SessionSlots3ds * SessionEntry + 1);
            p = std::move(q);
            translated++;
        }
    }
    return BuildPia(pk);
}

// ---- CRC-32 (zlib's) and a friend code seed with a chosen CRC-32
static const std::array<u32, 256>& CrcTable() {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) {
                c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

u32 Crc32(std::span<const u8> data) {
    const auto& t = CrcTable();
    u32 r = 0xFFFFFFFFu;
    for (const u8 b : data) {
        r = (r >> 8) ^ t[(r ^ b) & 0xFF];
    }
    return r ^ 0xFFFFFFFFu;
}

u64 SeedForAddress(u32 address) {
    // "MHGU" and 4 bytes that bring the CRC-32 of the 8 to the address (each table entry's top byte is unique, so
    // the 4 entries are found backwards from the result, then the bytes forwards)
    const auto& t = CrcTable();
    u8 seed[8] = {'M', 'H', 'G', 'U', 0, 0, 0, 0};
    u32 state = Crc32({seed, 4}) ^ 0xFFFFFFFFu;
    u32 f = address ^ 0xFFFFFFFFu;
    u8 index[4];
    for (int k = 3; k >= 0; k--) {
        u8 j = 0;
        for (int i = 0; i < 256; i++) {
            if ((t[i] >> 24) == (f >> 24)) {
                j = static_cast<u8>(i);
            }
        }
        index[k] = j;
        f = (f ^ t[j]) << 8;
    }
    for (int k = 0; k < 4; k++) {
        const u8 b = static_cast<u8>((state ^ index[k]) & 0xFF);
        seed[4 + k] = b;
        state = (state >> 8) ^ t[(state ^ b) & 0xFF];
    }
    return Rd64(seed);
}

// ---- SHA-256
std::array<u8, 32> Sha256(std::span<const u8> data) {
    static constexpr u32 k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<u8> m(data.begin(), data.end());
    const u64 bits = static_cast<u64>(m.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) {
        m.push_back(0);
    }
    for (int i = 7; i >= 0; i--) {
        m.push_back(static_cast<u8>(bits >> (8 * i)));
    }
    auto rotr = [](u32 x, int n) { return (x >> n) | (x << (32 - n)); };
    for (std::size_t c = 0; c < m.size(); c += 64) {
        u32 w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = Rd32Be(&m[c + 4 * i]);
        }
        for (int i = 16; i < 64; i++) {
            const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], cc = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const u32 ch = (e & f) ^ (~e & g);
            const u32 t1 = hh + s1 + ch + k[i] + w[i];
            const u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const u32 mj = (a & b) ^ (a & cc) ^ (b & cc);
            const u32 t2 = s0 + mj;
            hh = g, g = f, f = e, e = d + t1, d = cc, cc = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += cc, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    std::array<u8, 32> out;
    for (int i = 0; i < 8; i++) {
        Wr32Be(&out[4 * i], h[i]);
    }
    return out;
}

} // namespace MhguHub
