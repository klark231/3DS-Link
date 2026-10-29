// In-game chat, the part that needs no console: the cleaning of the text, the lines kept for the overlay, and the on-screen touch
// keyboard as a state machine (where the keys are, what a tap does). Pure C++ (no 3ds.h, no CTRPF): Tests/chat_test.cpp runs it on a PC.
// The drawing and the touch reading are in Sources/ctr/chat_overlay.cpp (only built with -DAZL_CHAT_OVERLAY).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace azl::chat {

constexpr std::size_t kLines = 4;          ///< lines of incoming chat shown at once
constexpr std::size_t kLineChars = 52;     ///< characters of one shown line (the top screen is 400 px, the font 6 px: 66 fit; the rest is margin)
constexpr std::uint64_t kShowMs = 9000;    ///< how long a line stays on the screen
constexpr std::size_t kTypedChars = 100;   ///< what one can type (the room relays up to 500)

/// What may be drawn: printable ASCII only. Control characters and every byte of a multi-byte character are dropped (the overlay font is
/// ASCII; chat comes from other people's consoles and PCs: it is never trusted), spaces are collapsed, the result is trimmed and cut at `max`.
inline std::string Clean(const std::string& in, std::size_t max) {
    std::string out;
    bool space = true;
    for (unsigned char c : in) {
        if (c == ' ' || c == '\t') {
            if (!space) {
                out.push_back(' ');
                space = true;
            }
        } else if (c > 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
            space = false;
        }
        if (out.size() >= max) {
            break;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

/// The last lines received (or sent by us). Push comes from the network thread, Read from the game's frame: a tiny spin lock guards the slots.
class Log {
public:
    struct Line {
        std::string text;
        std::uint64_t at_ms = 0;
    };

    void Push(const std::string& nick, const std::string& message, std::uint64_t now_ms) {
        std::string n = Clean(nick, 12);
        std::string m = Clean(message, 200);
        if (m.empty()) {
            return;
        }
        std::string line = (n.empty() ? std::string("?") : n) + ": " + m;
        if (line.size() > kLineChars) {
            line.resize(kLineChars - 2);
            line += "..";
        }
        Lock();
        lines_[next_ % kLines].text = std::move(line);
        lines_[next_ % kLines].at_ms = now_ms ? now_ms : 1;
        ++next_;
        Unlock();
    }

    /// The lines still to be shown, oldest first. Returns how many were written to `out` (room for kLines).
    std::size_t Visible(std::uint64_t now_ms, Line* out) const {
        std::size_t n = 0;
        Lock();
        const std::size_t count = next_ < kLines ? next_ : kLines;
        for (std::size_t i = 0; i < count; ++i) {
            const Line& l = lines_[(next_ - count + i) % kLines];
            if (l.at_ms != 0 && now_ms >= l.at_ms && now_ms - l.at_ms < kShowMs) {
                out[n++] = l;
            }
        }
        Unlock();
        return n;
    }

private:
    void Lock() const {
        while (flag_.test_and_set(std::memory_order_acquire)) {
        }
    }
    void Unlock() const {
        flag_.clear(std::memory_order_release);
    }
    mutable std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    Line lines_[kLines];
    std::size_t next_ = 0;
};

/// A key of the touch keyboard (bottom screen, 320 x 240).
struct Key {
    int x, y, w, h;
    char normal;        ///< what it types; 0 for the special keys
    char shifted;
    enum Kind { Char, Shift, Back, Space, Send, Close } kind;
    const char* label;
};

constexpr int kTextBarY = 96;   ///< the line that shows what is typed (the keys start at kKeysY)
constexpr int kKeysY = 120;
constexpr int kKeyH = 24;

/// The button that opens the keyboard (corner of the bottom screen, drawn while the keyboard is closed).
constexpr Key kChatButton = {262, 2, 56, 18, 0, 0, Key::Char, "CHAT"};

inline const Key* Keys(std::size_t* count) {
    static const Key keys[] = {
        // row 0: digits (shifted: symbols)
        {0, kKeysY, 32, kKeyH, '1', '!', Key::Char, nullptr}, {32, kKeysY, 32, kKeyH, '2', '@', Key::Char, nullptr},
        {64, kKeysY, 32, kKeyH, '3', '#', Key::Char, nullptr}, {96, kKeysY, 32, kKeyH, '4', '$', Key::Char, nullptr},
        {128, kKeysY, 32, kKeyH, '5', '%', Key::Char, nullptr}, {160, kKeysY, 32, kKeyH, '6', '^', Key::Char, nullptr},
        {192, kKeysY, 32, kKeyH, '7', '&', Key::Char, nullptr}, {224, kKeysY, 32, kKeyH, '8', '*', Key::Char, nullptr},
        {256, kKeysY, 32, kKeyH, '9', '(', Key::Char, nullptr}, {288, kKeysY, 32, kKeyH, '0', ')', Key::Char, nullptr},
        // row 1
        {0, kKeysY + 24, 32, kKeyH, 'q', 'Q', Key::Char, nullptr}, {32, kKeysY + 24, 32, kKeyH, 'w', 'W', Key::Char, nullptr},
        {64, kKeysY + 24, 32, kKeyH, 'e', 'E', Key::Char, nullptr}, {96, kKeysY + 24, 32, kKeyH, 'r', 'R', Key::Char, nullptr},
        {128, kKeysY + 24, 32, kKeyH, 't', 'T', Key::Char, nullptr}, {160, kKeysY + 24, 32, kKeyH, 'y', 'Y', Key::Char, nullptr},
        {192, kKeysY + 24, 32, kKeyH, 'u', 'U', Key::Char, nullptr}, {224, kKeysY + 24, 32, kKeyH, 'i', 'I', Key::Char, nullptr},
        {256, kKeysY + 24, 32, kKeyH, 'o', 'O', Key::Char, nullptr}, {288, kKeysY + 24, 32, kKeyH, 'p', 'P', Key::Char, nullptr},
        // row 2 (nine keys, centred)
        {16, kKeysY + 48, 32, kKeyH, 'a', 'A', Key::Char, nullptr}, {48, kKeysY + 48, 32, kKeyH, 's', 'S', Key::Char, nullptr},
        {80, kKeysY + 48, 32, kKeyH, 'd', 'D', Key::Char, nullptr}, {112, kKeysY + 48, 32, kKeyH, 'f', 'F', Key::Char, nullptr},
        {144, kKeysY + 48, 32, kKeyH, 'g', 'G', Key::Char, nullptr}, {176, kKeysY + 48, 32, kKeyH, 'h', 'H', Key::Char, nullptr},
        {208, kKeysY + 48, 32, kKeyH, 'j', 'J', Key::Char, nullptr}, {240, kKeysY + 48, 32, kKeyH, 'k', 'K', Key::Char, nullptr},
        {272, kKeysY + 48, 32, kKeyH, 'l', 'L', Key::Char, nullptr},
        // row 3: shift, seven letters, backspace
        {0, kKeysY + 72, 48, kKeyH, 0, 0, Key::Shift, "Aa"}, {48, kKeysY + 72, 32, kKeyH, 'z', 'Z', Key::Char, nullptr},
        {80, kKeysY + 72, 32, kKeyH, 'x', 'X', Key::Char, nullptr}, {112, kKeysY + 72, 32, kKeyH, 'c', 'C', Key::Char, nullptr},
        {144, kKeysY + 72, 32, kKeyH, 'v', 'V', Key::Char, nullptr}, {176, kKeysY + 72, 32, kKeyH, 'b', 'B', Key::Char, nullptr},
        {208, kKeysY + 72, 32, kKeyH, 'n', 'N', Key::Char, nullptr}, {240, kKeysY + 72, 32, kKeyH, 'm', 'M', Key::Char, nullptr},
        {272, kKeysY + 72, 48, kKeyH, 0, 0, Key::Back, "<-"},
        // row 4: close, space, . ? and send
        {0, kKeysY + 96, 56, kKeyH, 0, 0, Key::Close, "Close"}, {56, kKeysY + 96, 160, kKeyH, ' ', ' ', Key::Space, "space"},
        {216, kKeysY + 96, 32, kKeyH, '.', ',', Key::Char, nullptr}, {248, kKeysY + 96, 32, kKeyH, '?', '!', Key::Char, nullptr},
        {280, kKeysY + 96, 40, kKeyH, 0, 0, Key::Send, "Send"},
    };
    if (count) {
        *count = sizeof(keys) / sizeof(keys[0]);
    }
    return keys;
}

/// The keyboard as a state machine: feed it the touch state once per frame. A tap acts on the press (not on the release); the
/// backspace repeats while held. Nothing here touches the game: the overlay draws what this says.
class Keyboard {
public:
    enum class Event { None, Opened, Closed, Send };

    Event OnTouch(bool down, int x, int y, std::uint64_t now_ms) {
        const bool pressed = down && !was_down_;
        const bool released = !down && was_down_;
        was_down_ = down;
        if (!down) {
            held_ = nullptr;
        }
        if (!open_) {
            if (pressed && Inside(kChatButton, x, y)) {
                open_ = true;
                shift_ = false;
                held_ = &kChatButton;
                return Event::Opened;
            }
            return Event::None;
        }
        if (pressed) {
            held_ = Find(x, y);
            repeat_at_ = now_ms + 450;
            return held_ ? Act(*held_) : Event::None;
        }
        if (down && held_ && held_->kind == Key::Back && now_ms >= repeat_at_) { // a held backspace erases on
            repeat_at_ = now_ms + 70;
            return Act(*held_);
        }
        (void)released;
        return Event::None;
    }

    bool is_open() const {
        return open_;
    }
    bool shift() const {
        return shift_;
    }
    const std::string& text() const {
        return text_;
    }
    const Key* held() const {
        return held_;
    }
    /// The message to send (after Event::Send); the box is empty again and the keyboard stays open for the next line.
    std::string TakeMessage() {
        std::string m = Clean(sent_, kTypedChars);
        sent_.clear();
        return m;
    }
    /// What the text bar shows: the end of what is typed, `width` characters at most.
    std::string Tail(std::size_t width) const {
        return text_.size() <= width ? text_ : text_.substr(text_.size() - width);
    }
    void Close() {
        open_ = false;
        held_ = nullptr;
    }

private:
    static bool Inside(const Key& k, int x, int y) {
        return x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h;
    }
    static const Key* Find(int x, int y) {
        std::size_t n = 0;
        const Key* keys = Keys(&n);
        for (std::size_t i = 0; i < n; ++i) {
            if (Inside(keys[i], x, y)) {
                return &keys[i];
            }
        }
        return nullptr;
    }
    Event Act(const Key& k) {
        switch (k.kind) {
        case Key::Char:
        case Key::Space:
            if (text_.size() < kTypedChars) {
                text_.push_back(shift_ ? k.shifted : k.normal);
            }
            if (k.kind == Key::Char && shift_) {
                shift_ = false; // Shift lasts for one character (a capital or a symbol), then it is off again
            }
            return Event::None;
        case Key::Shift:
            shift_ = !shift_;
            return Event::None;
        case Key::Back:
            if (!text_.empty()) {
                text_.pop_back();
            }
            return Event::None;
        case Key::Send:
            sent_ = text_;
            text_.clear();
            return Clean(sent_, kTypedChars).empty() ? Event::None : Event::Send;
        case Key::Close:
            open_ = false;
            return Event::Closed;
        }
        return Event::None;
    }

    bool open_ = false;
    bool shift_ = false;
    bool was_down_ = false;
    const Key* held_ = nullptr;
    std::uint64_t repeat_at_ = 0;
    std::string text_;
    std::string sent_;
};

/// Hides the touch screen from the game while the keyboard is open. The game reads the touch straight from the HID shared page (no service
/// call to catch), so the only way is to clear it there. The layout of that page is not trusted: the scrubber LEARNS where the current
/// touch lives (a word that holds exactly the position the overlay just read, a pad-state word that has the touch key set) and only ever
/// writes to words it has seen holding the live touch. Nothing is learned when nobody touches, so nothing is written then.
/// `page` is the shared page as 32-bit words (libctru's hidSharedMem); the ranges follow 3dbrew's HID layout (pad entries at 0x28,
/// the touch block at 0xA8) and only bound the search.
class TouchScrubber {
public:
    static constexpr std::size_t kTouchFirst = 0xA8 / 4;
    static constexpr std::size_t kTouchEnd = 0x140 / 4;
    static constexpr std::size_t kPadFirst = 0x28 / 4;
    static constexpr std::size_t kPadEnd = 0xA8 / 4;
    static constexpr std::uint32_t kKeyTouch = 1u << 20;
    static constexpr std::size_t kMaxSlots = 24;

    /// While the finger is down: remember where its position and its key bit are.
    void Learn(const volatile std::uint32_t* page, unsigned x, unsigned y) {
        if (!page || (x == 0 && y == 0)) {
            return;
        }
        const std::uint32_t xy = (x & 0xFFFFu) | ((y & 0xFFFFu) << 16);
        for (std::size_t w = kTouchFirst; w < kTouchEnd; ++w) {
            if (page[w] == xy) {
                AddTouch(static_cast<std::uint16_t>(w));
            }
        }
        for (std::size_t w = kPadFirst; w < kPadEnd; ++w) {
            if ((page[w] & kKeyTouch) != 0 && (w - kPadFirst) % 4 < 3) { // (current, pressed, released of an entry)
                pad_seen_.store(true, std::memory_order_release);
            }
        }
    }

    /// Clears what was learned (the position words, the touch key in every pad entry). Safe to call from another thread, any time.
    void Scrub(volatile std::uint32_t* page) const {
        if (!page) {
            return;
        }
        const std::size_t n = n_touch_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < n; ++i) {
            page[touch_[i]] = 0;
        }
        if (pad_seen_.load(std::memory_order_acquire)) {
            for (std::size_t w = kPadFirst; w < kPadEnd; ++w) {
                if ((w - kPadFirst) % 4 < 3) {
                    const std::uint32_t v = page[w];
                    if ((v & kKeyTouch) != 0) {
                        page[w] = v & ~kKeyTouch;
                    }
                }
            }
        }
    }

    /// While the touch is being hidden the framework's own touch reading sees the cleared page almost all the time. The scrubber therefore
    /// looks at the learned words BEFORE it clears them: a fresh non-zero position there means the HID service has just written a touch. Gives
    /// the position of the first learned word that holds one (x in the low half, y in the high half).
    bool Peek(const volatile std::uint32_t* page, std::uint32_t& xy) const {
        if (!page) {
            return false;
        }
        const std::size_t n = n_touch_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t v = page[touch_[i]];
            if (v != 0) {
                xy = v;
                return true;
            }
        }
        return false;
    }

    std::size_t touch_slots() const {
        return n_touch_.load(std::memory_order_acquire);
    }
    bool pad_learned() const {
        return pad_seen_.load(std::memory_order_acquire);
    }

private:
    void AddTouch(std::uint16_t w) {
        const std::size_t n = n_touch_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < n; ++i) {
            if (touch_[i] == w) {
                return;
            }
        }
        if (n < kMaxSlots) {
            touch_[n] = w;
            n_touch_.store(n + 1, std::memory_order_release);
        }
    }
    std::atomic<std::size_t> n_touch_{0};
    std::atomic<bool> pad_seen_{false};
    std::uint16_t touch_[kMaxSlots] = {};
};

