// In-game chat overlay (CTRPF): incoming lines on the top screen, a CHAT button on the bottom screen that opens a touch keyboard drawn over
// the game. The game is never paused: the framework calls our drawing function once per frame, and the touch is read from the same
// shared page the game uses. Built only with -DAZL_CHAT_OVERLAY (see Sources/ctr/chat_overlay.cpp); without it Attach/Detach do nothing.
#pragma once

namespace azl {
class RoomClient;
}

namespace chat_overlay {

/// Starts showing the chat of `room` and lets the keyboard send into it. Call after the room client exists, before Start().
void Attach(azl::RoomClient& room);
/// Stops sending (the room client is going away); the overlay keeps drawing the lines already received until they expire.
void Detach();

} // namespace chat_overlay
