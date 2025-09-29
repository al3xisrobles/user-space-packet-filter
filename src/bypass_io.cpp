#include "bypass_io.h"
#include <poll.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "common.h"
#include "spsc_ring.h"

#ifdef USE_NETMAP
#ifndef NETMAP_WITH_LIBS
#define NETMAP_WITH_LIBS
#endif
#include <net/netmap_user.h>
#endif

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

void pin_thread_to_core(int core) {
#ifdef __linux__
    if (core < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)core;
#endif
}

// ----------------- Netmap-backed implementation -----------------
struct BypassIO::Impl {
#ifdef USE_NETMAP
    nm_desc* nmd{nullptr};
#endif
    int fd{-1};
    int rx_first{0}, rx_last{0};
    int tx_first{0}, tx_last{0};
    int vnet_len{0};
};

BypassIO::BypassIO(const BypassConfig& cfg) : cfg_(cfg), impl_(new Impl) {
#ifdef USE_NETMAP
    nm_desc* nmd = nm_open(cfg_.ifname.c_str(), nullptr, 0, nullptr);
    if (!nmd) {
        ok_ = false;
        return;
    }
    impl_->nmd = nmd;
    impl_->fd = nmd->fd;
    impl_->rx_first = (cfg_.rx_ring_first >= 0) ? cfg_.rx_ring_first : nmd->first_rx_ring;
    impl_->rx_last = (cfg_.rx_ring_last >= 0) ? cfg_.rx_ring_last : nmd->last_rx_ring;
    impl_->tx_first = (cfg_.tx_ring_first >= 0) ? cfg_.tx_ring_first : nmd->first_tx_ring;
    impl_->tx_last = (cfg_.tx_ring_last >= 0) ? cfg_.tx_ring_last : nmd->last_tx_ring;

    // Optional virtio-net header negotiation (see pkt-gen get/set helpers)
    if (cfg_.enable_vnet_hdr) {
        // best-effort: request header via ioctl path (omitted here for brevity)
        // impl_->vnet_len = 10 or 12 depending on host; leave 0 if unsupported
    }
    ok_ = true;
#else
    ok_ = false;  // No fallback here; keep explicit for this project
#endif
}

BypassIO::~BypassIO() {
#ifdef USE_NETMAP
    if (impl_ && impl_->nmd) { nm_close(impl_->nmd); }
#endif
    delete impl_;
}

int BypassIO::rx_batch(const std::function<bool(const PacketView&)>& cb) {
    if (!ok_) return -1;
#ifdef USE_NETMAP
    auto* nmd = impl_->nmd;
    int processed = 0;

    // Optionally use poll() if busy_poll is false; otherwise spin for low latency
    if (!cfg_.busy_poll) {
        pollfd pfd{impl_->fd, POLLIN, 0};
        if (poll(&pfd, 1, 1000) <= 0) return 0;
    }

    const int limit_per_ring = cfg_.burst;

    for (int r = impl_->rx_first; r <= impl_->rx_last; ++r) {
        auto* ring = NETMAP_RXRING(nmd->nifp, r);
        uint32_t avail = nm_ring_space(ring);
        if (avail == 0) continue;

        uint32_t take =
            (avail > (uint32_t)limit_per_ring) ? (uint32_t)limit_per_ring : avail;
        uint32_t cur = ring->cur;

        for (uint32_t i = 0; i < take; ++i) {
            auto& slot = ring->slot[cur];
            auto* buf = (uint8_t*)NETMAP_BUF(ring, slot.buf_idx);
            PacketView v{buf, (uint16_t)slot.len, rdtsc()};
            ++processed;
            ++stats_.pkts;
            stats_.bytes += slot.len;

            if (!cb(v)) {
                ring->head = ring->cur = cur;
                return processed;
            }
            cur = nm_ring_next(ring, cur);
        }
        ring->head = ring->cur = cur;
        ++stats_.batches;
    }
    return processed;
#else
    return -1;
#endif
}

int BypassIO::tx(const uint8_t* data, uint16_t len) {
#ifndef USE_NETMAP
    (void)data;
    (void)len;
    return -1;
#else
    auto* nmd = impl_->nmd;
    for (int r = impl_->tx_first; r <= impl_->tx_last; ++r) {
        auto* ring = NETMAP_TXRING(nmd->nifp, r);
        if (nm_ring_space(ring) == 0) continue;
        auto& slot = ring->slot[ring->cur];
        uint8_t* dst = (uint8_t*)NETMAP_BUF(ring, slot.buf_idx);
        nm_pkt_copy(data, dst, len);
        slot.len = len;
        ring->head = ring->cur = nm_ring_next(ring, ring->cur);
        return 1;
    }
    ++stats_.drops;  // no space
    return 0;
#endif
}
