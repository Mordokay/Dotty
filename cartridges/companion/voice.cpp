#include "voice.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/stream_buffer.h>
#include <math.h>

#include <string>

#include "audio_player.h"
#include "log.h"
#include "pcm_ring.h"

namespace voice {
namespace {

constexpr size_t kCaptureBytes = 64 * 1024;           // ~2 s of microphone, if the network stalls
constexpr size_t kBacklogFrames = 60 * kOutRate;      // a minute of reply, allocated once (2.9 MB PSRAM)

AudioPlayer *audio = nullptr;
StreamBufferHandle_t captured = nullptr;
TaskHandle_t captureTask = nullptr;
volatile bool listening = false, captureRunning = false;
volatile int level = -60;

// Reply audio not yet in the player's 2 s ring (replies arrive faster than they play). Fixed size:
// a growing string doubled itself to 8 MB on a long reply and couldn't be allocated.
PcmRing *backlog = nullptr;
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
  auto *frames = static_cast<int16_t *>(heap_caps_malloc(kBacklogFrames * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  if (frames) backlog = new PcmRing(frames, kBacklogFrames);
  else LOGE("voice", "no memory for the reply backlog");
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
  if (backlog) backlog->clear();
  replyEnding = false;
  replyActive = audio->startStream(kOutRate);
}

void play(const int16_t *pcm, size_t frames) {
  if (!replyActive || !backlog) return;
  if (backlog->write(pcm, frames) < frames) LOGW("voice", "reply over a minute ahead: dropping audio");
  poll();
}

void playEnd() {
  replyEnding = true;
  poll();
}

void playStop() {
  if (replyActive) audio->stopStream();
  replyActive = false;
  if (backlog) backlog->clear();
}

bool playing() {
  return replyActive && ((backlog && backlog->available()) || audio->streaming());
}

size_t playedFrames() {
  return replyActive ? static_cast<size_t>(audio->streamPlayed()) : SIZE_MAX;
}

void poll() {
  if (!replyActive) return;
  static int16_t chunk[1024];
  while (backlog && backlog->available()) {  // only what the player's ring can take now
    const size_t n = min(min(backlog->available(), audio->streamSpace()), sizeof(chunk) / 2);
    if (n == 0) break;
    backlog->read(chunk, n);
    audio->writeStream(chunk, n);
  }
  if (backlog && !backlog->available() && replyEnding) {
    audio->endStream();
    replyEnding = false;
  }
  if (!audio->streaming() && (!backlog || !backlog->available())) replyActive = false;
}

}  // namespace voice
