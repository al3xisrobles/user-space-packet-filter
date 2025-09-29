#pragma once
#include "spsc_ring.h"
#include <cstdint>
#include <cstddef>

// Build-time feature toggles
// Define USE_NETMAP to enable kernel bypass. Otherwise raw sockets/pcap stubs.
#ifndef BATCH_SIZE
#define BATCH_SIZE 128
#endif

struct Tick {
    uint64_t ts_ns;    // timestamp (or TSC)
    uint32_t instr_id; // mapping to instrument
    uint8_t  side;     // 0=bid,1=ask
    float    px;
    float    qty;
    // ~24 bytes; keep small
};

// Simple result type
struct Stats {
    uint64_t pkts{0};
    uint64_t bytes{0};
    uint64_t drops{0};
    uint64_t batches{0};
};

// Ingress sample handed to strategy
struct PacketView {
    const uint8_t* data{nullptr};
    uint16_t len{0};
    uint64_t tsc{0};
};

// Pin a thread to a CPU core (best effort)
void pin_thread_to_core(int core);

// rdtsc helper
inline uint64_t rdtsc() {
#if defined(__x86_64__)
    unsigned hi, lo;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
#else
    return 0;
#endif
}
