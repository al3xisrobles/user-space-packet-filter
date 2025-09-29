#pragma once
#include <atomic>
#include <memory>
#include <thread>

#include "common.h"
#include "spsc_ring.h"

class TradingEngine {
public:
    using Ring = SpscRing<Tick, 4096>;

    explicit TradingEngine(std::shared_ptr<Ring> ring);
    ~TradingEngine();

    // non-copyable
    TradingEngine(const TradingEngine&) = delete;
    TradingEngine& operator=(const TradingEngine&) = delete;

    // Start/stop the background consumer thread
    void start();
    void stop();

    // Optional: run inline (no thread) — useful for tests
    void run_once();   // drain available items once
    void run_loop();   // blocking loop (returns when stopped)

private:
    void thread_main();

    std::shared_ptr<Ring> ring_;
    std::atomic<bool>     running_{false};
    std::thread           worker_;
};

// Small portable “yield”
inline void engine_yield() {
#if defined(__unix__)
    ::sched_yield();
#else
    std::this_thread::yield();
#endif
}
