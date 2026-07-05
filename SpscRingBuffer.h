#pragma once
#include <atomic>
#include <cstring>
#include <memory>
#include <utility>

template <typename T> class SpscRingBuffer {
public:
  explicit SpscRingBuffer(uint8_t exponent)
      : capacity_(1ULL << exponent), mask_(capacity_ - 1),
        buffer_(std::make_unique<T[]>(capacity_)) {

    std::memset(buffer_.get(), 0, capacity_ * sizeof(T));
  }

  template <typename... Args> bool try_emplace(Args &&...args) {
    const size_t current_tail = tail_.load(std::memory_order_relaxed);

    if ((current_tail - head_cached_) == capacity_) {
      head_cached_ = head_.load(std::memory_order_acquire);
      if ((current_tail - head_cached_) == capacity_) {
        return false; // queue full
      }
    }

    buffer_[current_tail & mask_] = T{std::forward<Args>(args)...};

    tail_.store(current_tail + 1, std::memory_order_release);
    return true;
  }

  bool try_push(T &&item) {
    const size_t current_tail = tail_.load(std::memory_order_relaxed);

    if ((current_tail - head_cached_) == capacity_) {
      head_cached_ = head_.load(std::memory_order_acquire);
      if ((current_tail - head_cached_) == capacity_) {
        return false; // Kön är full
      }
    }

    buffer_[current_tail & mask_] = std::move(item);

    tail_.store(current_tail + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T &value) {
    const size_t current_head = head_.load(std::memory_order_relaxed);

    if (current_head == tail_cached_) {
      tail_cached_ = tail_.load(std::memory_order_acquire);
      if (current_head == tail_cached_) {
        return false; // Kön är tom
      }
    }

    value = std::move(buffer_[current_head & mask_]);

    head_.store(current_head + 1, std::memory_order_release);
    return true;
  }

private:
  const size_t capacity_;
  const size_t mask_;
  const std::unique_ptr<T[]> buffer_;

  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) size_t head_cached_{0};

  alignas(64) std::atomic<size_t> tail_{0};
  alignas(64) size_t tail_cached_{0};
};