// ---------------------------------------------------------------------------------------------
// Switching rooms from the overlay: the saved room lists and the picker (no console needed to test them)
// ---------------------------------------------------------------------------------------------

constexpr int kRoomLists = 10;             ///< servers.txt (list 1), servers2.txt ... servers10.txt
constexpr std::size_t kRoomsPerList = 50;  ///< rooms read from one list file (the app writes 10; a hand-made file may hold more)
constexpr std::size_t kRoomRows = 5;       ///< rooms shown at once
constexpr std::uint16_t kDefaultRoomPort = 24872;

struct RoomEntry {
    std::string name;     ///< may be empty
    std::string host;
    std::uint16_t port = kDefaultRoomPort;
    std::string password; ///< may be empty
};

/// The file of list `n` (1..kRoomLists): "servers.txt" for 1 (the file the app has always written), "servers<n>.txt" after.
inline std::string RoomListFile(int n) {
    return n <= 1 ? std::string("servers.txt") : "servers" + std::to_string(n) + ".txt";
}

/// One line of a list file: "host[:port]", "host[:port]|name" or "host[:port]|name|password" (the format of the app's servers.txt; the
/// password is everything after the second bar). A blank line, a comment (#) or a line whose host or port is not valid gives false.
inline bool ParseRoomLine(const std::string& raw, RoomEntry& out) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    std::size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }
    line.erase(0, start);
    if (line.empty() || line[0] == '#') {
        return false;
    }
    const std::size_t bar1 = line.find('|');
    std::string address = line.substr(0, bar1);
    RoomEntry e;
    if (bar1 != std::string::npos) {
        const std::size_t bar2 = line.find('|', bar1 + 1);
        e.name = Clean(line.substr(bar1 + 1, bar2 == std::string::npos ? std::string::npos : bar2 - bar1 - 1), 12);
        if (bar2 != std::string::npos) {
            e.password = line.substr(bar2 + 1);
            if (e.password.size() > 64) {
                e.password.resize(64);
            }
        }
    }
    std::string host = address;
    const std::size_t colon = address.rfind(':');
    if (colon != std::string::npos) {
        host = address.substr(0, colon);
        const std::string port_text = address.substr(colon + 1);
        if (port_text.empty() || port_text.size() > 5) {
            return false;
        }
        unsigned long port = 0;
        for (char c : port_text) {
            if (c < '0' || c > '9') {
                return false;
            }
            port = port * 10 + static_cast<unsigned long>(c - '0');
        }
        if (port == 0 || port > 65535) {
            return false;
        }
        e.port = static_cast<std::uint16_t>(port);
    }
    if (host.empty() || host.size() > 100) {
        return false;
    }
    for (unsigned char c : host) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    e.host = host;
    out = e;
    return true;
}

