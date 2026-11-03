#pragma once

#include <stdint.h>

#include <string>

#include "assets_manifest.h"

// The rules for the Companion's packs on the card (spec: docs/superpowers/specs/2026-11-03-companion-make-
// room-design.md), plain C++ (host-tested): a pack is ready when its stamp (data/assets/<stem>.stamp, the
// SHA-256 of the pack last installed) is the one the firmware wants; it's downloaded from the latest release
// (or a bench server); and a missing one is tried 30 s after start, 10 min after a failure, then hourly.

constexpr const char *kReleaseAssetBase = "https://github.com/Mordokay/Dotty/releases/latest/download/";

inline bool assetReady(const std::string &stampText, const AssetPack &p) {
  size_t end = stampText.size();
  while (end && (stampText[end - 1] == '\n' || stampText[end - 1] == '\r' || stampText[end - 1] == ' ')) end--;
  return stampText.compare(0, end, p.sha256) == 0 && end == std::string(p.sha256).size();
}

inline std::string assetUrl(const std::string &base, const AssetPack &p) {
  return base + p.file;
}

class AssetRetry {
 public:
  static constexpr uint32_t kFirstMs = 30 * 1000, kSecondMs = 10 * 60 * 1000, kLaterMs = 60 * 60 * 1000;
  uint32_t due() const { return due_; }
  bool isDue(uint32_t nowMs) const { return !done_ && nowMs >= due_; }
  void failed(uint32_t nowMs) { due_ = nowMs + (failures_++ ? kLaterMs : kSecondMs); }
  void succeeded() { done_ = true; }
  void reset(uint32_t nowMs) {  // a stamp turned stale: try again soon
    done_ = false;
    failures_ = 0;
    due_ = nowMs + kFirstMs;
  }

 private:
  uint32_t due_ = kFirstMs;
  int failures_ = 0;
  bool done_ = false;
};
