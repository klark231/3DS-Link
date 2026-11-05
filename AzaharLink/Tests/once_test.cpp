// Built and run by Tests/run.sh (with the thread sanitizer): azl/once.hpp builds its object once, whoever asks first, and the others wait for it.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <type_traits>
#include <vector>

#include "azl/once.hpp"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static std::atomic<int> g_built{0};
struct Slow {
    int value = 0;
    Slow() {
        ++g_built;
        std::this_thread::sleep_for(std::chrono::milliseconds(40)); // a constructor that lasts: the others arrive while it runs
        value = 42;
    }
};

static azl::LazyNew<Slow>& Box() {
    static azl::LazyNew<Slow> box; // constant-initialized: no guard
    return box;
}

int main() {
    static_assert(std::is_trivially_destructible<azl::LazyNew<Slow>>::value, "never destroyed, nothing to run at exit");
    CHECK(Box().Peek() == nullptr);
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 12; ++i) {
        threads.emplace_back([&] {
            const Slow& s = Box().Get([] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
            if (s.value != 42) {
                ++wrong; // a thread must never see the object before its constructor is done
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    CHECK(g_built.load() == 1); // built exactly once
    CHECK(wrong.load() == 0);
    CHECK(Box().Peek() != nullptr && Box().Peek()->value == 42);
    CHECK(&Box().Get([] {}) == Box().Peek()); // later calls return the same object, without waiting
    std::printf(fails ? "once_test: %d FAILED\n" : "once_test: ALL OK\n", fails);
    return fails ? 1 : 0;
}