/// The rooms of a list file's text, in file order, at most `max`.
inline std::vector<RoomEntry> ParseRoomList(const std::string& text, std::size_t max = kRoomsPerList) {
    std::vector<RoomEntry> rooms;
    std::size_t pos = 0;
    while (pos < text.size() && rooms.size() < max) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        RoomEntry e;
        if (ParseRoomLine(text.substr(pos, end - pos), e)) {
            rooms.push_back(e);
        }
        pos = end + 1;
    }
    return rooms;
}

/// What a row shows: the name, else host:port (the port left out when it is the default).
inline std::string RoomLabel(const RoomEntry& e) {
    std::string text = e.host;
    if (e.port != kDefaultRoomPort) {
        text += ":" + std::to_string(e.port);
    }
    return Clean(e.name, 12).empty() ? text : Clean(e.name, 12) + "  " + text;
}

/// The button that opens the picker (left of CHAT).
constexpr Key kRoomsButton = {204, 2, 56, 18, 0, 0, Key::Char, "ROOMS"};
/// The picker (bottom screen): a header with Close, kRoomRows rows, then Up / Down / List.
constexpr Key kPickClose = {262, 2, 56, 18, 0, 0, Key::Close, "Close"};
constexpr int kRowsY = 26;
constexpr int kRowH = 24;
constexpr Key kPickUp = {4, 154, 60, 24, 0, 0, Key::Char, "Up"};
constexpr Key kPickDown = {68, 154, 60, 24, 0, 0, Key::Char, "Down"};
constexpr Key kPickList = {132, 154, 184, 24, 0, 0, Key::Char, nullptr}; // "List n/5": the label is drawn with the number

