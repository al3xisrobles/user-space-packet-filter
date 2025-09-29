#include "common.h"
#include "trading_engine.h"
#include "packet_capture.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

static void usage(const char* prog){
    std::fprintf(stderr,
      "Usage: %s -i netmap:ethX [-p udp_port] [-c core] [-b burst] [-r seconds]\n", prog);
}

// Global stop flag set by SIGINT
static std::atomic<bool> g_running{true};

static void on_sigint(int) {
    g_running = false;
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

    for (int i=1;i<argc;i++){
        if (!std::strcmp(argv[i], "-i") && i+1<argc) io.ifname = argv[++i];
        else if (!std::strcmp(argv[i], "-p") && i+1<argc) fc.udp_port = (uint16_t)std::stoi(argv[++i]);
        else if (!std::strcmp(argv[i], "-c") && i+1<argc) io.cpu_affinity = std::stoi(argv[++i]);
        else if (!std::strcmp(argv[i], "-b") && i+1<argc) io.burst = std::stoi(argv[++i]);
        else if (!std::strcmp(argv[i], "-r") && i+1<argc) run_seconds = std::stoi(argv[++i]);
        else { usage(argv[0]); return 2; }
    }

    auto ring = std::make_shared<SpscRing<Tick, 4096>>();

    PacketCapture cap(io, fc);
    // quick sanity: single pump with no-op
    if (!cap.pump([](const PacketView&){ return true; }) && !cap.stats().pkts) {
        std::fprintf(stderr, "Failed to start capture (is the interface correct?)\n");
        return 1;
    }

    TradingEngine engine{ring};
    engine.start();

    // Stats printer (delta pps/gbps)
    uint64_t last_pkts=0, last_bytes=0;
    auto print_once = [&](bool final){
        const auto& s = cap.stats();
        uint64_t dpkts = s.pkts - last_pkts;
        uint64_t dbytes = s.bytes - last_bytes;
        last_pkts = s.pkts; last_bytes = s.bytes;
        std::printf("%sRX: %llu pkts  %llu bytes  drops=%llu  +%llu pps  +%.3f Gbps\n",
            final ? "[final] " : "",
            (unsigned long long)s.pkts, (unsigned long long)s.bytes,
            (unsigned long long)s.drops, (unsigned long long)dpkts, (double)dbytes*8.0/1e9);
        std::fflush(stdout);
    };

    const bool timed = run_seconds > 0;
    auto end = timed ? (std::chrono::steady_clock::now() + std::chrono::seconds(run_seconds))
                     : std::chrono::time_point<std::chrono::steady_clock>::max();

    // Start background capture owned by PacketCapture
    cap.start(ring, &g_running, end, io.cpu_affinity);

    // Background thread for logging
    std::thread reporter([&]{
        while (g_running && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
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
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // Stop threads
    reporter.join();
    cap.stop();       // joins producer thread
    engine.stop();    // joins consumer thread

    print_once(true);
    return 0;
#endif
}
