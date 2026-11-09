import re, sys

def edit(path, fn):
    raw = open(path, "rb").read().decode("utf-8")
    crlf = "\r\n" in raw
    s = raw.replace("\r\n", "\n")
    s2 = fn(s)
    if s2 is None:
        print("FAILED:", path)
        return False
    if crlf:
        s2 = s2.replace("\n", "\r\n")
    open(path, "wb").write(s2.encode("utf-8"))
    print("patched:", path)
    return True

HPP_ADD = """/// Same as ParseEapolStartFrame, but lenient about the connection type: 0 and 1 are a
/// client, 2 a spectator, anything else a client. *legacy is true if the type wasn't 1 or 2.
bool ParseCompatibleEapolStartFrame(const std::vector<u8>& frame, EapolStart* out, bool* legacy = nullptr);
"""

CPP_ADD = """bool ParseCompatibleEapolStartFrame(const std::vector<u8>& frame, EapolStart* out, bool* legacy) {
    if (!ParseEapolStartFrame(frame, out)) {
        return false;
    }
    bool is_legacy = true;
    switch (out->connection_type) {
    case 1:
        out->connection_type = static_cast<u8>(ConnectionType::Client);
        is_legacy = false;
        break;
    case 2:
        out->connection_type = static_cast<u8>(ConnectionType::Spectator);
        is_legacy = false;
        break;
    default: // 0 and anything unknown
        out->connection_type = static_cast<u8>(ConnectionType::Client);
        break;
    }
    if (legacy) {
        *legacy = is_legacy;
    }
    return true;
}

"""

def hpp(s):
    if "ParseCompatibleEapolStartFrame" in s:
        return s
    m = re.search(r"^bool ParseEapolStartFrame\([^;]*\);\n", s, re.M)
    return s[:m.end()] + HPP_ADD + s[m.end():] if m else None

def cpp_frames(s):
    if "ParseCompatibleEapolStartFrame" in s:
        return s
    m = re.search(r"^std::vector<u8> GenerateEapolLogoffFrame\(", s, re.M)
    return s[:m.start()] + CPP_ADD + s[m.start():] if m else None

def engine(s):
    if "ParseCompatibleEapolStartFrame" in s:
        return s
    pat = re.compile(r"([ \t]*)EapolStart start;\n[ \t]*if \(!ParseEapolStartFrame\(packet\.data, &start\)\) \{\n[ \t]*return;\n[ \t]*\}\n")
    m = pat.search(s)
    if not m:
        return None
    ind = m.group(1)
    new = (f"{ind}EapolStart start;\n"
           f"{ind}bool legacy_start = false;\n"
           f"{ind}if (!ParseCompatibleEapolStartFrame(packet.data, &start, &legacy_start)) {{\n"
           f"{ind}    return;\n"
           f"{ind}}}\n"
           f"{ind}if (legacy_start) {{\n"
           f"{ind}    Log(\"uds: odd connection type in EAPoL start, taking it as client\");\n"
           f"{ind}}}\n")
    return s[:m.start()] + new + s[m.end():]

base = "AzaharLink/"
ok = all([
    edit(base + "Includes/azl/uds_frames.hpp", hpp),
    edit(base + "Sources/azl/uds_frames.cpp", cpp_frames),
    edit(base + "Sources/azl/uds_engine.cpp", engine),
])
sys.exit(0 if ok else 1)
