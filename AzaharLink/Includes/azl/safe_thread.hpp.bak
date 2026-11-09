// A std::thread that cannot kill the game: the thread of the MHGU hub bridge (Sources/mhgu, Sources/ctr/crossplay.cpp).
//
// Why not std::thread: the plugin has no exceptions. When pthread_create fails, libstdc++ throws std::system_error ("Enable
// multithreading to use std::thread" is the check next to it), there is nobody to catch it, and the process dies in
// terminate -> abort -> svcBreak (Luma dumps of 2026-11-05, process Rabbit_R: abort <- __cxa_throw <- std::thread::_M_start_thread <-
// Bridge::Attach <- CrossplayBridge::Start <- Plugin::Connect). pthread_create takes libctru's one fixed choice of core and
// priority and gives up on the first refusal; the plugin's own threads (platform::Thread) try the wanted core, then any,
// then 0, then 1, because "some configurations refuse some cores". A SafeThread does the same, and when nothing works it is
// simply not joinable() (the caller decides: run the work inline, report the failure, ...), it never throws or aborts.
//
// The subset of std::thread the bridge uses: construct from one callable, joinable(), join(), detach(), move.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

#ifdef __3DS__
#include "azl/platform.hpp"
#endif

namespace azl {

#ifdef __3DS__

namespace safe_thread_detail {

constexpr int kDetachedSlots = 8;

// Threads that were detach()ed keep running on their own stack: they are parked here and freed once they have finished
// (the next detach() looks). A plain zero-initialized array (no constructor, so no guard, see azl/once.hpp) touched only with
// the compiler's atomic builtins.
inline platform::Thread** DetachedSlots() {
    static platform::Thread* slots[kDetachedSlots];
    return slots;
}

inline void Park(std::unique_ptr<platform::Thread> thread) {
    platform::Thread** slots = DetachedSlots();
    for (int i = 0; i < kDetachedSlots; i++) {
        platform::Thread* p = __atomic_load_n(&slots[i], __ATOMIC_ACQUIRE);
        if (p && !p->running() &&
            __atomic_compare_exchange_n(&slots[i], &p, static_cast<platform::Thread*>(nullptr), false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            p->Join(); // finished: this only gives back its stack
            delete p;
        }
    }
    platform::Thread* mine = thread.release();
    for (int i = 0; i < kDetachedSlots; i++) {
        platform::Thread* expected = nullptr;
        if (__atomic_compare_exchange_n(&slots[i], &expected, mine, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return;
        }
    }
    // No free slot (eight detached threads alive at once): its stack stays allocated, on purpose, the thread may still run.
}

} // namespace safe_thread_detail

class SafeThread {
public:
    SafeThread() = default;

    template <class F, class = std::enable_if_t<!std::is_same<std::decay_t<F>, SafeThread>::value>>
    explicit SafeThread(F&& fn) {
        std::unique_ptr<platform::Thread> t(new platform::Thread());
        if (t->Start(std::function<void()>(std::forward<F>(fn)))) { // (logs each refused core itself)
            thread_ = std::move(t);
        }
    }

    SafeThread(SafeThread&& other) noexcept : thread_(std::move(other.thread_)) {}
    SafeThread& operator=(SafeThread&& other) noexcept {
        if (this != &other) {
            join();
            thread_ = std::move(other.thread_);
        }
        return *this;
    }
    SafeThread(const SafeThread&) = delete;
    SafeThread& operator=(const SafeThread&) = delete;
    ~SafeThread() {
        join(); // (a std::thread would terminate here; every owner of the bridge joins first, so this is only a net)
    }

    bool joinable() const {
        return thread_ != nullptr;
    }
    void join() {
        if (thread_) {
            thread_->Join();
            thread_.reset();
        }
    }
    void detach() {
        if (thread_) {
            safe_thread_detail::Park(std::move(thread_));
        }
    }

private:
    std::unique_ptr<platform::Thread> thread_;
};

#else // PC (the unit tests): a std::thread

class SafeThread {
public:
    SafeThread() = default;

    template <class F, class = std::enable_if_t<!std::is_same<std::decay_t<F>, SafeThread>::value>>
    explicit SafeThread(F&& fn) : thread_(std::forward<F>(fn)) {}

    SafeThread(SafeThread&&) noexcept = default;
    SafeThread& operator=(SafeThread&& other) noexcept {
        if (this != &other) {
            join();
            thread_ = std::move(other.thread_);
        }
        return *this;
    }
    ~SafeThread() {
        join();
    }

    bool joinable() const {
        return thread_.joinable();
    }
    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    void detach() {
        thread_.detach();
    }

private:
    std::thread thread_;
};

#endif

} // namespace azl
