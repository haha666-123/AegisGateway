#pragma once

// Bounded multi-producer / single-consumer queue.
//
// The implementation follows the classic Dmitry Vyukov bounded MPMC ring, with
// the consumer side restricted to one thread. Every cell carries a sequence
// number so producers and consumers can detect whether a slot is ready without
// taking a lock:
//
//   cell.seq == pos            -> slot is free, a producer may claim it
//   cell.seq == pos + 1        -> slot is published and readable
//   cell.seq == pos + Capacity -> slot has been consumed and is free again
//
// try_push() never blocks and never allocates. When the ring is full it returns
// false, which lets the caller decide between dropping, spinning, or growing an
// external buffer. This is exactly the behaviour the async logger wants.

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace aegis {

template <class T, std::size_t Capacity>
class MpscBoundedQueue {
    static_assert(Capacity >= 2, "capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

public:
    MpscBoundedQueue() {
        for (std::size_t i = 0; i < Capacity; ++i) {
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    ~MpscBoundedQueue() { drain_and_destroy(); }

    MpscBoundedQueue(const MpscBoundedQueue&) = delete;
    MpscBoundedQueue& operator=(const MpscBoundedQueue&) = delete;

    // Producer side, safe to call from any thread.
    [[nodiscard]] bool try_push(T&& value) {
        Cell* cell = nullptr;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            const std::size_t seq = cell->seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // ring is full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        new (cell->storage) T(std::move(value));
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Consumer side, only one thread may call this at a time.
    [[nodiscard]] bool try_pop(T& out) {
        Cell* cell = nullptr;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            const std::size_t seq = cell->seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) -
                              static_cast<std::ptrdiff_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // ring is empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        T* item = reinterpret_cast<T*>(cell->storage);
        out = std::move(*item);
        item->~T();
        cell->seq.store(pos + Capacity, std::memory_order_release);
        return true;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    // Rough size estimate, useful for metrics only.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const auto enq = enqueue_pos_.load(std::memory_order_relaxed);
        const auto deq = dequeue_pos_.load(std::memory_order_relaxed);
        return static_cast<std::size_t>(enq - deq);
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    struct Cell {
        std::atomic<std::size_t> seq{0};
        alignas(T) unsigned char storage[sizeof(T)];
    };

    Cell cells_[Capacity];
    alignas(64) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(64) std::atomic<std::size_t> dequeue_pos_{0};

    void drain_and_destroy() {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            T tmp{};
            while (try_pop(tmp)) {
            }
        }
    }
};

}  // namespace aegis
