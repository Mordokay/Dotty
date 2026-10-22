#include "voice.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/stream_buffer.h>
#include <math.h>

#include <string>

#include "audio_player.h"
#include "log.h"

namespace voice {
namespace {

constexpr size_t kCaptureBytes = 64 * 1024;           // ~2 s of microphone, if the network stalls
constexpr size_t kMaxBacklogBytes = 90 * kOutRate * 2;  // 90 s of reply

AudioPlayer *audio = nullptr;
StreamBufferHandle_t captured = nullptr;
TaskHandle_t captureTask = nullptr;
volatile bool listening = false, captureRunning = false;
volatile int level = -60;

std::string backlog;  // reply PCM bytes not yet in the player's ring (PSRAM: it grows past 4 KB)
size_t backlogRead = 0;
bool replyEnding = false, replyActive = false;

void captureLoop(void *) {
  static int16_t block[kChunkFrames];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    captureRunning = true;
    while (listening) {
      const size_t got = audio->capture(block, kChunkFrames);
      if (!got) continue;
      int peak = 1;
      for (size_t i = 0; i < got; i++) peak = max(peak, abs(block[i]));
      level = max(-60, static_cast<int>(20 * log10f(peak / 32768.0f)));
      if (xStreamBufferSend(captured, block, got * 2, 0) != got * 2) {
        static uint32_t lastWarned = 0;  // once a second: a stalled reader flooded the log ring
        if (millis() - lastWarned > 1000) {
          lastWarned = millis();
          LOGW("voice", "capture buffer full (the loop isn't reading)");
        }
      }
    }
    captureRunning = false;
  }
}

}  // namespace

void begin(AudioPlayer &player) {
  audio = &player;
  uint8_t *storage = static_cast<uint8_t *>(heap_caps_malloc(kCaptureBytes + 1, MALLOC_CAP_SPIRAM));
  static StaticStreamBuffer_t control;
  captured = xStreamBufferCreateStatic(kCaptureBytes, kChunkFrames * 2, storage, &control);
  xTaskCreatePinnedToCore(captureLoop, "voice-in", 4096, nullptr, configMAX_PRIORITIES - 3, &captureTask, 0);
}

bool startListening() {
  playStop();
  xStreamBufferReset(captured);
  if (!audio->startCapture(30, 2.5f)) return false;
  level = -60;
  listening = true;
  xTaskNotifyGive(captureTask);
  return true;
}

void stopListening() {
  listening = false;
  for (int i = 0; i < 50 && captureRunning; i++) delay(5);  // let the last block finish
  audio->stopCapture();
}

bool nextChunk(int16_t *out, size_t &frames) {
  const size_t ready = xStreamBufferBytesAvailable(captured);
  // While listening wait for a whole chunk; afterwards hand over whatever is left.
  if (ready == 0 || (listening && ready < kChunkFrames * 2)) return false;
  frames = xStreamBufferReceive(captured, out, kChunkFrames * 2, 0) / 2;
  return frames > 0;
}

int levelDb() {
  return level;
}

void playStart() {
  backlog.clear();
  backlogRead = 0;
  replyEnding = false;
  replyActive = audio->startStream(kOutRate);
}

void play(const int16_t *pcm, size_t frames) {
  if (!replyActive) return;
  if (backlog.size() - backlogRead + frames * 2 > kMaxBacklogBytes) {
    LOGW("voice", "reply longer than 90 s: dropping audio");
    return;
  }
  backlog.append(reinterpret_cast<const char *>(pcm), frames * 2);
  poll();
}

void playEnd() {
  replyEnding = true;
  poll();
}

void playStop() {
  if (replyActive) audio->stopStream();
  replyActive = false;
  backlog.clear();
  backlogRead = 0;
}

bool playing() {
  return replyActive && (backlogRead < backlog.size() || audio->streaming());
}

size_t playedFrames() {
  return replyActive ? static_cast<size_t>(audio->streamPlayed()) : SIZE_MAX;
}

void poll() {
  if (!replyActive) return;
  while (backlogRead + 1 < backlog.size()) {
    const size_t frames = (backlog.size() - backlogRead) / 2;
    const size_t taken = audio->writeStream(reinterpret_cast<const int16_t *>(backlog.data() + backlogRead), frames);
    if (taken == 0) break;
    backlogRead += taken * 2;
  }
  if (backlogRead >= backlog.size()) {
    backlog.clear();
    backlogRead = 0;
    if (replyEnding) {
      audio->endStream();
      replyEnding = false;
    }
  }
  if (!audio->streaming() && backlog.empty()) replyActive = false;
}

}  // namespace voice
