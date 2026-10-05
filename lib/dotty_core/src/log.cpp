#include "log.h"

#include <esp_heap_caps.h>

namespace dlog {
namespace {

constexpr size_t kRingSize = 16 * 1024;
constexpr size_t kMaxLine = 192;
constexpr uint32_t kDrainIntervalMs = 20;

// The ring always holds the most recent kRingSize bytes of log (the history).
// Positions are absolute byte counts since boot; index = position % kRingSize.
char *ring = nullptr;
uint64_t written = 0;  // total bytes ever logged
uint64_t sent = 0;     // live stream position
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

uint64_t oldest() {
  return written > kRingSize ? written - kRingSize : 0;
}

void push(const char *data, size_t len) {
  portENTER_CRITICAL(&lock);
  for (size_t i = 0; i < len; i++) {
    ring[(written + i) % kRingSize] = data[i];
  }
  written += len;
  portEXIT_CRITICAL(&lock);
}

// Copies up to maxLen contiguous bytes from the live stream position.
size_t take(char *out, size_t maxLen, uint64_t &skipped) {
  portENTER_CRITICAL(&lock);
  skipped = 0;
  if (sent < oldest()) {
    skipped = oldest() - sent;
    sent = oldest();
  }
  const size_t index = sent % kRingSize;
  size_t n = min<uint64_t>(maxLen, written - sent);
  n = min(n, kRingSize - index);  // stop at the wrap point
  memcpy(out, ring + index, n);
  sent += n;
  portEXIT_CRITICAL(&lock);
  return n;
}

void replayHistory() {
  portENTER_CRITICAL(&lock);
  sent = oldest();
  portEXIT_CRITICAL(&lock);
}

void drainTask(void *) {
  char chunk[256];
  for (;;) {
    // No computer attached: leave the USB serial port alone. Its driver can end up in an
    // interrupt storm (interrupt watchdog reset on CPU1) when it's fed on battery with the
    // cable gone; the lines wait in the ring and stream once a host is back.
    if (!HWCDC::isPlugged()) {
      vTaskDelay(pdMS_TO_TICKS(kDrainIntervalMs));
      continue;
    }
    // Serial commands: 'd' replays the whole history.
    while (Serial.available()) {
      if (Serial.read() == 'd') {
        replayHistory();
        Serial.print("\n----- log history -----\n");
      }
    }

    int room = Serial.availableForWrite();
    while (room > 0) {
      uint64_t skipped;
      const size_t n = take(chunk, min(sizeof(chunk), static_cast<size_t>(room)), skipped);
      if (skipped > 0) {
        Serial.printf("\n[... %llu bytes of older log overwritten ...]\n", skipped);
        break;
      }
      if (n == 0) break;
      Serial.write(reinterpret_cast<uint8_t *>(chunk), n);
      room -= n;
    }
    vTaskDelay(pdMS_TO_TICKS(kDrainIntervalMs));
  }
}

}  // namespace

void begin() {
  ring = static_cast<char *>(heap_caps_malloc(kRingSize, MALLOC_CAP_SPIRAM));
  if (!ring) ring = static_cast<char *>(malloc(kRingSize));
  xTaskCreatePinnedToCore(drainTask, "log", 4096, nullptr, 1, nullptr, 1);
}

void write(char level, const char *tag, const char *fmt, ...) {
  if (!ring) return;
  char line[kMaxLine];
  const uint32_t ms = millis();
  int n = snprintf(line, sizeof(line), "[%6lu.%03lu] %c %s: ", ms / 1000, ms % 1000, level, tag);
  va_list args;
  va_start(args, fmt);
  n += vsnprintf(line + n, sizeof(line) - n, fmt, args);
  va_end(args);
  n = min(n, static_cast<int>(sizeof(line)) - 2);
  line[n++] = '\n';
  push(line, n);
}

}  // namespace dlog
