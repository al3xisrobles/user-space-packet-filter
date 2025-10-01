#include "packet_filter.h"
#include <arpa/inet.h>
#include <cstring>  // for std::memcpy
#include <iostream>
#include <thread>  // for std::this_thread::yield
#include "common.h"
#include "spsc_ring.h"

#ifdef __unix__
#endif

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

static inline void cpu_relax() {
#ifdef __unix__
    sched_yield();
#else
    std::this_thread::yield();
#endif
}

/**
 * @brief Fast-path predicate to accept/drop a packet based on L2/L3/L4 rules
 *        and a fixed 14-byte UDP payload shape.
 *
 *  - Validates Ethernet type (IPv4), IPv4 header length/bounds, and UDP protocol.
 *  - Checks destination UDP port if configured (cfg_.udp_port).
 *  - Verifies UDP length and frame bounds, requiring exactly 14 bytes of payload.
 *  - Performs a cheap shape check of the payload by memcpy’ing fields into
 *    temporaries (u32 instr_id, u8 instr_type, u8 side, f32 px, f32 qty)
 *    without mutating any external state.
 *
 * Performance characteristics:
 *  - Branching is laid out to favor the likely fast path (IPv4/UDP).
 *  - Bounds checks ensure we never read beyond `p + len`.
 *  - The payload field memcpy here is intentionally minimal and does not
 *    allocate or write to external state.
 *
 * TODO: accept(), decode_tick_from_packet(), and locate_udp_payload_14()
 * duplicate parsing logic. Consider unifying validation+decode to avoid double
 * memcpy and bounds checks.
 *
 * @param p   Pointer to the start of the Ethernet frame.
 * @param len Total frame length in bytes.
 * @return true if the packet matches the configured filters and has a valid
 *               14-byte UDP payload.
 * @return false if the packet is non-IPv4/UDP, wrong port (if set), malformed,
 *               too short, or does not carry a 14-byte payload.
 */
bool PacketFilter::accept(const uint8_t* p, uint16_t len) const {
    if (unlikely(len < 14)) return false;

    // L2: Ethernet
    const uint16_t etype = (uint16_t(p[12]) << 8) | uint16_t(p[13]);
    if (cfg_.require_ipv4) {
        if (etype != 0x0800) return false;  // IPv4
        if (unlikely(len < 14 + 20)) return false;

        const uint8_t* ip = p + 14;
        const uint8_t ihl_bytes = (ip[0] & 0x0F) * 4;
        if (unlikely(ihl_bytes < 20 || len < 14 + ihl_bytes)) return false;

        if (cfg_.require_udp) {
            if (ip[9] != 17) return false;  // UDP
            if (unlikely(len < 14 + ihl_bytes + 8)) return false;

            const uint8_t* udp = ip + ihl_bytes;
            const uint16_t dport = (uint16_t(udp[2]) << 8) | uint16_t(udp[3]);
            if (cfg_.udp_port && dport != cfg_.udp_port) return false;

            // UDP length and payload
            const uint16_t ulen = (uint16_t(udp[4]) << 8) | uint16_t(udp[5]);
            if (ulen < 8) return false;
            const uint16_t payload_len = ulen - 8;

            // We expect exactly 14 bytes
            if (payload_len != 14) return false;

            // Bounds check against the whole frame length
            const size_t udp_off = 14 + ihl_bytes;
            const size_t payload_off = udp_off + 8;
            if (payload_off + payload_len > len) return false;

            const uint8_t* payload = p + payload_off;

            // Cheap shape check without mutating user state
            uint32_t instr_id = 0;
            uint8_t instr_type = 0, side = 0;
            float px = 0.f, qty = 0.f;
            std::memcpy(&instr_id, payload + 0, 4);
            std::memcpy(&instr_type, payload + 4, 1);
            std::memcpy(&side, payload + 5, 1);
            std::memcpy(&px, payload + 6, 4);
            std::memcpy(&qty, payload + 10, 4);
        }
    }
    return true;
}
