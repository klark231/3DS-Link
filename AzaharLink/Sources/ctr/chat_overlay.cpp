#include "ctr/chat_overlay.hpp"

#ifdef AZL_CHAT_OVERLAY

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
    const auto event = g_keyboard.OnTouch(down, static_cast<int>(pos.x), static_cast<int>(pos.y), now);
    if (event == azl::chat::Keyboard::Event::Send) {
        const std::string message = g_keyboard.TakeMessage();
        azl::RoomClient* room = g_room.load(std::memory_order_acquire);
        if (room && !message.empty()) {
            room->SendChat(message);
            g_log.Push("me", message, now);
        }
    }
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
    }
}

void Detach() {
    g_room.store(nullptr, std::memory_order_release);
}

} // namespace chat_overlay

#else // AZL_CHAT_OVERLAY

namespace chat_overlay {
void Attach(azl::RoomClient&) {}
void Detach() {}
} // namespace chat_overlay

#endif
