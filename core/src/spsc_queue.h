#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace sorus {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padding from alignas is the point: head/tail on separate cache lines
#endif

// Wait-free single-producer single-consumer queue holding up to N items.
// Counters only grow; size = tail - head, index = counter & (N - 1).
template <class T, size_t N>
class SpscQueue {
  static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

 public:
  bool push(const T& v) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_.load(std::memory_order_acquire) == N) return false;
    items_[tail & (N - 1)] = v;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool pop(T& out) {
    const size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) return false;
    out = items_[head & (N - 1)];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

 private:
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
  std::array<T, N> items_{};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace sorus
