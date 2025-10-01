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

    TradingEngine(const TradingEngine&) = delete;
    TradingEngine& operator=(const TradingEngine&) = delete;

    void start();
    void stop();

    void run_once();
    void run_loop();

private:
    void thread_main();

    std::shared_ptr<Ring> ring_;
    std::atomic<bool>     running_{false};
    std::thread           worker_;
};

inline void engine_yield() {
#if defined(__unix__)
    ::sched_yield();
#else
    std::this_thread::yield();
#endif
}
