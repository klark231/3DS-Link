#!/usr/bin/env python3
"""Scaffold: add a "MHGU Crossplay" mode toggle to 3DS Link (AzaharLink).

Run from the AzaharLink root:  python3 crossplay_scaffold.py

What it does (mirrors the existing `autoconnect` bool end-to-end):
  * a `crossplay` bool in the shared ini Fields, the plugin's Config, and the app's Settings
  * it is parsed from / rendered to AzaharLink.ini (default OFF) — so it round-trips, but it is
    deliberately NOT added to kKeyNames/kKeyCount, so HasAllKeys and old .ini files are unaffected
  * a "MHGU Crossplay" toggle on the ConfigApp settings page
  * a branch stub in Plugin::Connect(): when crossplay is on it logs and (for now) still joins the
    room, so the build is unchanged in behaviour until the bridge is wired in.

This compiles with the normal devkitARM toolchain; it changes no behaviour yet (the stub only logs).
Idempotent: safe to run twice.
"""
import sys

def patch(path, old, new, count=1):
    with open(path, encoding="utf-8") as f:
        s = f.read()
    if new in s:
        print(f"  skip (already applied): {path}")
        return
    n = s.count(old)
    if n != count:
        sys.exit(f"ERROR {path}: anchor found {n}x (expected {count}):\n  {old[:120]!r}")
    with open(path, "w", encoding="utf-8") as f:
        f.write(s.replace(old, new))
    print(f"  patched: {path}")

# ---- 1. shared ini layer: Fields + Render --------------------------------------------------
INI = "Includes/azl/ini_values.hpp"
patch(INI,
      "    bool autoconnect = true;\n"
      "    int radio_like = 1; // 0, 1, 2 (ParseRadioLike)",
      "    bool autoconnect = true;\n"
      "    bool crossplay = false; // route local play to the MHGU crossplay hub instead of an Azahar room\n"
      "    int radio_like = 1; // 0, 1, 2 (ParseRadioLike)")
patch(INI,
      '    out += std::string("autoconnect=") + flag(f.autoconnect) + "\\n";\n'
      '    out += "radio_like=" + std::to_string(f.radio_like) + "\\n";',
      '    out += std::string("autoconnect=") + flag(f.autoconnect) + "\\n";\n'
      '    out += std::string("crossplay=") + flag(f.crossplay) + "\\n";\n'
      '    out += "radio_like=" + std::to_string(f.radio_like) + "\\n";')

# ---- 2. plugin Config struct ---------------------------------------------------------------
PH = "Includes/ctr/plugin.hpp"
patch(PH,
      "    bool autoconnect = true;              ///< join the room as soon as the game has booted\n",
      "    bool autoconnect = true;              ///< join the room as soon as the game has booted\n"
      "    bool crossplay = false;               ///< true: bridge local play to the MHGU crossplay hub instead of an Azahar room\n")

# ---- 3. app Settings struct ----------------------------------------------------------------
SH = "ConfigApp/source/settings.hpp"
patch(SH,
      "    bool autoconnect = true;\n"
      "    int radio_like = 1;",
      "    bool autoconnect = true;\n"
      "    bool crossplay = false; // MHGU crossplay mode instead of an Azahar room\n"
      "    int radio_like = 1;")

# ---- 4. app settings.cpp: parse + render assign --------------------------------------------
SC = "ConfigApp/source/settings.cpp"
patch(SC,
      '        } else if (key == "autoconnect") {\n'
      "            cfg.autoconnect = azl::ini::ParseBool(value, true);\n",
      '        } else if (key == "autoconnect") {\n'
      "            cfg.autoconnect = azl::ini::ParseBool(value, true);\n"
      '        } else if (key == "crossplay") {\n'
      "            cfg.crossplay = azl::ini::ParseBool(value, false);\n")
patch(SC,
      "    fields.autoconnect = cfg.autoconnect;\n",
      "    fields.autoconnect = cfg.autoconnect;\n"
      "    fields.crossplay = cfg.crossplay;\n")

# ---- 5. plugin.cpp: parse + SaveConfig assign + Connect() stub ------------------------------
PC = "Sources/ctr/plugin.cpp"
patch(PC,
      '        } else if (key == "autoconnect") {\n'
      "            config_.autoconnect = ini::ParseBool(value, true);\n",
      '        } else if (key == "autoconnect") {\n'
      "            config_.autoconnect = ini::ParseBool(value, true);\n"
      '        } else if (key == "crossplay") {\n'
      "            config_.crossplay = ini::ParseBool(value, false);\n")
patch(PC,
      "    fields.autoconnect = config_.autoconnect;\n",
      "    fields.autoconnect = config_.autoconnect;\n"
      "    fields.crossplay = config_.crossplay;\n")
# Connect() stub: log the mode at the top of the room-join path.
patch(PC,
      "bool Plugin::Connect() {\n"
      "    if (config_.host.empty()) {\n"
      '        PostNotification("Set the room address first");\n'
      "        return false;\n"
      "    }\n",
      "bool Plugin::Connect() {\n"
      "    if (config_.crossplay) {\n"
      "        // TODO(crossplay): drive the MHGU bridge (mhgu_hub + mhgu_hub_proto) to the hub here\n"
      "        // instead of joining a room. For now this only logs and falls through to the room path,\n"
      "        // so behaviour is unchanged until the bridge is wired in.\n"
      '        Log("mode: MHGU crossplay is ON (bridge not wired yet -> using the room path)");\n'
      "    }\n"
      "    if (config_.host.empty()) {\n"
      '        PostNotification("Set the room address first");\n'
      "        return false;\n"
      "    }\n")

# ---- 6. ConfigApp UI: enum + Row + handler -------------------------------------------------
PG = "ConfigApp/source/pages_config.cpp"
patch(PG,
      "    enum Id { kAutoconnect = 1, kRadio, kSleep, kDebug, kMenu, kTest, kCheck, kAbout, kNickname, kDlpDelivery, kCheckUpdates, kUpdateAtStart };",
      "    enum Id { kAutoconnect = 1, kRadio, kSleep, kDebug, kMenu, kTest, kCheck, kAbout, kNickname, kDlpDelivery, kCheckUpdates, kUpdateAtStart, kCrossplay };")
patch(PG,
      "        add(autoconnect);\n",
      "        add(autoconnect);\n\n"
      "        Row crossplay;\n"
      "        crossplay.id = kCrossplay;\n"
      "        crossplay.kind = Kind::Toggle;\n"
      '        crossplay.label = "MHGU Crossplay";\n'
      "        crossplay.on = g.cfg.crossplay;\n"
      '        crossplay.note_title = "MHGU Crossplay";\n'
      '        crossplay.note = "ON: local play is bridged to the MHGU crossplay hub, to play with MHGU "\n'
      '                         "(Switch/Android) players.\\n"\n'
      '                         "OFF: normal 3DS Link (join an Azahar room).";\n'
      "        add(crossplay);\n")
patch(PG,
      "        case kAutoconnect:\n"
      "            if (SetSwitch(g.cfg.autoconnect, step)) {\n"
      "                g.Save();\n"
      "            }\n"
      "            break;\n",
      "        case kAutoconnect:\n"
      "            if (SetSwitch(g.cfg.autoconnect, step)) {\n"
      "                g.Save();\n"
      "            }\n"
      "            break;\n"
      "        case kCrossplay:\n"
      "            if (SetSwitch(g.cfg.crossplay, step)) {\n"
      "                g.Save();\n"
      "            }\n"
      "            break;\n")

print("crossplay scaffold: done")
