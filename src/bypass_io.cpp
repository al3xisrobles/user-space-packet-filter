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

struct BypassIO::Impl {
#ifdef USE_NETMAP
    // Netmap descriptor
    nm_desc* nmd{nullptr};
#endif
    // File descriptor for poll/ioctl
    int fd{-1};

    // RX/TX ring range
    int rx_first{0}, rx_last{0};

    // TX ring range
    int tx_first{0}, tx_last{0};

    // Virtio-net header length (0 = disabled/unsupported)
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

/**
 * @brief Drain RX rings and invoke callback on each packet.
 *
 * Attempts to receive up to `cfg_.burst` packets from the netmap-backed
 * RX rings. For each available packet, a PacketView is constructed and
 * passed to the callback `cb`.
 *
 * - If the callback returns `true`, processing continues to the next packet.
 * - If the callback returns `false`, something went wrong, so processing stops
 *   early and the current position is saved for the next call.
 *
 * Behavior depends on polling mode:
 * - In blocking poll mode, waits for readiness using poll().
 * - In busy-poll mode, explicitly calls NIOCRXSYNC to spin until packets arrive.
 *
 * @param cb Callback applied to each packet. Must return `false` ONLY to stop draining
 * early.
 * @return Number of packets consumed from RX rings this batch, or -1 on error.
 */
int BypassIO::rx_batch(const std::function<bool(const PacketView&)>& cb) {
    if (!ok_) return -1;
#ifdef USE_NETMAP
    auto* nmd = impl_->nmd;
    int processed = 0;

    // If busy_poll is enabled, we skip the poll() step and directly issue
    // NIOCRXSYNC to synchronize the kernel's view of the RX rings with
    // userspace.
    if (cfg_.busy_poll) {
        // In busy-poll mode, explicitly ask the kernel to sync all RX rings.
        // NIOCRXSYNC does not block; it returns immediately if no packets
        // are available. The kernel updates the ring state so that we can
        // read packets below.
        ioctl(impl_->fd, NIOCRXSYNC, nullptr);
    } else {
        // Set up a pollfd structure to wait for the netmap file descriptor to
        // be readable.
        pollfd pfd{impl_->fd, POLLIN, 0};

        // Block the calling thread until the kernel marks the fd as readable
        // (i.e., if the tail pointer > curr, then nm_ring_space() > 0). If
        // poll() returns > 0, the kernel has done the equivalent of NIOCRXSYNC
        // and we can proceed to read packets. If it returns 0, we timed out. We
        // could use a timeout of -1 to block indefinitely, but a finite timeout
        // allows us to return to the caller periodically (e.g., to check a
        // running flag).
        if (poll(&pfd, 1, 1000) <= 0) return 0;
    }

    // Our burst limit is per ring; we iterate over all RX rings in the
    // interface and process up to cfg_.burst packets from each ring.
    const int limit_per_ring = cfg_.burst;

    // Iterate over the RX rings
    for (int r = impl_->rx_first; r <= impl_->rx_last; ++r) {
        // Get a pointer to the current RX ring
        auto* ring = NETMAP_RXRING(nmd->nifp, r);

        // Check how many packets are available in the ring. If zero, move to
        // the next ring. If non-zero, we will process up to limit_per_ring
        // packets from this ring.
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
