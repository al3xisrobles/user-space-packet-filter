#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

#include "common.h"
#include "packet_capture.h"
#include "trading_engine.h"

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s -i netmap:ethX [-p udp_port] [-c core] [-b burst] [-r seconds]\n",
        prog);
}

// Global stop flag set by SIGINT
static std::atomic<bool> g_running{true};

static void on_sigint(int) {
    g_running = false;
}

// Debug helpers (opt-in via USPF_DEBUG=1)
static bool debug_enabled() {
    static bool enabled = std::getenv("USPF_DEBUG") != nullptr;
    return enabled;
}

// Log a debug message if USPF_DEBUG=1
static void log_debug(const char* fmt, ...) {
    if (!debug_enabled()) return;
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    auto t = clock::to_time_t(now);
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) %
        1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::fprintf(stderr, "[%02d:%02d:%02d.%03d] [main] ", tm.tm_hour, tm.tm_min,
        tm.tm_sec, (int)ms.count());
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

int main(int argc, char** argv) {
#ifndef USE_NETMAP
    std::fprintf(stderr, "Build with -DUSE_NETMAP and netmap user libs.\n");
    return 1;
#else
    std::signal(SIGINT, on_sigint);

    BypassConfig io{};
    FilterConfig fc{};
    int run_seconds = 0;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "-i") && i + 1 < argc)
            io.ifname = argv[++i];
        else if (!std::strcmp(argv[i], "-p") && i + 1 < argc)
            fc.udp_port = (uint16_t)std::stoi(argv[++i]);
        else if (!std::strcmp(argv[i], "-c") && i + 1 < argc)
            io.cpu_affinity = std::stoi(argv[++i]);
        else if (!std::strcmp(argv[i], "-b") && i + 1 < argc)
            io.burst = std::stoi(argv[++i]);
        else if (!std::strcmp(argv[i], "-r") && i + 1 < argc)
            run_seconds = std::stoi(argv[++i]);
        else {
            usage(argv[0]);
            return 2;
        }
    }

    if (debug_enabled()) {
        log_debug("Config:");
        log_debug("  ifname         = %s", io.ifname.c_str());
        log_debug("  udp_port       = %u", (unsigned)fc.udp_port);
        log_debug("  cpu_affinity   = %d", io.cpu_affinity);
        log_debug("  burst          = %d", io.burst);
        log_debug("  run_seconds    = %d", run_seconds);
    }

    // Create an SPSC ring for Ticks to be passed to TradingEngine
    auto ring = std::make_shared<SpscRing<Tick, 4096> >();

    PacketCapture cap(io, fc);

    // Sanityb check: ingle pump with no-op; print why if it fails or returns 0
    log_debug("Performing sanity pump...");
    int first_got = cap.pump([](const PacketView&) { return true; });
    const auto first_stats = cap.stats();
    log_debug(
        "Sanity pump result: got=%d pkts; agg_stats: pkts=%llu bytes=%llu drops=%llu",
        first_got, (unsigned long long)first_stats.pkts,
        (unsigned long long)first_stats.bytes, (unsigned long long)first_stats.drops);
    if (first_got < 0 && !first_stats.pkts) {
        std::fprintf(stderr,
            "Failed to start capture (is the interface correct and accessible?)\n");
        return 1;
    }

    // Start the trading engine consumer
    TradingEngine engine{ring};
    engine.start();

    // Stats printer (delta pps/gbps)
    uint64_t last_pkts = 0, last_bytes = 0;
    auto print_once = [&](bool final) {
        const auto& s = cap.stats();
        uint64_t dpkts = s.pkts - last_pkts;
        uint64_t dbytes = s.bytes - last_bytes;
        last_pkts = s.pkts;
        last_bytes = s.bytes;
        std::printf("%sRX: %llu pkts  %llu bytes  drops=%llu  +%llu pps  +%.3f Gbps\n",
            final ? "[final] " : "", (unsigned long long)s.pkts,
            (unsigned long long)s.bytes, (unsigned long long)s.drops,
            (unsigned long long)dpkts, (double)dbytes * 8.0 / 1e9);
        std::fflush(stdout);

        if (debug_enabled()) {
            if (dpkts == 0 && dbytes == 0) {
                log_debug(
                    "No traffic in last interval. If this persists, check link, "
                    "mirror/span, and NIC binding.");
            } else if (dpkts > 0 && dbytes == 0) {
                log_debug(
                    "Saw packets but zero bytes delta (unexpected) — verify stats "
                    "plumbing.");
            }
        }
    };

    const bool timed = run_seconds > 0;
    auto end = timed
        ? (std::chrono::steady_clock::now() + std::chrono::seconds(run_seconds))
        : std::chrono::time_point<std::chrono::steady_clock>::max();

    // Start background capture owned by PacketCapture
    log_debug("Starting PacketCapture background thread (affinity=%d)...",
        io.cpu_affinity);
    cap.start(ring, &g_running, end, io.cpu_affinity);

    // Background thread for logging
    std::thread reporter([&] {
        while (g_running && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            print_once(false);
        }
    });

    // Wait for end or Ctrl+C
    if (timed) {
        while (g_running && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        g_running = false;
    } else {
        while (g_running) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    }

    // Stop threads
    log_debug("Stopping PacketCapture and TradingEngine...");
    reporter.join();
    cap.stop();
    engine.stop();

    print_once(true);
    log_debug("Shutdown complete.");
    return 0;
#endif
}
