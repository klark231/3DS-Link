#!/usr/bin/env python3
"""Fix the 'Connect to room' freeze in 3DS-Link crossplay.
Usage (Termux):  cd ~/3DS-Link   (the folder that contains AzaharLink/)
                 python fix_crossplay_freeze.py
Idempotent: safe to run twice. Makes .bak copies of every file it touches."""
import os, sys, shutil

root = sys.argv[1] if len(sys.argv) > 1 else "."
base = None
for cand in (os.path.join(root, "AzaharLink"), root):
    if os.path.isdir(os.path.join(cand, "Sources", "mhgu")):
        base = cand
        break
if not base:
    sys.exit("Run this inside the 3DS-Link folder (the one containing AzaharLink/Sources/mhgu).")

def rd(p):
    with open(p, "rb") as f:
        return f.read().decode("utf-8")
def wr(p, s):
    with open(p, "wb") as f:
        f.write(s.encode("utf-8"))

def patch(rel, edits):
    p = os.path.join(base, rel)
    s = rd(p)
    crlf = "\r\n" in s
    s = s.replace("\r\n", "\n")
    orig = s
    for old, new, marker in edits:
        if marker in s:
            continue  # already applied
        if s.count(old) < 1:
            sys.exit("PATCH FAILED in %s: text not found:\n%s" % (rel, old))
        s = s.replace(old, new)
    if s != orig:
        shutil.copyfile(p, p + ".bak")
        wr(p, s.replace("\n", "\r\n") if crlf else s)
        print("patched", rel)
    else:
        print("already ok", rel)

# 1) portable sleep helper (svcSleepThread on the console: std::sleep_for / cond_var timed waits are not reliable there)
helper = os.path.join(base, "Includes", "azl", "bridge_sleep.hpp")
if not os.path.exists(helper):
    wr(helper, '''// Sleep for the MHGU bridge threads. On the console a std::condition_variable timed wait / std::this_thread::sleep_for can
// return at once; the bridge worker then spins at a priority above the game's and starves it (game frozen on "Connect to room").
// svcSleepThread always blocks.
#pragma once
#include <chrono>
#include <thread>
#ifdef __3DS__
#include <3ds.h>
#endif
namespace azl {
inline void BridgeSleepMs(unsigned ms) {
#ifdef __3DS__
    svcSleepThread(static_cast<long long>(ms) * 1000000LL);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}
} // namespace azl
''')
    print("created Includes/azl/bridge_sleep.hpp")

# 2) SafeThread: bridge threads below the game's priority (0x30). Higher number = lower priority.
patch("Includes/azl/safe_thread.hpp", [
    ("if (t->Start(std::function<void()>(std::forward<F>(fn)))) {",
     "if (t->Start(std::function<void()>(std::forward<F>(fn)), platform::kCoreDefault, 0x38)) { // (0x38: below the game's 0x30, never starve it)",
     "platform::kCoreDefault, 0x38"),
])

# 3) mhgu_hub.cpp: no condition_variable wait, no sleep_for
patch("Sources/mhgu/mhgu_hub.cpp", [
    ('#include "azl/once.hpp"\n', '#include "azl/bridge_sleep.hpp"\n#include "azl/once.hpp"\n', "azl/bridge_sleep.hpp"),
    ("            if (tasks.empty()) {\n                task_cv.wait_for(lock, 100ms);\n            }\n",
     "            if (tasks.empty()) {\n                lock.unlock();\n                azl::BridgeSleepMs(10); // (was task_cv.wait_for(100ms): returned at once on the console -> busy loop)\n                lock.lock();\n            }\n",
     "azl::BridgeSleepMs(10)"),
    ("return box.Get([] { std::this_thread::sleep_for(1ms); }).gen;", "return box.Get([] { azl::BridgeSleepMs(1); }).gen;", "box.Get([] { azl::BridgeSleepMs(1)"),
    ("                std::this_thread::sleep_for(100ms);\n            }\n            worker =", "                azl::BridgeSleepMs(100);\n            }\n            worker =", "azl::BridgeSleepMs(100);\n            }\n            worker ="),
    ("                std::this_thread::sleep_for(50ms);\n                HostHello", "                azl::BridgeSleepMs(50);\n                HostHello", "azl::BridgeSleepMs(50)"),
    ("return bridge.Get([] { std::this_thread::sleep_for(1ms); });", "return bridge.Get([] { azl::BridgeSleepMs(1); });", "bridge.Get([] { azl::BridgeSleepMs(1)"),
])

# 4) hub listing wait loop
patch("Sources/mhgu/mhgu_hub_proto.cpp", [
    ('#include "azl/safe_thread.hpp"\n', '#include "azl/bridge_sleep.hpp"\n#include "azl/safe_thread.hpp"\n', "azl/bridge_sleep.hpp"),
    ("std::this_thread::sleep_for(std::chrono::milliseconds(100));", "azl::BridgeSleepMs(100);", "azl::BridgeSleepMs(100);"),
])

# 5) crossplay.cpp: tick thread sleep + bridge messages into AzaharLink.log + progress logs
patch("Sources/ctr/crossplay.cpp", [
    ('#include "azl/safe_thread.hpp"\n', '#include "azl/bridge_sleep.hpp"\n#include "azl/log.hpp"\n#include "azl/safe_thread.hpp"\n', "azl/log.hpp"),
    ("void Log(int /*level*/, const std::string& /*text*/) {\n    // Routed to the plugin's own log elsewhere if wanted; silent by default to keep this file\n    // free of the 3DS logging headers.\n}",
     "void Log(int level, const std::string& text) {\n    if (level >= 4) {\n        azl::Log(\"hub: %s\", text.c_str()); // (the bridge's own messages, now in AzaharLink.log)\n    }\n}",
     'azl::Log("hub: %s"'),
    ("    impl_->deliver = std::move(deliver);\n    impl_->own_mac = std::move(own_mac);",
     "    azl::Log(\"crossplay: Start begins (hubs '%s')\", servers.c_str());\n    impl_->deliver = std::move(deliver);\n    impl_->own_mac = std::move(own_mac);",
     "crossplay: Start begins"),
    ("    if (!attached) {\n", "    azl::Log(\"crossplay: Bridge::Attach -> %s\", attached ? \"ok\" : \"FAILED\");\n    if (!attached) {\n", "crossplay: Bridge::Attach ->"),
    ("                impl_->tick(static_cast<std::uint64_t>(now.count()));\n                std::this_thread::sleep_for(milliseconds(200));",
     "                impl_->tick(static_cast<std::uint64_t>(now.count()));\n                azl::BridgeSleepMs(200);",
     "azl::BridgeSleepMs(200)"),
])
print("\nDone. Rebuild (make) and test. Originals are saved as *.bak")