/// The room picker as a state machine: feed it the touch once per frame. The overlay owns the list (SetRooms) and loads the file
/// of the list the picker asks for (Event::ChangeList). A tap acts on the press.
class RoomPicker {
public:
    enum class Event { None, Opened, Closed, Pick, ChangeList };

    Event OnTouch(bool down, int x, int y) {
        const bool pressed = down && !was_down_;
        was_down_ = down;
        if (!pressed) {
            return Event::None;
        }
        if (!open_) {
            if (Inside(kRoomsButton, x, y)) {
                open_ = true;
                return Event::Opened;
            }
            return Event::None;
        }
        if (Inside(kPickClose, x, y)) {
            open_ = false;
            return Event::Closed;
        }
        if (Inside(kPickUp, x, y)) {
            if (first_ >= kRoomRows) {
                first_ -= kRoomRows;
            } else {
                first_ = 0;
            }
            return Event::None;
        }
        if (Inside(kPickDown, x, y)) {
            if (first_ + kRoomRows < rooms_.size()) {
                first_ += kRoomRows;
            }
            return Event::None;
        }
        if (Inside(kPickList, x, y)) {
            list_ = list_ % kRoomLists + 1;
            first_ = 0;
            rooms_.clear();
            return Event::ChangeList;
        }
        if (y >= kRowsY && y < kRowsY + static_cast<int>(kRoomRows) * kRowH) {
            const std::size_t i = first_ + static_cast<std::size_t>((y - kRowsY) / kRowH);
            if (i < rooms_.size()) {
                picked_ = i;
                open_ = false;
                return Event::Pick;
            }
        }
        return Event::None;
    }

