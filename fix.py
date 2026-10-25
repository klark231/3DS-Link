import re, os

def edit(path, fn):
    if not os.path.exists(path):
        print("MISSING", path); return
    d = open(path, "rb").read().decode("utf-8")
    n = fn(d)
    if n != d:
        open(path, "wb").write(n.encode("utf-8")); print("fixed  ", path)
    else:
        print("ok     ", path)

# 1) byte-for-byte cmp of the CIA can never pass (makerom draws random fields)
edit("DlpLink/tests/dlpc_check.sh",
     lambda d: re.sub(r'^[ \t]*cmp "\$W/run/composed(\.emu)?\.cia"[^\n]*\n', '', d, flags=re.M))

# 2) saved rooms back to 10 (50 uses 5x the RAM)
edit("AzaharLink/ConfigApp/source/settings.hpp",
     lambda d: d.replace("kMax = 50;", "kMax = 10;"))
edit("AzaharLink/Tests/policy_check.sh",
     lambda d: d.replace('"static constexpr std::size_t kMax = 50;" "the list of saved rooms holds 50"',
                         '"static constexpr std::size_t kMax = 10;" "the list of saved rooms holds 10"'))

# 3) scan filter OFF (cross-region): source and policy agree
edit("AzaharLink/Includes/azl/uds_engine.hpp",
     lambda d: d.replace("scan_filter_enforced_ = true;", "scan_filter_enforced_ = false;"))
edit("AzaharLink/Tests/policy_check.sh",
     lambda d: d.replace("scan_filter_enforced_ = true;", "scan_filter_enforced_ = false;"))

# 4) CI shows the failing lines of a failed step
edit("DlpLink/tools/build_cias.sh",
     lambda d: d if "refuses|differ" in d else d.replace(
         'tail -25 "$tmp"',
         '{ grep -nE "FAIL|ERROR|refuses|differ" "$tmp" | head -40; echo "--- last 25 ---"; tail -25 "$tmp"; }'))
