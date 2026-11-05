// A lazily built object that is never destroyed, safe to ask for from any thread, WITHOUT a function-local static's guard.
//
// Why not `static T* p = new T();`: that is a thread-safe static. When a second thread asks while the first is still constructing,
// the first one's __cxa_guard_release has to wake the waiter with a condition-variable broadcast; on the console that call fails,
// libstdc++ throws __concurrence_broadcast_error, the plugin has no exceptions, and the process dies in terminate -> abort -> svcBreak
// (Luma dumps of 2026-11-05: Plugin::Get() at launch, MHXX / MH4U / MH3U). Here both atomics are constant-initialized, so a
// `static LazyNew<T>` needs no guard at all, and the thread that loses the race sleeps until the winner is done.
//
// Not re-entrant: T's constructor must not ask for the same LazyNew (it would wait for itself).
#pragma once

#include <atomic>

namespace azl {

template <class T>
class LazyNew {
public:
    constexpr LazyNew() = default;
    LazyNew(const LazyNew&) = delete;
    LazyNew& operator=(const LazyNew&) = delete;

    /// `sleep` is called (with no arguments) between two looks while another thread builds the object: it should give up the
    /// processor for about a millisecond (svcSleepThread on the console, std::this_thread::sleep_for elsewhere).
    template <class Sleep>
    T& Get(Sleep sleep) {
        T* ready = ptr_.load(std::memory_order_acquire);
        if (ready) {
            return *ready;
        }
        if (building_.exchange(1, std::memory_order_acq_rel) == 0) {
            ready = new T();
            ptr_.store(ready, std::memory_order_release);
            return *ready;
        }
        while ((ready = ptr_.load(std::memory_order_acquire)) == nullptr) {
            sleep();
        }
        return *ready;
    }

    /// The object if it is already built, else nullptr (never builds, never waits).
    T* Peek() const {
        return ptr_.load(std::memory_order_acquire);
    }

private:
    std::atomic<T*> ptr_{nullptr};
    std::atomic<int> building_{0};
};

} // namespace azl
