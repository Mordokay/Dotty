#pragma once

#include <stdint.h>

// The phone wake (spec: "The phone wake"): a general way to reach the iPhone while Dotty is locked.
// Every wakeEvery minutes, only when a job is due (recording lines, the daily plan; the phone's bundle
// later), Dotty turns Bluetooth on (shell::bluetoothWindow) and waits up to 30 s for the phone to
// reconnect and say relay.hello; the jobs then run through it for up to 3 minutes. No phone: the jobs
// that can use Wi-Fi run over Wi-Fi (Bluetooth off), the others wait for the next wake. Bluetooth is
// always off at the end. Plain C++ (host-tested); main.cpp runs the jobs.
class PhoneWake {
 public:
  static constexpr uint32_t kPhoneWaitMs = 30000, kJobsMaxMs = 180000;
  enum class Stage { Idle, WaitingForPhone, Phone, Wifi, Done };

  // The next wake's time (local epoch): lastWake + wakeEvery minutes when a job is due, else 0.
  int64_t nextWakeAt(int64_t lastWake, int wakeEveryMinutes, bool jobDue) const {
    return jobDue ? lastWake + static_cast<int64_t>(wakeEveryMinutes) * 60 : 0;
  }
  void start(uint32_t nowMs) {
    stage_ = Stage::WaitingForPhone;
    startedAt_ = nowMs;
  }
  // phoneReady: relay.hello said, with a key in common; wifiJobs: a due job can use Wi-Fi; jobsDone:
  // nothing left to do.
  Stage poll(uint32_t nowMs, bool phoneReady, bool wifiJobs, bool jobsDone) {
    switch (stage_) {
      case Stage::WaitingForPhone:
        if (phoneReady) {
          enter(Stage::Phone, nowMs);
        } else if (nowMs - startedAt_ >= kPhoneWaitMs) {
          if (wifiJobs) enter(Stage::Wifi, nowMs);
          else stage_ = Stage::Done;
        }
        break;
      case Stage::Phone:
      case Stage::Wifi:
        if (jobsDone || nowMs - jobsAt_ >= kJobsMaxMs) stage_ = Stage::Done;
        break;
      default:
        break;
    }
    return stage_;
  }
  // Is the wake's work over? A running job always finishes; two failures (no internet, quota) end the rest
  // for this wake, plan included — else a day without internet kept every wake awake its full 3 minutes.
  static bool jobsDone(bool planDue, bool planRunning, bool lineBusy, bool lineLeft, int failures) {
    if (planRunning || lineBusy) return false;
    if (failures >= 2) return true;
    return !planDue && !lineLeft;
  }
  bool bluetoothOn() const { return stage_ == Stage::WaitingForPhone || stage_ == Stage::Phone; }
  Stage stage() const { return stage_; }
  void end() { stage_ = Stage::Idle; }  // a chat, BOOT or a firing moment cancels a wake; or it's over

 private:
  void enter(Stage s, uint32_t nowMs) {
    stage_ = s;
    jobsAt_ = nowMs;
  }
  Stage stage_ = Stage::Idle;
  uint32_t startedAt_ = 0, jobsAt_ = 0;
};
