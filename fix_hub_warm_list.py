#!/usr/bin/env python3
"""Step 3: put back what the port lost from Azahar's mhgu_hub.cpp Tick():
keep the hub list warm while idle, and pre-fill the game's beacon buffer, so the FIRST search shows hubs.
Run after the other two patches, from the 3DS-Link folder:  python fix_hub_warm_list.py   (idempotent, makes .bak3)"""
import os, sys, shutil
root = sys.argv[1] if len(sys.argv) > 1 else "."
base = next((c for c in (os.path.join(root, "AzaharLink"), root) if os.path.isdir(os.path.join(c, "Sources", "mhgu"))), None)
if not base:
    sys.exit("Run this inside the 3DS-Link folder.")
p = os.path.join(base, "Sources", "mhgu", "mhgu_hub.cpp")
s = open(p, "rb").read().decode("utf-8")
crlf = "\r\n" in s
s = s.replace("\r\n", "\n")
old = """            LOG_WARNING(Service_NWM, "[hub] the game did not finish connecting to the hub");
            sta = Sta::Idle;
        }
"""
new = old + """        // Keep the hub list warm while idle so the first in-game search already has results.
        // (Azahar does this; the port had lost it: the list was only asked for AFTER the first search, so that search was empty.)
        if (!hosting && sta == Sta::Idle && !list_pending && now - last_list > 5s) {
            RequestList();
        }
        // Pre-fill the game's beacon buffer every tick while it can scan, so the FIRST search already shows hubs
        // (RecvBeaconBroadcastData reads the buffer right after triggering Scan(), before the per-scan delivery
        // Scan() posts to this worker would run).
        if (!hosting && sta == Sta::Idle && !listed.empty()) {
            DeliverBeacons();
        }
"""
if "Keep the hub list warm" in s:
    print("already ok"); sys.exit(0)
if s.count(old) != 1:
    sys.exit("PATCH FAILED: Tick() text not found exactly once")
s = s.replace(old, new)
shutil.copyfile(p, p + ".bak3")
open(p, "wb").write((s.replace("\n", "\r\n") if crlf else s).encode("utf-8"))
print("patched Sources/mhgu/mhgu_hub.cpp")
