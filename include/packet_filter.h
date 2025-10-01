#pragma once
#include <cstdint>
#include <vector>
#include "common.h"

struct FilterConfig {
    uint16_t udp_port = 5001;  // 0 = accept any
    uint32_t dst_ip = 0;  // 0 = any
    bool require_udp = true;
    bool require_ipv4 = true;
};

class PacketFilter {
   public:
    explicit PacketFilter(const FilterConfig& cfg) : cfg_(cfg) {}
    // Returns true if packet should be kept
    bool accept(const uint8_t* p, uint16_t len) const;

   private:
    FilterConfig cfg_;
};
