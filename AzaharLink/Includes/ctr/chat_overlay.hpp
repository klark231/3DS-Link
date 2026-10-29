// In-game chat overlay (CTRPF): incoming lines on the top screen, a CHAT button on the bottom screen that opens a touch keyboard drawn over
// the game, and a ROOMS button that opens a list of saved rooms (servers.txt, servers2.txt ... in /3ds/3DSLink) to switch to. The game is never paused: the framework calls our drawing function once per frame, and the touch is read from the same
// shared page the game uses. Built only with -DAZL_CHAT_OVERLAY (see Sources/ctr/chat_overlay.cpp); without it Attach/Detach do nothing.
#pragma once

#include <string>

namespace azl {
class RoomClient;
}

namespace chat_overlay {

/// What the ROOMS picker calls when a room is chosen (a plain function: no allocation). The strings are valid only during the call.
using SwitchFn = void (*)(const char* host, unsigned short port, const char* password, const char* name);
/// Call once, before Attach. Without a handler a tap on a room only shows "Joining ..." in the chat lines.
void SetSwitchHandler(SwitchFn fn);

/// What the picker calls to get the text of room list `list` (1..azl::chat::kRoomLists); false when there is no such file.
using ReadListFn = bool (*)(int list, std::string& text);
void SetListReader(ReadListFn fn);

/// Starts showing the chat of `room` and lets the keyboard send into it. Call after the room client exists, before Start().
void Attach(azl::RoomClient& room);
/// Stops sending (the room client is going away); the overlay keeps drawing the lines already received until they expire.
void Detach();

} // namespace chat_overlay
