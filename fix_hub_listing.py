#!/usr/bin/env python3
"""Step 2 for 3DS-Link crossplay: make the hub list visible in AzaharLink.log and stop it eating threads.
Run AFTER fix_crossplay_freeze.py, from the 3DS-Link folder:  python fix_hub_listing.py
Idempotent. Makes .bak copies."""
import os, sys, shutil
root = sys.argv[1] if len(sys.argv) > 1 else "."
base = next((c for c in (os.path.join(root, "AzaharLink"), root) if os.path.isdir(os.path.join(c, "Sources", "mhgu"))), None)
if not base:
    sys.exit("Run this inside the 3DS-Link folder.")
p = os.path.join(base, "Sources", "mhgu", "mhgu_hub.cpp")
s = open(p, "rb").read().decode("utf-8")
crlf = "\r\n" in s
s = s.replace("\r\n", "\n")
orig = s

def sub(old, new, marker):
    global s
    if marker in s:
        return
    if old not in s:
        sys.exit("PATCH FAILED, text not found:\n" + old)
    s = s.replace(old, new)

# 1) one thread for all hub servers (every thread costs one of the game's 36; the log showed 36/36 and thread creation failing)
sub("""                const std::function<void()> ask = [&, i] { states[i] = H::ListHubs(servers[i], key, 1500, found[i]); };
                ts.emplace_back(ask);
                if (!ts.back().joinable()) {
                    ask(); // no thread for this server: ask it here (slower, but the list still comes)
                }
""", """                // (one server after the other, in this thread: every thread costs one of the game's 36 slots)
                states[i] = H::ListHubs(servers[i], key, 1500, found[i]);
                static const char* const kState[] = {"online", "DNS failed (name not found)", "TCP connect failed/timed out",
                                                     "connected but no answer", "refused (wrong key?)"};
                const int st = static_cast<int>(states[i]);
                LOG_INFO(Service_NWM, "[hub] list {}: {} -> {} room(s)", servers[i], st >= 0 && st < 5 ? kState[st] : "?",
                         found[i].size());
""", "kState[]")

# 2) say how many rooms the servers sent and why they are dropped
sub("""        list_pending = false;
        std::vector<Listed> next;
        for (auto& h : hubs) {
""", """        list_pending = false;
        std::vector<Listed> next;
        {
            static std::size_t last_raw = static_cast<std::size_t>(-1);
            if (hubs.size() != last_raw) {
                last_raw = hubs.size();
                LOG_INFO(Service_NWM, "[hub] the servers sent {} room(s)", hubs.size());
            }
        }
        for (auto& h : hubs) {
            if (H::Rd64(&h.info[H::NI::LocalCommId]) != H::MhguLocalCommId || !H::InfoSane(h.info) ||
                h.info[H::NI::AcceptPolicy] == 1 || h.info[H::NI::NodeCount] >= h.info[H::NI::NodeMax]) {
                LOG_INFO(Service_NWM, "[hub] room {:016x} skipped: commid {:016x} sane {} policy {} nodes {}/{}", h.id,
                         H::Rd64(&h.info[H::NI::LocalCommId]), H::InfoSane(h.info) ? 1 : 0, h.info[H::NI::AcceptPolicy],
                         h.info[H::NI::NodeCount], h.info[H::NI::NodeMax]);
            }
""", "the servers sent {} room(s)")

if s != orig:
    shutil.copyfile(p, p + ".bak2")
    open(p, "wb").write((s.replace("\n", "\r\n") if crlf else s).encode("utf-8"))
    print("patched Sources/mhgu/mhgu_hub.cpp")
else:
    print("already ok")
