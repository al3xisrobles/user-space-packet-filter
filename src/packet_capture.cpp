#include "packet_capture.h"
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "common.h"

// forward declaration; implemented in bypass_io.cpp
void pin_thread_to_core(int core);

inline bool locate_udp_payload_14(const uint8_t* p, uint16_t len, const uint8_t*& out) {
    if (!p || len < 14 + 20 + 8) return false;
    const uint16_t etype = (uint16_t(p[12]) << 8) | uint16_t(p[13]);
    if (etype != 0x0800) return false;  // IPv4

    const uint8_t* ip = p + 14;
    const uint8_t ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || len < 14 + ihl + 8) return false;
    if (ip[9] != 17) return false;  // UDP

    const uint8_t* udp = ip + ihl;
    const uint16_t ulen = (uint16_t(udp[4]) << 8) | uint16_t(udp[5]);
    if (ulen < 8) return false;
    const uint16_t paylen = ulen - 8;
    if (paylen != 14) return false;

    const uint8_t* payload = udp + 8;
    if ((payload + 14) > (p + len)) return false;
    out = payload;
    return true;
}

inline bool decode_tick_from_packet(const uint8_t* p, uint16_t len, uint64_t tsc,
    Tick& out) {
    const uint8_t* payload = nullptr;
    if (!locate_udp_payload_14(p, len, payload)) return false;

    uint32_t instr_id = 0;
    uint8_t instr_type = 0;
    uint8_t side = 0;
    float px = 0.f, qty = 0.f;

    std::memcpy(&instr_id, payload + 0, 4);
    std::memcpy(&instr_type, payload + 4, 1);
    std::memcpy(&side, payload + 5, 1);
    std::memcpy(&px, payload + 6, 4);
    std::memcpy(&qty, payload + 10, 4);

    out.ts_ns = tsc;
    out.instr_id = instr_id;
    out.instr_type = instr_type;  // <-- now set
    out.side = side;  // <-- now set
    out.px = px;
    out.qty = qty;
    return true;
}

// Local debug helpers (opt-in via USPF_DEBUG=1)
static bool debug_enabled() {
    static bool enabled = std::getenv("USPF_DEBUG") != nullptr;
    return enabled;
}

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
    std::fprintf(stderr, "[%02d:%02d:%02d.%03d] [cap ] ", tm.tm_hour, tm.tm_min,
        tm.tm_sec, (int)ms.count());
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

PacketCapture::PacketCapture(const BypassConfig& io_cfg, const FilterConfig& f_cfg)
    : io_(io_cfg), filter_(f_cfg) {
    if (debug_enabled()) {
        log_debug("ctor: ifname=%s burst=%d cpu_affinity=%d udp_port=%u",
            io_cfg.ifname.c_str(), io_cfg.burst, io_cfg.cpu_affinity,
            (unsigned)f_cfg.udp_port);
    }
}

int PacketCapture::pump(const std::function<bool(const PacketView&)>& cb) {
    if (!io_.ok()) {
        if (debug_enabled()) log_debug("pump: io_.ok() == false (device not open/ready)");
        return -1;
    }

    // Per-pump local counters for visibility
    uint64_t accepted = 0;
    uint64_t filtered = 0;

    // accepted_cb wraps filtering so that rejection does not stop draining.
    // Return value contract:
    //   - return true  => keep draining the ring
    //   - return false => request early stop (fatal/budget/shutdown)
    auto accepted_cb = [&](const PacketView& v) -> bool {
        if (filter_.accept(v.data, v.len)) {
            ++accepted;
            // cb(v) may push to the downstream SPSC ring.
            // cb(v)==false means: "stop draining RX now because something went wrong"
            if (!cb(v)) return false;
        } else {
            ++filtered;
            ++stats_.drops;
        }
        return true;
    };

    int got = io_.rx_batch(accepted_cb);

    // aggregate stats from IO
    auto ios = io_.stats();
    stats_.pkts = ios.pkts;
    stats_.bytes = ios.bytes;

    if (debug_enabled()) {
        if (got < 0) {
            log_debug("pump: rx_batch returned %d (error). io_stats: pkts=%" PRIu64
                      " bytes=%" PRIu64 " drops=%" PRIu64,
                got, ios.pkts, ios.bytes, ios.drops);
        } else {
            if (got > 0) {
                log_debug("pump: rx_batch got=%d, accepted=%" PRIu64 ", filtered=%" PRIu64
                          ", io_drops=%" PRIu64 ", agg_pkts=%" PRIu64
                          ", agg_bytes=%" PRIu64,
                    got, accepted, filtered, ios.drops, stats_.pkts, stats_.bytes);
            }
        }
    }
    return got;
}

