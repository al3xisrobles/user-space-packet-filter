#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <cassert>

// Small helper to align to cacheline to avoid false sharing
#if !defined(CACHELINE_SIZE)
#define CACHELINE_SIZE 64
#endif

template <typename T, size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be power of two");
    static_assert(std::is_trivially_copyable<T>::value, "T should be trivially copyable for lock-free copy");

public:
    SpscRing() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    // non-copyable, non-movable (shared by reference)
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // try to push; returns false if full
    bool push(const T& item) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t next = (t + 1) & mask_;
        const size_t h = head_.load(std::memory_order_acquire);
        if (next == (h & mask_)) {
            return false; // full
        }
        buffer_[t & mask_] = item; // copy
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // try to pop; returns false if empty
    bool pop(T& out) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);
        if (h == t) return false; // empty
        out = buffer_[h & mask_]; // copy out
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Convenience: check empty/full (approximate)
    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }
    bool full() const {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t next = (t + 1) & mask_;
        return next == (head_.load(std::memory_order_acquire) & mask_);
    }

    // capacity usable (N-1)
    constexpr size_t capacity() const { return N - 1; }

private:
    static constexpr size_t mask_ = N - 1;
    alignas(CACHELINE_SIZE) std::atomic<size_t> head_;
    char pad1_[CACHELINE_SIZE - sizeof(std::atomic<size_t>)]{};
    alignas(CACHELINE_SIZE) std::atomic<size_t> tail_;
    char pad2_[CACHELINE_SIZE - sizeof(std::atomic<size_t>)]{};
    T buffer_[N];
};
