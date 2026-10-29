// In-game chat, the part that needs no console: the cleaning of the text, the lines kept for the overlay, and the on-screen touch
// keyboard as a state machine (where the keys are, what a tap does). Pure C++ (no 3ds.h, no CTRPF): Tests/chat_test.cpp runs it on a PC.
// The drawing and the touch reading are in Sources/ctr/chat_overlay.cpp (only built with -DAZL_CHAT_OVERLAY).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

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

} // namespace azl::chat
