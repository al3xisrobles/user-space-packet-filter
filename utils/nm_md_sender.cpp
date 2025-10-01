#ifndef NETMAP_WITH_LIBS
#define NETMAP_WITH_LIBS
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <net/netmap_user.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

static volatile bool g_running = true;

static uint16_t ip_checksum(const void* vdata, size_t length) {
    const uint8_t* data = (const uint8_t*)vdata;
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < length; i += 2) {
        sum += (uint16_t)data[i] << 8 | data[i + 1];
    }
    if (length & 1) sum += (uint16_t)data[length - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return ~sum;
}

static bool parse_mac(const char* s, uint8_t mac[6]) {
    int vals[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &vals[0], &vals[1], &vals[2], &vals[3], &vals[4],
            &vals[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)vals[i];
    return true;
}

int main(int argc, char** argv) {
    std::string ifname = "netmap:vale0:1";
    std::string dst_mac_s = "ff:ff:ff:ff:ff:ff";
    std::string src_mac_s = "02:00:00:00:00:01";
    std::string src_ip_s = "10.0.0.1";
    std::string dst_ip_s = "10.0.0.2";
    uint16_t dst_port = 5001;
    uint64_t count = 0;  // 0 means run forever
    uint64_t rate_pps = 1000;  // packets per second (approx)

    int opt;
    while ((opt = getopt(argc, argv, "i:s:d:S:D:p:c:r:h")) != -1) {
        switch (opt) {
            case 'i':
                ifname = optarg;
                break;
            case 's':
                src_mac_s = optarg;
                break;
            case 'd':
                dst_mac_s = optarg;
                break;
            case 'S':
                src_ip_s = optarg;
                break;
            case 'D':
                dst_ip_s = optarg;
                break;
            case 'p':
                dst_port = (uint16_t)atoi(optarg);
                break;
            case 'c':
                count = strtoull(optarg, nullptr, 10);
                break;
            case 'r':
                rate_pps = strtoull(optarg, nullptr, 10);
                break;
            case 'h':
            default:
                std::cerr << "Usage: " << argv[0]
                          << " [-i netmap:iface] [-s src_mac] [-d dst_mac]\n"
                          << "  [-S src_ip] [-D dst_ip] [-p dst_port] [-c count] [-r "
                             "rate_pps]\n";
                return 1;
        }
    }

    signal(SIGINT, [](int) { g_running = false; });
    signal(SIGTERM, [](int) { g_running = false; });

    uint8_t dst_mac[6], src_mac[6];
    if (!parse_mac(dst_mac_s.c_str(), dst_mac)) {
        std::cerr << "bad dst mac\n";
        return 1;
    }
    if (!parse_mac(src_mac_s.c_str(), src_mac)) {
        std::cerr << "bad src mac\n";
        return 1;
    }

    in_addr src_ip{}, dst_ip{};
    if (inet_pton(AF_INET, src_ip_s.c_str(), &src_ip) != 1) {
        std::cerr << "bad src ip\n";
        return 1;
    }
    if (inet_pton(AF_INET, dst_ip_s.c_str(), &dst_ip) != 1) {
        std::cerr << "bad dst ip\n";
        return 1;
    }

    struct nm_desc* nmd = nm_open(ifname.c_str(), NULL, 0, NULL);
    if (!nmd) {
        std::perror("nm_open");
        return 2;
    }
    struct netmap_if* nifp = nmd->nifp;
    if (!nifp) {
        std::cerr << "no nifp\n";
        nm_close(nmd);
        return 3;
    }

    // use first TX ring (0)
    struct netmap_ring* txring = NETMAP_TXRING(nifp, 0);
    if (!txring) {
        std::cerr << "no tx ring\n";
        nm_close(nmd);
        return 4;
    }

    const size_t payload_len = 14;
    const size_t eth_len = sizeof(ether_header);
    const size_t ip_len = sizeof(iphdr);
    const size_t udp_len = sizeof(udphdr);
    const size_t pkt_len = eth_len + ip_len + udp_len + payload_len;
    if (pkt_len > 2048) {
        std::cerr << "packet too large\n";
        nm_close(nmd);
        return 5;
    }

    std::mt19937 rng((unsigned)time(nullptr));
    std::uniform_int_distribution<uint32_t> instr_dist(1, 0xFFFFFF);
    std::uniform_int_distribution<int> type_dist(0, 2);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_real_distribution<float> valf(1.0f, 100.0f);

    uint64_t sent = 0;
    auto interval = std::chrono::microseconds(rate_pps ? (1000000ULL / rate_pps) : 0);

    while (g_running && (count == 0 || sent < count)) {
        if (nm_ring_space(txring) == 0) {
            // no room - poll sync to flush
            ioctl(nmd->fd, NIOCTXSYNC, NULL);
            // busy-wait until space appears
            continue;
        }

        uint32_t cur = txring->cur;
        struct netmap_slot* slot = &txring->slot[cur];
        char* buf = NETMAP_BUF(txring, slot->buf_idx);
        memset(buf, 0, pkt_len);

        // Ethernet
        ether_header* eth = (ether_header*)buf;
        memcpy(eth->ether_dhost, dst_mac, 6);
        memcpy(eth->ether_shost, src_mac, 6);
        eth->ether_type = htons(ETHERTYPE_IP);

        // IPv4 header
        iphdr* iph = (iphdr*)(buf + eth_len);
        iph->ihl = 5;
        iph->version = 4;
        iph->tos = 0;
        iph->tot_len = htons(ip_len + udp_len + payload_len);
        iph->id = htons(0);
        iph->frag_off = htons(0);
        iph->ttl = 64;
        iph->protocol = IPPROTO_UDP;
        iph->saddr = src_ip.s_addr;
        iph->daddr = dst_ip.s_addr;
        iph->check = 0;
        iph->check = ip_checksum(iph, ip_len);

        // UDP header
        udphdr* udph = (udphdr*)(buf + eth_len + ip_len);
        udph->source = htons(12345);
        udph->dest = htons(dst_port);
        udph->len = htons(udp_len + payload_len);
        udph->check = 0;  // optional/zero per requirements

        // Payload (14 bytes): <u32, u8, u8, f32, f32> little-endian
        uint8_t* payload = (uint8_t*)(buf + eth_len + ip_len + udp_len);
        uint32_t instr = instr_dist(rng);
        uint8_t itype = (uint8_t)type_dist(rng);
        uint8_t side = (uint8_t)side_dist(rng);
        float px = valf(rng);
        float qty = valf(rng);

        // write little-endian explicitly
        payload[0] = (instr >> 0) & 0xFF;
        payload[1] = (instr >> 8) & 0xFF;
        payload[2] = (instr >> 16) & 0xFF;
        payload[3] = (instr >> 24) & 0xFF;
        payload[4] = itype;
        payload[5] = side;
        memcpy(payload + 6, &px, 4);
        memcpy(payload + 10, &qty, 4);

        // slot length & advance ring
        slot->len = pkt_len;
        txring->cur = nm_ring_next(txring, cur);
        txring->head = txring->cur;

        // notify kernel/user-netmap that there are packets to send
        ioctl(nmd->fd, NIOCTXSYNC, NULL);

        ++sent;
        if (sent % 1000 == 0) { std::cerr << "sent=" << sent << "\n"; }

        if (interval.count() > 0) { std::this_thread::sleep_for(interval); }
    }

    std::cerr << "exiting, sent=" << sent << "\n";
    nm_close(nmd);
    return 0;
}
