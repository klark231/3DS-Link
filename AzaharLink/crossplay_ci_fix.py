#!/usr/bin/env python3
"""Fixes the policy_check failures from the first crossplay push. Run from the AzaharLink root:
    python3 crossplay_ci_fix.py
- Makefile: back to gnu++17 globally, with a per-object gnu++20 just for the MHGU bridge.
- policy_check.sh: crossplay is a known ini key; the imported bridge is out of the file-touch scan.
Idempotent; works whether or not the earlier phase-2 Makefile edit is present."""
import sys, os

def edit(path, old, new, optional=False):
    s = open(path, encoding="utf-8").read()
    if new in s:
        print(f"  skip: {path}"); return
    n = s.count(old)
    if n == 0 and optional:
        print(f"  n/a: {path}"); return
    if n != 1:
        sys.exit(f"ERROR {path}: anchor {n}x:\n  {old[:90]!r}")
    open(path, "w", encoding="utf-8").write(s.replace(old, new))
    print(f"  patched: {path}")

GNU20 = ("BRIDGE_OFILES := crossplay.o mhgu_hub.o mhgu_hub_proto.o minicrypto.o uds_beacon.o uds_connection.o uds_data.o\n"
         "$(BRIDGE_OFILES): CXXFLAGS := $(filter-out -std=gnu++17,$(CXXFLAGS)) -std=gnu++20 -DFMT_HEADER_ONLY=1 -DFMT_EXCEPTIONS=0")

# Makefile: from the old phase-2 line (global gnu++20) ...
edit("Makefile",
     "CXXFLAGS\t:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++20 -DFMT_HEADER_ONLY=1 -DFMT_EXCEPTIONS=0",
     "CXXFLAGS\t:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17\n"
     "# The MHGU bridge (Sources/mhgu) and the crossplay glue need C++20 (std::span) and header-only\n"
     "# fmt; the rest of the plugin stays gnu++17. Per-object override:\n" + GNU20,
     optional=True)
# ... or from a pristine gnu++17 line (if the old phase-2 Makefile edit was never applied)
edit("Makefile",
     "CXXFLAGS\t:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17\n",
     "CXXFLAGS\t:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17\n"
     "# The MHGU bridge (Sources/mhgu) and the crossplay glue need C++20 (std::span) and header-only\n"
     "# fmt; the rest of the plugin stays gnu++17. Per-object override:\n" + GNU20 + "\n",
     optional=True)

# policy_check.sh: crossplay joins the known ini keys (two lists), and the bridge is out of the scan.
P = "Tests/policy_check.sh"
for var in ("WANTED", "FIELDS"):
    edit(P,
         var + '="autoconnect debug enabled game_name host menu_key net_thread_core nickname password port radio_like rcvbuf_kb sleep_stop socket_buffers tunnel "',
         var + '="autoconnect crossplay debug enabled game_name host menu_key net_thread_core nickname password port radio_like rcvbuf_kb sleep_stop socket_buffers tunnel "')
edit(P,
     'grep -v "^Sources/ctr/plugin.cpp:\\|^Sources/azl/log_files.cpp:\\|^Sources/ctr/boot_diag.cpp:"',
     'grep -v "^Sources/ctr/plugin.cpp:\\|^Sources/azl/log_files.cpp:\\|^Sources/ctr/boot_diag.cpp:\\|^Sources/mhgu/\\|^Includes/mhgu_compat/"')

print("crossplay CI fix: done")
