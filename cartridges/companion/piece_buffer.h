#pragma once

#include <stddef.h>

#include <string>

// Something sent in pieces (a long profile field, a photo: BLE commands carry ≤ 512 bytes), kept
// aside until the last piece arrives — an interrupted upload leaves the old value as it was.
class PieceBuffer {
 public:
  // `max`: the most a client may announce (it's reserved up front, in internal RAM).
  bool begin(size_t expected, size_t max = 4096) {
    clear();
    if (expected == 0 || expected > max) return false;
    expected_ = expected;
    data_.reserve(expected);
    return true;
  }
  // Pieces must come in order; one past the end or out of order is refused.
  bool add(size_t offset, const std::string &bytes) {
    if (offset != data_.size() || data_.size() + bytes.size() > expected_) return false;
    data_ += bytes;
    return true;
  }
  bool complete() const { return expected_ > 0 && data_.size() == expected_; }
  const std::string &data() const { return data_; }
  void clear() {  // gives the memory back, not just the contents
    std::string().swap(data_);
    expected_ = 0;
  }

 private:
  std::string data_;
  size_t expected_ = 0;
};
