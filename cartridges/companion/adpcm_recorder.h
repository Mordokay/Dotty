#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "adpcm.h"

// The replay's recording, kept as IMA ADPCM (a quarter of the size): two minutes of a 24 kHz reply in
// ~1.4 MB instead of 5.8 (PSRAM can't spare two raw minutes next to the reply's own buffer). Full
// frames of kFrame samples are encoded as they fill; the rest waits raw. decode() hands the audio back
// in order, for the WAV file. Plain C++ (host-tested).
class AdpcmRecorder {
 public:
  static constexpr size_t kFrame = 570;
  static constexpr size_t kFrameBytes = adpcm::kHeader + kFrame / 2;

  void reset(size_t maxSamples) {
    std::string().swap(data_);
    std::vector<int16_t>().swap(pending_);
    state_ = adpcm::State();
    max_ = maxSamples;
    total_ = 0;
  }
  void add(const int16_t *pcm, size_t count) {
    const size_t take = total_ + count > max_ ? max_ - total_ : count;
    if (take == 0) return;
    if (data_.capacity() == 0) data_.reserve(max_ / kFrame * kFrameBytes + kFrameBytes);  // once (PSRAM on Dotty)
    pending_.insert(pending_.end(), pcm, pcm + take);
    total_ += take;
    size_t at = 0;
    uint8_t frame[kFrameBytes];
    while (pending_.size() - at >= kFrame) {
      const size_t n = adpcm::encodeFrame(pending_.data() + at, kFrame, state_, frame);
      data_.append(reinterpret_cast<const char *>(frame), n);
      at += kFrame;
    }
    pending_.erase(pending_.begin(), pending_.begin() + at);
  }
  size_t samples() const { return total_; }
  size_t bytes() const { return data_.size() + pending_.size() * 2; }

  // sink(const int16_t *pcm, size_t count), in order: every full frame decoded, then the raw rest.
  template <typename Sink>
  void decode(Sink sink) const {
    int16_t pcm[kFrame];
    for (size_t at = 0; at + kFrameBytes <= data_.size(); at += kFrameBytes) {
      const size_t n = adpcm::decodeFrame(reinterpret_cast<const uint8_t *>(data_.data()) + at, kFrameBytes, pcm, kFrame);
      sink(pcm, n);
    }
    if (!pending_.empty()) sink(pending_.data(), pending_.size());
  }

 private:
  std::string data_;
  std::vector<int16_t> pending_;
  adpcm::State state_;
  size_t max_ = 0, total_ = 0;
};
