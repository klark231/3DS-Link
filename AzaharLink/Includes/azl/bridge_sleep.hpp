// Sleep for the MHGU bridge threads. On the console a std::condition_variable timed wait / std::this_thread::sleep_for can
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
