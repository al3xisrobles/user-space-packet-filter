#pragma once
#include <cstdint>
#include <cstring>

// instr_id   : uint32   (4 bytes)
// instr_type : uint8    (0=UNDERLYING, 1=OPTION, 2=FUTURE)
// side       : uint8    (0=bid, 1=ask)
// px         : float32  (4 bytes)
// qty        : float32  (4 bytes)
// ------------------------------------
// TOTAL = 14 bytes


// Parses Ethernet+IPv4+UDP headers, returns pointer/len to UDP payload.
// Returns false on any bounds error.
inline bool parse_eth_ipv4_udp(const uint8_t* p, uint16_t len,
                               const uint8_t*& payload, uint16_t& payload_len) {
    if (len < 14) return false; // Ethernet
    uint16_t ethertype = (uint16_t(p[12]) << 8) | p[13];
    if (ethertype != 0x0800) return false; // IPv4 only

    const uint8_t* ip = p + 14;
    if (len < 14 + 20) return false;
    uint8_t ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || len < 14 + ihl) return false;

    if (ip[9] != 17) return false; // UDP protocol

    const uint8_t* udp = ip + ihl;
    if (len < 14 + ihl + 8) return false;

    uint16_t udp_len = (uint16_t(udp[4]) << 8) | udp[5];
    // UDP length includes header (8)
    if (udp_len < 8) return false;

    const uint16_t off = 14 + ihl + 8;
    if (len < off) return false;

    payload = p + off;
    // payload_len cannot exceed frame len, use the smaller of the two
    uint16_t max_pl = len - off;
    payload_len = (udp_len > 8) ? (udp_len - 8) : 0;
    if (payload_len > max_pl) payload_len = max_pl;

    return true;
}

// Our test market-data payload: <u32 instr_id, u8 instr_type, u8 side, f32 px, f32 qty>
// Little-endian, packed to 14 bytes.
inline bool decode_md_payload(const uint8_t* pl, uint16_t pl_len,
                              uint32_t& instr_id, uint8_t& instr_type, uint8_t& side,
                              float& px, float& qty) {
    if (pl_len < 14) return false;
    // Use memcpy to avoid alignment/strict-aliasing issues
    std::memcpy(&instr_id, pl + 0, 4);
    std::memcpy(&instr_type, pl + 4, 1);
    std::memcpy(&side,       pl + 5, 1);
    std::memcpy(&px,         pl + 6, 4);
    std::memcpy(&qty,        pl + 10, 4);
    return true;
}
