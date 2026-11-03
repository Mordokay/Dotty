#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The avatars kept on the card (tools/avatar_art.py → assets/avatars.bin, every avatar but the robot,
// which is built in): "DAV1", a count, id[16] per avatar (zero-padded), then per avatar in that order
// 12 faces at 128 px (2048 B) and 12 at 48 px (288 B); rows MSB-first, set bit = black.
class AvatarPack {
 public:
  static constexpr int kFaces = 12;
  static constexpr size_t kBigFace = 2048, kSmallFace = 288;

  AvatarPack(const uint8_t *data, size_t size) : data_(data), size_(size) {}

  bool valid() const {
    if (!data_ || size_ < 8 || memcmp(data_, "DAV1", 4) != 0) return false;
    const uint32_t n = u32(4);
    return n <= kMaxAvatars && size_ == faces() + n * kBlock;
  }
  int count() const { return valid() ? static_cast<int>(u32(4)) : 0; }
  // Up to 15 characters, zero-padded in the file.
  const char *id(int i) const {
    memcpy(id_, data_ + 8 + i * kId, kId);
    id_[kId - 1] = 0;
    return id_;
  }
  // A face of avatar `avatarId`, or nullptr (no valid pack, an id it lacks, a face out of range).
  const uint8_t *face(const char *avatarId, int face, bool big) const {
    if (face < 0 || face >= kFaces) return nullptr;
    for (int i = 0; i < count(); i++) {
      if (strcmp(id(i), avatarId) != 0) continue;
      const uint8_t *block = data_ + faces() + i * kBlock;
      return big ? block + face * kBigFace : block + kFaces * kBigFace + face * kSmallFace;
    }
    return nullptr;
  }

 private:
  static constexpr size_t kId = 16, kBlock = kFaces * (kBigFace + kSmallFace);
  static constexpr uint32_t kMaxAvatars = 32;
  uint32_t u32(size_t at) const {
    return data_[at] | data_[at + 1] << 8 | data_[at + 2] << 16 | static_cast<uint32_t>(data_[at + 3]) << 24;
  }
  size_t faces() const { return 8 + u32(4) * kId; }  // where the first avatar's faces start

  const uint8_t *data_;
  size_t size_;
  mutable char id_[kId];
};
