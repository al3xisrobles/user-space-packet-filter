#include "packet_capture.h"
#include "udp_md_codec.h"

// forward declaration; implemented in bypass_io.cpp
void pin_thread_to_core(int core);

PacketCapture::PacketCapture(const BypassConfig& io_cfg, const FilterConfig& f_cfg)
    : io_(io_cfg), filter_(f_cfg) {}

int PacketCapture::pump(const std::function<bool(const PacketView&)>& cb) {
    if (!io_.ok()) return -1;
    auto accepted_cb = [&](const PacketView& v) -> bool {
        if (filter_.accept(v.data, v.len)) {
            if (!cb(v)) return false;
        } else {
            ++stats_.drops;
        }
        return true;
    };
    int got = io_.rx_batch(accepted_cb);
    // aggregate stats
    auto ios = io_.stats();
    stats_.pkts  = ios.pkts;
    stats_.bytes = ios.bytes;
    return got;
}

void PacketCapture::start(std::shared_ptr<Ring> ring,
                          std::atomic<bool>* running_flag,
                          std::chrono::time_point<std::chrono::steady_clock> end,
                          int cpu_affinity) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    worker_ = std::thread(&PacketCapture::thread_main, this,
                          std::move(ring), running_flag, end, cpu_affinity);
}

void PacketCapture::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    if (worker_.joinable()) worker_.join();
}

void PacketCapture::thread_main(std::shared_ptr<Ring> ring,
                                std::atomic<bool>* running_flag,
                                std::chrono::time_point<std::chrono::steady_clock> end,
                                int cpu_affinity) {
    // Optional: pin thread to a core close to the NIC NUMA node
    if (cpu_affinity >= 0) pin_thread_to_core(cpu_affinity);

    // Fast path callback: PacketView -> Tick -> push to SPSC
    auto to_tick_and_push = [&](const PacketView& v) -> bool {
        const uint8_t* pl = nullptr;
        uint16_t pl_len = 0;
        if (!parse_eth_ipv4_udp(v.data, v.len, pl, pl_len)) {
            // Not UDP/IPv4 market data; drop quietly.
            return true;
        }

        uint32_t instr_id = 0;
        uint8_t  instr_type = 0;
        uint8_t  side = 0;
        float    px = 0.f, qty = 0.f;

        if (!decode_md_payload(pl, pl_len, instr_id, instr_type, side, px, qty)) {
            // Malformed payload; drop.
            return true;
        }

        Tick t{};
        t.ts_ns    = v.tsc;          // you’re filling TSC in rx_batch already
        t.instr_id = instr_type;     // map directly: 0=UNDERLYING,1=OPTION,2=FUTURE
        t.side     = side;           // optional for your prints
        t.px       = px;
        t.qty      = qty;

        if (!ring->push(t)) {
            ++stats_.drops; // backpressure
        }
        return true;
    };

    // Main capture loop
    while (running_.load(std::memory_order_relaxed) &&
           running_flag && running_flag->load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < end)
    {
        pump(to_tick_and_push);
        // Busy-poll by design; if you want NIC-driven poll(), set io_.cfg to disable busy_poll
    }

    // Ensure final stats snapshot
    auto ios = io_.stats();
    stats_.pkts  = ios.pkts;
    stats_.bytes = ios.bytes;
}
