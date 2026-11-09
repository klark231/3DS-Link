#!/usr/bin/env python3
"""Phase 2: wire the MHGU bridge into the plugin. Run from the AzaharLink root, AFTER
crossplay_scaffold.py and AFTER copying the bridge in (see CROSSPLAY_INTEGRATION.md step 1):

    Sources/mhgu/          <- the 6 kit src/*.cpp
    Includes/mhgu_compat/  <- the kit compat/ headers

Patches the Makefile (build the bridge, C++20 + header-only fmt) and plugin.cpp (route the
transport through the bridge in crossplay mode, start it in Connect). Idempotent.
"""
import sys

def patch(path, old, new, count=1):
    with open(path, encoding="utf-8") as f:
        s = f.read()
    if new in s:
        print(f"  skip: {path}")
        return
    n = s.count(old)
    if n != count:
        sys.exit(f"ERROR {path}: anchor {n}x (want {count}):\n  {old[:110]!r}")
    with open(path, "w", encoding="utf-8") as f:
        f.write(s.replace(old, new))
    print(f"  patched: {path}")

# ---- Makefile: build the bridge, bump to C++20, header-only fmt without exceptions -----------
patch("Makefile",
      "SOURCES \t:= \tSources Sources/azl Sources/ctr Sources/enet",
      "SOURCES \t:= \tSources Sources/azl Sources/ctr Sources/enet Sources/mhgu")
patch("Makefile",
      "INCLUDES\t:= \tIncludes",
      "INCLUDES\t:= \tIncludes Includes/mhgu_compat Includes/mhgu_compat/fmt_include")
patch("Makefile",
      "CXXFLAGS\t:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17",
      "CXXFLAGS\t:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++20 -DFMT_HEADER_ONLY=1 -DFMT_EXCEPTIONS=0")

# ---- plugin.hpp: the member + its header ----------------------------------------------------
patch("Includes/ctr/plugin.hpp",
      '#include "azl/platform.hpp"\n',
      '#include "azl/platform.hpp"\n#include "ctr/crossplay.hpp"\n')
patch("Includes/ctr/plugin.hpp",
      "    std::unique_ptr<RoomClient> room_;\n",
      "    std::unique_ptr<RoomClient> room_;\n"
      "    std::unique_ptr<azlx::CrossplayBridge> crossplay_; ///< drives the MHGU hub when config_.crossplay\n")

# ---- plugin.cpp: build the bridge object, route the transport -------------------------------
patch("Sources/ctr/plugin.cpp",
      "    room_.reset(new RoomClient());\n"
      "\n"
      "    UdsEngine::Transport transport;",
      "    room_.reset(new RoomClient());\n"
      "    if (config_.crossplay) {\n"
      "        crossplay_.reset(new azlx::CrossplayBridge());\n"
      "    }\n"
      "\n"
      "    UdsEngine::Transport transport;")
patch("Sources/ctr/plugin.cpp",
      "    transport.send = [this](WifiPacket& packet) {\n"
      "        if (!room_->joined()) {\n"
      "            return; // not in a room: frames go nowhere, like an emulator without a connection\n"
      "        }\n"
      "        packet.transmitter_address = room_->mac();\n"
      "        room_->SendWifiPacket(packet);\n"
      "    };\n"
      "    transport.get_mac = [this]() { return room_->joined() ? room_->mac() : console_mac_; };",
      "    transport.send = [this](WifiPacket& packet) {\n"
      "        if (config_.crossplay) {\n"
      "            packet.transmitter_address = console_mac_;\n"
      "            if (crossplay_) {\n"
      "                crossplay_->SendFrame(packet); // beacon -> hub listing, the rest -> the hub\n"
      "            }\n"
      "            return;\n"
      "        }\n"
      "        if (!room_->joined()) {\n"
      "            return; // not in a room: frames go nowhere, like an emulator without a connection\n"
      "        }\n"
      "        packet.transmitter_address = room_->mac();\n"
      "        room_->SendWifiPacket(packet);\n"
      "    };\n"
      "    transport.get_mac = [this]() {\n"
      "        if (config_.crossplay) {\n"
      "            return console_mac_;\n"
      "        }\n"
      "        return room_->joined() ? room_->mac() : console_mac_;\n"
      "    };")

# ---- plugin.cpp: Connect() -- start the bridge instead of joining a room --------------------
# Replaces the scaffold's stub (crossplay_scaffold.py put it there).
patch("Sources/ctr/plugin.cpp",
      "    if (config_.crossplay) {\n"
      "        // TODO(crossplay): drive the MHGU bridge (mhgu_hub + mhgu_hub_proto) to the hub here\n"
      "        // instead of joining a room. For now this only logs and falls through to the room path,\n"
      "        // so behaviour is unchanged until the bridge is wired in.\n"
      '        Log("mode: MHGU crossplay is ON (bridge not wired yet -> using the room path)");\n'
      "    }\n",
      "    if (config_.crossplay) {\n"
      "        if (!InitSockets()) {\n"
      "            PostNotification(SocketInitFailure());\n"
      "            return false;\n"
      "        }\n"
      "        // config_.host holds the hub servers in this mode (space/comma list); empty = the defaults.\n"
      "        const std::string servers =\n"
      "            config_.host.empty()\n"
      '                ? std::string("hunterhubid-gu-hub.duckdns.org morerokk-mhgurecomp-ldnserver.duckdns.org")\n'
      "                : config_.host;\n"
      "        const bool ok = crossplay_ && crossplay_->Start(\n"
      "            servers, config_.password,\n"
      "            [this](const azl::WifiPacket& p) { engine_->OnWifiPacket(p); }, // hub -> game\n"
      "            [this]() { return console_mac_; },\n"
      "            [this](u64 now_ms) {\n"
      "                engine_->Tick(now_ms);\n"
      "                crossplay_->Scan(0, 0); // keep the hub list warm so a search finds MHGU rooms\n"
      "            });\n"
      '        Log("crossplay: %s -> MHGU hub (%s)", ok ? "bridging local play" : "could not start",\n'
      "            servers.c_str());\n"
      "        return ok;\n"
      "    }\n")

print("crossplay phase 2: done")
