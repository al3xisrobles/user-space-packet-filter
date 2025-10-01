#include "bypass_io.h"
#include "common.h"
#include "packet_capture.h"
#include <cstdio>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <iostream>

// Minimal RX soak: prints pps/bps every second like pkt-gen main thread
int run_rx_benchmark(BypassConfig cfg, int seconds) {
    BypassIO io(cfg);
    if (!io.ok()) { std::fprintf(stderr, "Bypass init failed\n"); return 1; }

    uint64_t last_pkts = 0, last_bytes = 0;
    auto last = std::chrono::steady_clock::now();

    for (;;) {
        // Drain packets from RX rings
        io.rx_batch([](const PacketView&){ return true; });

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count() >= 1) {
            const auto& s = io.stats();
            uint64_t dpkts = s.pkts - last_pkts;
            uint64_t dbytes = s.bytes - last_bytes;
            double pps = (double)dpkts;
            double bps = (double)dbytes * 8.0;
            std::printf("RX: %.0f pps  %.3f Gbps  drops=%llu  batches=%llu\n",
                        pps, bps/1e9, (unsigned long long)s.drops, (unsigned long long)s.batches);
            last_pkts = s.pkts; last_bytes = s.bytes; last = now;
            if (--seconds == 0) break;
        }
    }
    return 0;
}

/**
 * @brief Starts a background thread that prints delta pps/gbps every second and
 * prints a final summary when it exits. Caller may join the returned std::thread.
 *
 * @param cap PacketCapture instance to query stats from
 * @param global_running External atomic<bool> flag to control thread lifetime
 * @param end_time Time point to stop reporting (pass max() if not timed)
 * @return std::thread running the reporter (caller must join)
 */
std::thread start_stats_reporter(PacketCapture& cap,
                                 std::atomic<bool>& global_running,
                                 std::chrono::steady_clock::time_point end_time)
{
    return std::thread([&cap, &global_running, end_time]() {
        uint64_t last_pkts = 0;
        uint64_t last_bytes = 0;

        auto now = std::chrono::steady_clock::now();
        // Loop until global flag is false or end_time is reached.
        while (global_running && now < end_time) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            now = std::chrono::steady_clock::now();

            const auto& s = cap.stats();
            uint64_t dpkts = s.pkts - last_pkts;
            uint64_t dbytes = s.bytes - last_bytes;
            last_pkts = s.pkts;
            last_bytes = s.bytes;

            std::printf("RX: %llu pkts  %llu bytes  drops=%llu  +%llu pps  +%.3f Gbps\n",
                        (unsigned long long)s.pkts,
                        (unsigned long long)s.bytes,
                        (unsigned long long)s.drops,
                        (unsigned long long)dpkts,
                        (double)dbytes*8.0/1e9);
            std::fflush(stdout);
        }

        // final print (deltas since last printed second)
        const auto& s = cap.stats();
        uint64_t dpkts = s.pkts - last_pkts;
        uint64_t dbytes = s.bytes - last_bytes;
        std::printf("[final] RX: %llu pkts  %llu bytes  drops=%llu  +%llu pps  +%.3f Gbps\n",
                    (unsigned long long)s.pkts,
                    (unsigned long long)s.bytes,
                    (unsigned long long)s.drops,
                    (unsigned long long)dpkts,
                    (double)dbytes*8.0/1e9);
        std::fflush(stdout);
    });
}
