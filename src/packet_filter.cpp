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

bool PacketFilter::accept(const uint8_t* p, uint16_t len) const {
    // Fast-drop: minimum Ethernet+IPv4
    // Print out that we received a packet
    std::cout << "Received packet of length: " << len << " bytes" << std::endl;

    if (unlikely(len < 14)) return false;

    // EtherType at offset 12
    if (cfg_.require_ipv4) {
        uint16_t ethertype = (uint16_t(p[12]) << 8) | uint16_t(p[13]);
        if (ethertype != 0x0800) return false;  // IPv4 only
        if (unlikely(len < 14 + 20)) return false;

        const uint8_t* ip = p + 14;
        uint8_t ihl = (ip[0] & 0x0F) * 4;
        if (unlikely(ihl < 20 || len < 14 + ihl)) return false;

        if (cfg_.require_udp) {
            if (ip[9] != 17) return false;  // UDP protocol
            if (cfg_.udp_port) {
                if (unlikely(len < 14 + ihl + 8)) return false;
                const uint8_t* udp = ip + ihl;
                uint16_t dport = (uint16_t(udp[2]) << 8) | uint16_t(udp[3]);
                if (dport != cfg_.udp_port) return false;
            }
        }

        if (cfg_.dst_ip) {
            uint32_t dst;
            std::memcpy(&dst, ip + 16, 4);  // network-order bytes
            if (dst != cfg_.dst_ip) return false;
        }
    }
    return true;
}
