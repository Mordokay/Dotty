#pragma once

#include <stdint.h>

#include <atomic>
#include <utility>

// One background job at a time (the writer / recall task, a second TLS connection) and its hand-over
// to the loop, safe across cores: the task copies everything it needs before it starts (a request
// kept in a global was being rewritten by the next job while the old task still sent it), finishes
// by posting its result, and the loop takes it only if it's the current job — BOOT or a disconnect
// cancels, and a cancelled job's result is dropped. Plain C++ (host-tested).
template <typename Result>
class JobSlot {
 public:
  ~JobSlot() { delete done_.exchange(nullptr); }

  bool busy() const { return running_.load() > 0; }  // a task is still running (cancelled or not)
  uint32_t start() {                                 // the caller checks busy() first
    running_++;
    delete done_.exchange(nullptr);  // a stale result must not land on this job
    return ++current_;
  }
  void cancel() { current_++; }
  void finish(uint32_t job, Result result) {  // on the task, as its last step
    delete done_.exchange(new Done{job, std::move(result)});
    running_--;
  }
  bool take(Result &out) {  // on the loop
    Done *d = done_.exchange(nullptr);
    if (!d) return false;
    const bool current = d->job == current_.load();
    if (current) out = std::move(d->result);
    delete d;
    return current;
  }

 private:
  struct Done {
    uint32_t job;
    Result result;
  };
  std::atomic<uint32_t> current_{0};
  std::atomic<int> running_{0};
  std::atomic<Done *> done_{nullptr};
};
