#pragma once

#include <stdint.h>

// Which way a chat goes (spec: docs/superpowers/specs/2026-10-23-companion-relay-design.md, "Choosing
// the path"): the iPhone over Bluetooth when it's connected and relay-ready; else Wi-Fi is raced
// against it from the first press and whichever is ready first takes the chat. If Wi-Fi won, Bluetooth
// stays on for a grace window — until the first reply ends, and at least 15 s — and a phone that
// connects in it takes the chat at the next press (never mid-turn). Plain C++ (host-tested); times in
// millis().
class RelayPath {
 public:
  enum Way { None, Relay, Wifi };
  static constexpr uint32_t kGraceMs = 15000;

  Way way() const { return way_; }
  bool racing() const { return racing_; }

  void press(uint32_t now, bool phoneReady) {  // a chat starts
    end();
    (void)now;
    if (phoneReady) way_ = Relay;
    else racing_ = true;
  }
  void phoneReady(uint32_t now) {
    if (racing_) {
      racing_ = false;
      way_ = Relay;
      dropWifi_ = true;
    } else if (way_ == Wifi && keepBluetooth(now)) {
      handover_ = true;
    }
  }
  void wifiReady(uint32_t now) {
    if (!racing_) return;
    racing_ = false;
    way_ = Wifi;
    wifiReadyAt_ = now;
  }
  void replyEnded(uint32_t now) {
    if (way_ == Wifi && !replyEnded_) replyEnded_ = true, replyEndAt_ = now;
  }
  // Bluetooth on: racing, in a relay chat, or in a Wi-Fi chat's grace window (and outside a chat).
  bool keepBluetooth(uint32_t now) const {
    if (way_ != Wifi) return true;
    if (!replyEnded_ || handover_) return true;  // a phone waiting to take the chat keeps its link
    const uint32_t minEnd = wifiReadyAt_ + kGraceMs;
    return now <= (replyEndAt_ > minEnd ? replyEndAt_ : minEnd);
  }
  bool takeDropWifi() {  // the phone won the race: drop the Wi-Fi join (once)
    const bool d = dropWifi_;
    dropWifi_ = false;
    return d;
  }
  bool takeHandover() {  // at a press in a Wi-Fi chat: the phone arrived in the window (once)
    if (!handover_) return false;
    handover_ = false;
    way_ = Relay;
    return true;
  }
  void phoneLost() {
    if (way_ == Relay) way_ = None;
  }
  void end() {
    way_ = None;
    racing_ = dropWifi_ = handover_ = replyEnded_ = false;
    wifiReadyAt_ = replyEndAt_ = 0;
  }

 private:
  Way way_ = None;
  bool racing_ = false, dropWifi_ = false, handover_ = false, replyEnded_ = false;
  uint32_t wifiReadyAt_ = 0, replyEndAt_ = 0;
};
