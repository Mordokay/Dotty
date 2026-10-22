#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The voice samples built into the firmware (tools/build_voice_pack.py): "DVS1", a count, then
// {name[16], offset, size} per sample (little-endian), then the MP3s back to back.
class VoicePack {
 public:
  VoicePack(const uint8_t *data, size_t size) : data_(data), size_(size) {}

  bool valid() const {
    if (size_ < 8 || memcmp(data_, "DVS1", 4) != 0 || count() > 256) return false;
    if (8 + static_cast<size_t>(count()) * kEntry > size_) return false;
    for (int i = 0; i < count(); i++)
      if (static_cast<size_t>(offset(i)) + length(i) > size_) return false;
    return true;
  }
  int count() const { return static_cast<int>(u32(4)); }
  // Up to 15 characters, zero-padded in the file.
  const char *name(int i) const {
    memcpy(name_, data_ + 8 + i * kEntry, 16);
    name_[15] = 0;
    return name_;
  }
  bool find(const char *voice, const uint8_t *&bytes, size_t &len) const {
    if (!valid()) return false;
    for (int i = 0; i < count(); i++) {
      if (strcmp(name(i), voice) != 0) continue;
      bytes = data_ + offset(i);
      len = length(i);
      return true;
    }
    return false;
  }

 private:
  static constexpr size_t kEntry = 24;
  uint32_t u32(size_t at) const {
    return data_[at] | data_[at + 1] << 8 | data_[at + 2] << 16 | static_cast<uint32_t>(data_[at + 3]) << 24;
  }
  uint32_t offset(int i) const { return u32(8 + i * kEntry + 16); }
  uint32_t length(int i) const { return u32(8 + i * kEntry + 20); }

  const uint8_t *data_;
  size_t size_;
  mutable char name_[16];
};
