#pragma once
#include "bypass_io.h"
#include "packet_filter.h"
#include "spsc_ring.h"
#include "common.h"
#include <functional>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>

class PacketCapture {
public:
    using Ring = SpscRing<Tick, 4096>;

    PacketCapture(const BypassConfig& io_cfg, const FilterConfig& f_cfg);

    int pump(const std::function<bool(const PacketView&)>& cb);

    // Background capture: runs a producer thread that pushes Tick into ring
    // - running_flag: external stop flag (e.g., your g_running)
    // - end: stop time (pass max() if not timed)
    // - cpu_affinity: core to pin the capture thread (-1 = no pin)
    void start(std::shared_ptr<Ring> ring,
               std::atomic<bool>* running_flag,
               std::chrono::time_point<std::chrono::steady_clock> end,
               int cpu_affinity = -1);

    void stop();
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    const Stats& stats() const { return stats_; }

private:
    void thread_main(std::shared_ptr<Ring> ring,
                     std::atomic<bool>* running_flag,
                     std::chrono::time_point<std::chrono::steady_clock> end,
                     int cpu_affinity);

    BypassIO io_;
    PacketFilter filter_;
    Stats stats_{};

    std::atomic<bool> running_{false};
    std::thread worker_;
};
