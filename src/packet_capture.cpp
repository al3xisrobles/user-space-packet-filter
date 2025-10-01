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

/**
 * @brief Locate UDP payload of exactly 14 bytes in a raw Ethernet frame.
 *
 * Assumes Ethernet + IPv4 + UDP. Validates lengths and boundaries.
 *
 * @param p pointer to the start of the Ethernet frame
 * @param len length of p
 * @param out output pointer to the start of the UDP payload (if found)
 * @return true if found and out is set
 * @return false if not found or invalid
 */
inline bool locate_udp_payload_14(const uint8_t* p, uint16_t len, const uint8_t*& out) {
    // L2: Ethernet
    if (!p || len < 14 + 20 + 8) return false;
    const uint16_t etype = (uint16_t(p[12]) << 8) | uint16_t(p[13]);
    if (etype != 0x0800) return false;

    // L3: IPv4 + UDP
    const uint8_t* ip = p + 14;
    const uint8_t ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || len < 14 + ihl + 8) return false;
    if (ip[9] != 17) return false;

    // L4: UDP + payload
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

/**
 * @brief Decode a Tick from a UDP payload of exactly 14 bytes.
 *
 * Expects the payload to be located and validated by locate_udp_payload_14().
 *
 * @param p pointer to the start of the Ethernet frame
 * @param len length of p
 * @param tsc timestamp to set in the Tick
 * @param out output Tick to populate
 * @return true if successful
 * @return false if not successful (e.g., payload not found)
 */
inline bool decode_tick_from_packet(const uint8_t* p, uint16_t len, uint64_t tsc,
    Tick& out) {
    const uint8_t* payload = nullptr;

    // If it's not a valid UDP payload of 14 bytes, return false
    if (!locate_udp_payload_14(p, len, payload)) return false;

    uint32_t instr_id = 0;
    uint8_t instr_type = 0;
    uint8_t side = 0;
    float px = 0.f, qty = 0.f;

    // We assume little-endian host; memcpy to avoid alignment issues
    std::memcpy(&instr_id, payload + 0, 4);
    std::memcpy(&instr_type, payload + 4, 1);
    std::memcpy(&side, payload + 5, 1);
    std::memcpy(&px, payload + 6, 4);
    std::memcpy(&qty, payload + 10, 4);
    out.ts_ns = tsc;
    out.instr_id = instr_id;
    out.instr_type = instr_type;
    out.side = side;
    out.px = px;
    out.qty = qty;
    return true;
}

// Local debug helpers (opt-in via USPF_DEBUG=1)
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
    std::fprintf(stderr, "[%02d:%02d:%02d.%03d] [cap ] ", tm.tm_hour, tm.tm_min,
        tm.tm_sec, (int)ms.count());
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

/**
 * @brief Construct a new Packet Capture:: Packet Capture object
 *
 * @param io_cfg config for BypassIO
 * @param f_cfg config for PacketFilter
 */
PacketCapture::PacketCapture(const BypassConfig& io_cfg, const FilterConfig& f_cfg)
    : io_(io_cfg), filter_(f_cfg) {
    if (debug_enabled()) {
        log_debug("ctor: ifname=%s burst=%d cpu_affinity=%d udp_port=%u",
            io_cfg.ifname.c_str(), io_cfg.burst, io_cfg.cpu_affinity,
            (unsigned)f_cfg.udp_port);
    }
}

/**
 * @brief Start background capture thread that pumps packets from NIC → filter → SPSC ring.
 *
 * @param ring Shared pointer to the SPSC ring to push decoded Ticks into
 * @param running_flag External atomic<bool> flag to control lifetime (may be nullptr)
 * @param end Time point to stop capturing (pass max() if not timed)
 * @param cpu_affinity Core to pin the capture thread (-1 = no pin)
 */
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

/**
 * @brief Drain packets from the RX ring, apply filter rules, and invoke a callback
 *        for each accepted packet.
 *
 * This method pulls a batch of packets from the underlying I/O layer (netmap).
 * Each packet is wrapped in a PacketView and passed through the configured
 * PacketFilter. If the filter accepts the packet, the user-supplied callback
 * (cb) is invoked. The callback pushes decoded packets into a downstream queue.
 *
 * Control flow:
 *  - If `filter_.accept()` returns false, the packet is counted as dropped
 *    (filtered) and not passed to the callback.
 *  - If the callback `cb(v)` returns false, draining stops early and pump()
 *    returns immediately. This allows downstream components to signal backpressure
 *    or early termination.
 *  - Otherwise, pump() continues draining until the RX ring is empty or the burst
 *    budget is consumed.
 *
 * Statistics:
 *  - Updates aggregate stats_ counters for total packets, bytes, and drops.
 *  - Per-call counters for accepted and filtered packets are maintained for debug logs.
 *
 * @param cb User-supplied function that processes each accepted PacketView.
 *           Should return true to continue, or false to stop draining immediately.
 * @return int
 *   - >0 : number of packets processed in this pump call
 *   -  0 : no packets available (idle poll)
 *   - <0 : error occurred in the I/O layer
 */
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

    // Drain a batch of packets from the RX ring, applying filtering and
    // invoking accepted_cb for each accepted packet.
    int got = io_.rx_batch(accepted_cb);

    // Aggregate stats from IO
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

/**
 * @brief Producer thread entry point: capture → filter/decode → enqueue.
 *
 * Runs the high-frequency RX loop on a dedicated core. The thread
 *  1) Optionally pins itself to @p cpu_affinity
 *  2) Builds a fast-path lambda `to_tick_and_push` that decodes a Tick from
 *     each accepted packet and pushes it into the SPSC ring (records
 *     backpressure if the push fails).
 *  3) Repeatedly calls pump(to_tick_and_push) until:
 *        - @p running_ becomes false (internal stop),
 *        - @p running_flag is unset by the owner (external stop), or
 *        - the current time reaches @p end (timed stop).
 *  4) Periodically emits debug telemetry (when USPF_DEBUG=1): packets obtained,
 *     ticks pushed, ring backpressure, and underlying I/O stats.
 *  5) On exit, snapshots final I/O counters into stats_ and logs a summary.
 *
 *  - This function is intended to be executed by the background producer thread
 *    created in start(). It is not thread-safe to call directly from user code.
 *  - The SPSC ring is single-producer; only this thread should push().
 *  - Debug logging is rate-limited but still on this thread; avoid enabling it
 *    for peak-throughput measurements.
 *
 * @param ring          Shared pointer to the SPSC ring used to enqueue decoded ticks.
 * @param running_flag  Optional external stop flag (owned by caller). If non-null and becomes false, the loop terminates.
 * @param end           Absolute steady_clock deadline. When reached, the loop exits.
 * @param cpu_affinity  Core index to pin this thread to (>=0 pins, <0 leaves default).
 */
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

        // Pump packets from NIC → filter → SPSC ring
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
