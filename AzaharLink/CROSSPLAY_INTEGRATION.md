# 3DS Link — MHGU Crossplay integration (phase 2)

Status of the on-console crossplay port, honestly:

| Piece | State |
|---|---|
| **Toggle** (ConfigApp + ini + Connect stub) | ✅ done — `crossplay_scaffold.py` |
| **`CrossplayBridge` wrapper** (`Includes/ctr/crossplay.hpp`, `Sources/ctr/crossplay.cpp`) | ✅ **written + compile/link/run-validated on Linux against the real `mhgu_hub` + `mhgu_hub_proto`** |
| Makefile: import kit bridge + flags | ⚠️ steps below — needs devkitARM |
| `plugin.cpp`: route transport in crossplay mode | ⚠️ steps below — needs devkitARM |
| `Scan` client-direction hook | ⚠️ open (same gap relay2 marked LIVE-TODO) |
| 3DS socket layer (`soc`) for the proto | ⚠️ needs validation (libctru DNS) |

The wrapper is the hard part and it's **proven**: `azlx::CrossplayBridge` drives the exact bridge the
emulator uses, with only `azl` types exposed to the plugin. What's left is wiring it into the build
and the connect flow — concrete, but it needs your devkitARM + a console to confirm.

---

## 1. Import the bridge sources

The bridge (`mhgu_hub.cpp`, `mhgu_hub_proto.cpp`, `minicrypto.cpp`, `uds_beacon.cpp`,
`uds_connection.cpp`, `uds_data.cpp`) and its `compat/` headers live in the **MMJ crossplay kit**
(`mmj_mhgu_crossplay/`). Copy them into the AzaharLink tree:

```
Sources/mhgu/            <- the 6 kit src/*.cpp (the bridge)
Includes/mhgu_compat/    <- the kit compat/ headers (common/, core/, network/, cryptopp/, fmt_include/)
```

## 2. Makefile

`SOURCES` already globs `Sources/ctr`, so `crossplay.cpp` is picked up automatically. Add the bridge
dir and the compat include, and handle the two flag mismatches:

- **C++20**: the proto uses `std::span`. AzaharLink builds `-std=gnu++17`. Compile the bridge (and
  `crossplay.cpp`) with `-std=gnu++20`. Easiest: add `Sources/mhgu` to `SOURCES`, then give those
  objects a per-file `-std=gnu++20` override (a pattern rule on `mhgu_hub%.o`), or bump the whole
  project to `gnu++20` (devkitARM GCC 13+ supports it).
- **fmt + exceptions**: the bridge uses header-only fmt, AzaharLink builds `-fno-exceptions`. Add
  `-DFMT_HEADER_ONLY=1 -DFMT_EXCEPTIONS=0` for the bridge objects.
- **Includes**: add `-I Includes/mhgu_compat -I Includes/mhgu_compat/fmt_include -I Sources/mhgu`.

## 3. Socket layer (the one real unknown)

`mhgu_hub_proto.cpp` is POSIX sockets, feature-flagged (the relay built it with
`-DHAS_GETADDRINFO=1 -DHAS_POLL=1` …). libctru's `soc` has `socket/connect/recvfrom/select` but
**`getaddrinfo` is limited** — set `-DHAS_GETADDRINFO=0` and confirm the proto's fallback resolves a
host via `gethostbyname`, or resolve the hub to an IP yourself and pass that. `ctr/soc.cpp` already
owns `socInit()`, so the sockets exist once the game has the `soc:U` right.

## 4. `plugin.cpp` wiring (mirror the room path)

In `Includes/ctr/plugin.hpp`, add a member:
```cpp
#include "ctr/crossplay.hpp"
...
std::unique_ptr<azlx::CrossplayBridge> crossplay_;
```

Where the transport is set up (the `UdsEngine::Transport transport;` block), branch on
`config_.crossplay`:
```cpp
if (config_.crossplay) {
    crossplay_.reset(new azlx::CrossplayBridge());
    transport.send = [this](WifiPacket& p) {
        p.transmitter_address = /* this console's station mac */;
        crossplay_->SendFrame(p);               // game out -> hub
    };
    transport.get_mac = [this]() { return console_mac_; };
} else {
    /* existing room_ transport, unchanged */
}
```
Start the bridge where the room is started (`Connect()`), with the hub servers + key, delivering
into the engine:
```cpp
crossplay_->Start(hub_servers, matchmaking_key,
                  [this](const azl::WifiPacket& p) { engine_->OnWifiPacket(p); }, // hub -> game
                  [this]() { return console_mac_; });
```
(Put the hub servers in `config_.host` while crossplay is on, or default to the proto's
`MhguHub::DefaultServers`.) Stop it wherever the room is torn down (`crossplay_->Stop()`).

## 5. The `Scan` client direction (open)

Hosting (3DS hosts, MHGU joins) works through `SendFrame`: the game's beacons become `HostUpdate`.
The **client** direction (3DS *searches* and sees MHGU hubs) needs `crossplay_->Scan(comm_id, id)`
called when the game scans — i.e. a hook where `UdsEngine` handles `RecvBeaconBroadcastData`, the
way the emulator calls `MhguBridge::Scan` there. That hook doesn't exist in the engine yet; it's the
same gap `relay2.cpp` flagged. Add a scan callback on the engine (or call `Scan` on a timer while in
a title's lobby), then the hubs arrive as beacons via the deliver callback.

---

## What to do next

1. Build the toggle scaffold alone first (already given) — confirm it compiles and the switch shows.
2. Do steps 1–4 and get a clean **build** (crossplay off still behaves exactly as before).
3. Turn crossplay on and test **hosting** first (3DS hosts a room, an MHGU-native player joins) —
   that path is complete.
4. Then do step 5 for the search/client direction.

The wrapper (`crossplay.hpp`/`.cpp`) is done and validated; everything above is wiring + the one
socket/scan unknown that only a console build will settle.

---

## Hub servers (added)

Crossplay now has its own two hub servers, separate from the room address (`host`):

| ini key | default |
|---|---|
| `hub_a` | `hunterhubid-gu-hub.duckdns.org` |
| `hub_b` | `morerokk-mhgurecomp-ldnserver.duckdns.org` |

Same two servers as AzaharPlus's `mhgu_hub_servers`. The plugin passes `hub_a hub_b` to the bridge, which lists
hubs on both servers at once and advertises a hosted room on both. In the ConfigApp: **Settings -> Hub server A / B**
(A = keyboard; empty = that server is off; `*` = back to the default; both empty = the two defaults).
A name may carry a port (`name:port`). `password` is still the matchmaking key.
