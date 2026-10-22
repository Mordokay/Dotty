#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>

// A ring of mono 16-bit samples for streaming playback: one writer (e.g. a network task) and one
// reader (the audio task), no locks, no allocation (the caller owns the storage, e.g. in PSRAM).
class PcmRing {
 public:
  PcmRing(int16_t *storage, size_t frames) : buf_(storage), size_(frames) {}

  // Copies what fits; returns how many frames were taken.
  size_t write(const int16_t *src, size_t n) {
    const size_t head = head_.load(std::memory_order_relaxed), tail = tail_.load(std::memory_order_acquire);
    const size_t take = n < size_ - (head - tail) ? n : size_ - (head - tail);
    for (size_t i = 0; i < take; i++) buf_[(head + i) % size_] = src[i];
    head_.store(head + take, std::memory_order_release);
    return take;
  }

  // Copies up to n frames out; returns how many.
  size_t read(int16_t *dst, size_t n) {
    const size_t tail = tail_.load(std::memory_order_relaxed), head = head_.load(std::memory_order_acquire);
    const size_t take = n < head - tail ? n : head - tail;
    for (size_t i = 0; i < take; i++) dst[i] = buf_[(tail + i) % size_];
    tail_.store(tail + take, std::memory_order_release);
    return take;
  }

  size_t available() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }
  size_t space() const { return size_ - available(); }
  void clear() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

 private:
  int16_t *buf_;
  size_t size_;
  std::atomic<size_t> head_{0}, tail_{0};  // frames ever written / read (wrap with % size_)
};
