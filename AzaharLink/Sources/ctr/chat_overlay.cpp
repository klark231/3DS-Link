#include "ctr/chat_overlay.hpp"

#ifdef AZL_CHAT_OVERLAY

#include <3ds.h> // hidSharedMem (the HID shared page the game reads its touch from)
#include <CTRPluginFramework.hpp>

#include <atomic>

#include "azl/chat.hpp"
#include "azl/platform.hpp"
#include "azl/room_client.hpp"

// NOT YET COMPILED against the framework in this tree: the calls below are the CTRPluginFramework API as the framework documents it
// (OSD::Run with a Screen callback, Screen::Draw / DrawRect, Touch::IsDown / GetPosition). If the compiler disagrees with one of them,
// only this file needs to change: the logic is in Includes/azl/chat.hpp and is tested on a PC (Tests/chat_test.cpp).
namespace chat_overlay {

namespace {

using namespace CTRPluginFramework;

azl::chat::Log g_log;
azl::chat::Keyboard g_keyboard;
azl::chat::TouchScrubber g_scrub;       // learns where the live touch sits in the HID shared page, and clears it there
std::atomic<bool> g_hide{false};        // true while the keyboard is open (or the CHAT button is being pressed): the game must not see the touch
std::atomic<bool> g_run{false};
azl::platform::Thread g_scrub_thread;
std::atomic<azl::RoomClient*> g_room{nullptr};
bool g_installed = false;

constexpr int kMargin = 4;

void DrawKey(const Screen& s, const azl::chat::Key& k, bool held, bool shift) {
    const Color face = held ? Color(70, 120, 220) : Color(40, 42, 50);
    s.DrawRect(k.x, k.y, k.w - 1, k.h - 1, face, true);
    char one[2] = {0, 0};
    const char* label = k.label;
    if (!label) {
        one[0] = shift ? k.shifted : k.normal;
        label = one;
    }
    const int len = static_cast<int>(std::char_traits<char>::length(label));
    s.Draw(label, k.x + (k.w - len * 6) / 2, k.y + (k.h - 10) / 2, Color::White, face);
}

// Called by the framework once per frame for each screen (the top one, the bottom one).
bool Frame(const Screen& screen) {
    const u64 now = azl::platform::NowMs();
    if (screen.IsTop) {
        azl::chat::Log::Line lines[azl::chat::kLines];
        const std::size_t n = g_log.Visible(now, lines);
        for (std::size_t i = 0; i < n; ++i) {
            screen.Draw(lines[i].text, kMargin, kMargin + static_cast<int>(i) * 12, Color::White, Color(0, 0, 0));
        }
        return true;
    }
    // bottom screen: the touch is read once per frame (here, where the bottom screen is drawn)
    const bool down = Touch::IsDown();
    const UIntVector pos = down ? Touch::GetPosition() : UIntVector(0, 0);
    if (down) {
        g_scrub.Learn(reinterpret_cast<const volatile std::uint32_t*>(hidSharedMem), pos.x, pos.y); // (no-op until it sees a position it recognises)
    }
    const auto event = g_keyboard.OnTouch(down, static_cast<int>(pos.x), static_cast<int>(pos.y), now);
    if (event == azl::chat::Keyboard::Event::Send) {
        const std::string message = g_keyboard.TakeMessage();
        azl::RoomClient* room = g_room.load(std::memory_order_acquire);
        if (room && !message.empty()) {
            room->SendChat(message);
            g_log.Push("me", message, now);
        }
    }
    const bool over_button = down && pos.x >= static_cast<u32>(azl::chat::kChatButton.x) &&
                             pos.x < static_cast<u32>(azl::chat::kChatButton.x + azl::chat::kChatButton.w) &&
                             pos.y >= static_cast<u32>(azl::chat::kChatButton.y) &&
                             pos.y < static_cast<u32>(azl::chat::kChatButton.y + azl::chat::kChatButton.h);
    g_hide.store(g_keyboard.is_open() || over_button, std::memory_order_release);
    if (!g_keyboard.is_open()) {
        DrawKey(screen, azl::chat::kChatButton, false, false);
        return true;
    }
    screen.DrawRect(0, azl::chat::kTextBarY, 320, 240 - azl::chat::kTextBarY, Color(15, 16, 20), true);
    screen.Draw(g_keyboard.Tail(50) + "_", kMargin, azl::chat::kTextBarY + 6, Color::White, Color(15, 16, 20));
    std::size_t count = 0;
    const azl::chat::Key* keys = azl::chat::Keys(&count);
    for (std::size_t i = 0; i < count; ++i) {
        DrawKey(screen, keys[i], g_keyboard.held() == &keys[i], g_keyboard.shift());
    }
    return true;
}

} // namespace

void Attach(azl::RoomClient& room) {
    room.SetChatCallback([](const std::string& nick, const std::string& message) {
        g_log.Push(nick, message, azl::platform::NowMs());
    });
    g_room.store(&room, std::memory_order_release);
    if (!g_installed) {
        g_installed = true;
        OSD::Run(Frame);
        // The game reads the touch straight from the HID shared page, so the only way to keep a tap on a key from reaching it is to clear it there.
        // The HID service rewrites the page about once per frame; clearing it every millisecond while the keyboard is open keeps the game's window to see it tiny.
        g_run.store(true, std::memory_order_release);
        g_scrub_thread.Start([] {
            while (g_run.load(std::memory_order_acquire)) {
                if (g_hide.load(std::memory_order_acquire)) {
                    g_scrub.Scrub(reinterpret_cast<volatile std::uint32_t*>(hidSharedMem));
                }
                azl::platform::SleepMs(1);
            }
        });
    }
}

void Detach() {
    g_room.store(nullptr, std::memory_order_release);
    g_hide.store(false, std::memory_order_release);
    g_run.store(false, std::memory_order_release);
    g_scrub_thread.Join();
}

} // namespace chat_overlay

#else // AZL_CHAT_OVERLAY

namespace chat_overlay {
void Attach(azl::RoomClient&) {}
void Detach() {}
} // namespace chat_overlay

#endif
