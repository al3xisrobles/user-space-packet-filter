#pragma once
#include <cstddef>
#include <cstdint>
#include "spsc_ring.h"

// Build-time feature toggles
// Define USE_NETMAP to enable kernel bypass. Otherwise raw sockets/pcap stubs.
#ifndef BATCH_SIZE
#define BATCH_SIZE 128
#endif

struct Tick {
    // Timestamp (or TSC)
    uint64_t ts_ns;

    // Unique instrument identifier
    uint32_t instr_id;

    // 0=UNDERLYING, 1=OPTION, 2=FUTURE
    uint8_t instr_type;

    // 0=bid, 1=ask
    uint8_t side;

    // Price
    float px;

    // Quantity
    float qty;
};

struct Stats {
    uint64_t pkts{0};
    uint64_t bytes{0};
    uint64_t drops{0};
    uint64_t batches{0};
};

struct PacketView {
    const uint8_t* data{nullptr};
    uint16_t len{0};
    uint64_t tsc{0};
};

void pin_thread_to_core(int core);

inline uint64_t rdtsc() {
#if defined(__x86_64__)
    unsigned hi, lo;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
#else
    return 0;
#endif
}