    void SetRooms(std::vector<RoomEntry> rooms) {
        rooms_ = std::move(rooms);
        first_ = 0;
    }
    const RoomEntry& picked() const {
        return rooms_[picked_ < rooms_.size() ? picked_ : 0];
    }
    bool has_picked() const {
        return picked_ < rooms_.size();
    }
    bool is_open() const {
        return open_;
    }
    int list() const {
        return list_;
    }
    std::size_t first() const {
        return first_;
    }
    const std::vector<RoomEntry>& rooms() const {
        return rooms_;
    }

private:
    static bool Inside(const Key& k, int x, int y) {
        return x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h;
    }
    bool open_ = false;
    bool was_down_ = false;
    int list_ = 1;
    std::size_t first_ = 0;
    std::size_t picked_ = static_cast<std::size_t>(-1);
    std::vector<RoomEntry> rooms_;
};

/// Games whose memory is so tight that even the overlay's few KB are not worth the risk: the overlay does not start there. Matched on the low
/// half of the title id (every region of a game has its own). Monster Hunter XX (Rabbit_R) crashed with the plugin loaded (data abort writing 0xC000, a null allocation); add more title ids here.
inline bool OverlayDisabledFor(std::uint64_t title_id) {
    static const std::uint32_t kLow[] = {0x00197100u};
    for (std::uint32_t v : kLow) {
        if (static_cast<std::uint32_t>(title_id & 0xFFFFFFFFu) == v) {
            return true;
        }
    }
    return false;
}

} // namespace azl::chat