void PacketCapture::start(std::shared_ptr<Ring> ring, std::atomic<bool>* running_flag,
    std::chrono::time_point<std::chrono::steady_clock> end, int cpu_affinity) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    if (debug_enabled()) {
        log_debug(
            "start: launching producer thread (affinity=%d, until steady_clock=%lld)",
            cpu_affinity,
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                end.time_since_epoch())
                .count());
    }

    worker_ = std::thread(&PacketCapture::thread_main, this, std::move(ring),
        running_flag, end, cpu_affinity);
}

void PacketCapture::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    if (debug_enabled()) log_debug("stop: joining producer thread...");
    if (worker_.joinable()) worker_.join();
    if (debug_enabled()) {
        auto ios = io_.stats();
        log_debug("stop: final io_stats pkts=%" PRIu64 " bytes=%" PRIu64
                  " drops=%" PRIu64,
            ios.pkts, ios.bytes, ios.drops);
    }
}

// Producer
void PacketCapture::thread_main(std::shared_ptr<Ring> ring,
    std::atomic<bool>* running_flag,
    std::chrono::time_point<std::chrono::steady_clock> end, int cpu_affinity) {
    // Pin thread to a core close to the NIC NUMA node
    if (cpu_affinity >= 0) {
        if (debug_enabled()) log_debug("thread_main: pinning to core %d", cpu_affinity);
        pin_thread_to_core(cpu_affinity);
    }

    // Counters to explain *why* we might not be passing ticks downstream
    uint64_t ring_backpressure = 0;
    uint64_t ticks_pushed = 0;

    // Fast path callback: PacketView -> Tick -> push to SPSC
    auto to_tick_and_push = [&](const PacketView& v) -> bool {
        Tick t{};
        if (!decode_tick_from_packet(v.data, v.len, v.tsc, t)) return true;
        if (!ring->push(t))
            ++ring_backpressure;
        else
            ++ticks_pushed;
        return true;
    };
    // Main capture loop
    auto last_report = std::chrono::steady_clock::now();
    while (running_.load(std::memory_order_relaxed) && running_flag &&
        running_flag->load(std::memory_order_relaxed) &&
        std::chrono::steady_clock::now() < end) {
        int got = pump(to_tick_and_push);

        // Periodic debug summary (once per ~500ms)
        if (debug_enabled()) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::milliseconds(2000)) {
                auto ios = io_.stats();
                log_debug("loop: got=%d | pushed=%" PRIu64 " backpressure=%" PRIu64
                          " | io_pkts=%" PRIu64 " io_bytes=%" PRIu64 " io_drops=%" PRIu64,
                    got, ticks_pushed, ring_backpressure, ios.pkts, ios.bytes, ios.drops);
                if (ring_backpressure > 0) {
                    log_debug(
                        "loop: ring backpressure observed. Consider increasing ring size "
                        "or speeding up consumer.");
                }
                if (ios.pkts > 0 && ticks_pushed == 0) {
                    log_debug(
                        "loop: receiving packets but producing zero ticks. Likely filter "
                        "mismatch or decode errors.");
                }
                last_report = now;
            }
        }
        // Busy-poll by design
    }

    // Ensure final stats snapshot
    auto ios = io_.stats();
    stats_.pkts = ios.pkts;
    stats_.bytes = ios.bytes;

    if (debug_enabled()) {
        log_debug("thread_main: exit summary: pushed=%" PRIu64 ", backpressure=%" PRIu64
                  ", final_pkts=%" PRIu64 ", final_bytes=%" PRIu64,
            ticks_pushed, ring_backpressure, stats_.pkts, stats_.bytes);
    }
}
