#pragma once
#include <functional>
#include <string>
#include "common.h"

struct BypassConfig {
    std::string ifname = "netmap:eth0";
    int rx_ring_first = -1;  // -1 = all
    int rx_ring_last = -1;
    int tx_ring_first = -1;
    int tx_ring_last = -1;
    int burst = BATCH_SIZE;
    bool busy_poll = true;
    int cpu_affinity = -1;  // -1 = don't pin
};

class BypassIO {
   public:
    explicit BypassIO(const BypassConfig& cfg);
    ~BypassIO();

    // Receive up to cfg.burst packets; calls cb for each accepted packet.
    // Returns received count (not necessarily accepted).
    int rx_batch(const std::function<bool(const PacketView&)>& cb);

    // Transmit a buffer (optional for your filter pipeline).
    int tx(const uint8_t* data, uint16_t len);

    // Stats across life of this object
    const Stats& stats() const { return stats_; }

    // Valid after construction
    bool ok() const { return ok_; }

   private:
    BypassConfig cfg_;
    Stats stats_{};
    bool ok_{false};

    // netmap internals are hidden to keep user code clean
    struct Impl;
    Impl* impl_{nullptr};
};
