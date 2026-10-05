#include "recorder.h"

#include <SD_MMC.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>

#include <algorithm>

#include "log.h"
#include "storage.h"

namespace tape {
namespace {

constexpr size_t kStreamBytes = 128 * 1024;  // 2 s at 32 kHz: rides out slow card writes
constexpr size_t kBlockFrames = 512;
constexpr size_t kWriteBytes = 8192;
constexpr size_t kHeaderBytes = 44;

AudioPlayer *audio = nullptr;
StreamBufferHandle_t stream = nullptr;
SemaphoreHandle_t fileLock = nullptr;
TaskHandle_t readerTask = nullptr, writerTask = nullptr;
File file;
String name;
volatile State current_ = State::Idle;
volatile bool readerBusy = false;
volatile uint64_t framesCaptured = 0;  // on this tape, all its takes
volatile uint32_t dataBytes = 0;       // on the card
volatile uint8_t peak = 0;
volatile uint32_t dropped = 0;

String dir() {
  return storage::myDataDir() + "/recordings";
}

void header(uint8_t *h, uint32_t bytes) {
  auto put32 = [&](int at, uint32_t v) { memcpy(h + at, &v, 4); };
  auto put16 = [&](int at, uint16_t v) { memcpy(h + at, &v, 2); };
  memcpy(h, "RIFF", 4);
  put32(4, 36 + bytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 1);  // PCM
  put16(22, 1);  // mono
  put32(24, kRate);
  put32(28, kRate * 2);
  put16(32, 2);
  put16(34, 16);
  memcpy(h + 36, "data", 4);
  put32(40, bytes);
}

// Microphone → stream buffer. Never waits on the card.
void readerLoop(void *) {
  static int16_t block[kBlockFrames];
  for (;;) {
    if (current_ != State::Recording) {
      readerBusy = false;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
      continue;
    }
    readerBusy = true;
    const size_t frames = audio->capture(block, kBlockFrames);
    if (frames == 0) continue;
    int16_t loudest = 0;
    for (size_t i = 0; i < frames; i++) loudest = max<int16_t>(loudest, abs(block[i]));
    // Meter in decibels, like a deck's VU: -60 dBFS = empty, 0 dBFS = full.
    const float db = loudest > 0 ? 20 * log10f(loudest / 32768.0f) : -60;
    peak = constrain(static_cast<int>((db + 60) * 100 / 60), 0, 100);
    const size_t bytes = frames * 2;
    if (xStreamBufferSend(stream, block, bytes, 0) != bytes) dropped = dropped + 1;
    framesCaptured = framesCaptured + frames;
  }
}

// Stream buffer → card.
void writerLoop(void *) {
  static uint8_t chunk[kWriteBytes];
  for (;;) {
    const size_t n = xStreamBufferReceive(stream, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
    if (n == 0) continue;
    xSemaphoreTake(fileLock, portMAX_DELAY);
    if (file) dataBytes = dataBytes + file.write(chunk, n);
    xSemaphoreGive(fileLock);
  }
}

// Everything captured so far on the card, and the header telling its length (so a power
// cut keeps what was recorded until the last pause).
void settle() {
  while (readerBusy) delay(5);
  while (xStreamBufferBytesAvailable(stream) > 0) delay(10);
  xSemaphoreTake(fileLock, portMAX_DELAY);
  if (file) {
    uint8_t h[kHeaderBytes];
    header(h, dataBytes);
    const size_t end = file.position();
    file.seek(0);
    file.write(h, sizeof(h));
    file.seek(end);
    file.flush();
  }
  xSemaphoreGive(fileLock);
}

// A recording whose header never got its length (power cut while recording).
void repair(const String &fileName) {
  File f = SD_MMC.open(dir() + "/" + fileName, "r+");
  if (!f || f.size() < kHeaderBytes) return;
  uint8_t h[kHeaderBytes];
  f.read(h, sizeof(h));
  uint32_t stated;
  memcpy(&stated, h + 40, 4);
  const uint32_t actual = f.size() - kHeaderBytes;
  if (memcmp(h, "RIFF", 4) != 0 || stated == actual) return;
  header(h, actual);
  f.seek(0);
  f.write(h, sizeof(h));
  LOGI("tape", "repaired %s (%u bytes)", fileName.c_str(), static_cast<unsigned>(actual));
}

bool isRecording(const String &fileName) {
  String lower = fileName;
  lower.toLowerCase();
  return !fileName.startsWith(".") && lower.endsWith(".wav");
}

}  // namespace

bool begin(AudioPlayer &player) {
  audio = &player;
  storage::makeDirs(dir());
  for (const Recording &r : list()) repair(r.name);
  uint8_t *storage = static_cast<uint8_t *>(heap_caps_malloc(kStreamBytes + 1, MALLOC_CAP_SPIRAM));
  static StaticStreamBuffer_t control;
  if (!storage) return false;
  stream = xStreamBufferCreateStatic(kStreamBytes, 1, storage, &control);
  fileLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(readerLoop, "tape-in", 4096, nullptr, configMAX_PRIORITIES - 3, &readerTask, 0);
  xTaskCreatePinnedToCore(writerLoop, "tape-card", 4096, nullptr, 3, &writerTask, 1);
  return stream && fileLock && readerTask && writerTask;
}

State state() {
  return current_;
}

bool record() {
  if (current_ == State::Recording) return true;
  if (current_ == State::Idle) {
    // Named after when it started (local time), so names sort by date.
    const time_t now = time(nullptr);
    tm t;
    gmtime_r(&now, &t);
    char base[32];
    strftime(base, sizeof(base), "%Y%m%d-%H%M%S", &t);
    name = String(base) + ".wav";
    for (int n = 2; SD_MMC.exists(dir() + "/" + name); n++) name = String(base) + "-" + n + ".wav";
    xSemaphoreTake(fileLock, portMAX_DELAY);
    file = SD_MMC.open(dir() + "/" + name, FILE_WRITE);
    uint8_t h[kHeaderBytes];
    header(h, 0);
    if (file) file.write(h, sizeof(h));
    xSemaphoreGive(fileLock);
    if (!file) {
      LOGE("tape", "cannot create %s", name.c_str());
      name = "";
      return false;
    }
    dataBytes = 0;
    framesCaptured = 0;
    dropped = 0;
    LOGI("tape", "new tape %s", name.c_str());
  }
  if (!audio->startCapture(kMicGainDb)) return false;
  current_ = State::Recording;
  xTaskNotifyGive(readerTask);
  return true;
}

void pause() {
  if (current_ != State::Recording) return;
  current_ = State::Paused;
  audio->stopCapture();
  peak = 0;
  settle();
  LOGI("tape", "paused at %lu s", static_cast<unsigned long>(elapsedMs() / 1000));
}

String finish() {
  if (current_ == State::Idle) return "";
  pause();
  xSemaphoreTake(fileLock, portMAX_DELAY);
  file.close();
  xSemaphoreGive(fileLock);
  current_ = State::Idle;
  const String saved = name;
  name = "";
  if (dataBytes == 0) {  // nothing recorded: no empty file
    SD_MMC.remove(dir() + "/" + saved);
    return "";
  }
  LOGI("tape", "saved %s: %lu s%s", saved.c_str(), static_cast<unsigned long>(dataBytes / 2 / kRate),
       dropped ? " (some sound dropped: the card was slow)" : "");
  return saved;
}

uint32_t elapsedMs() {
  return static_cast<uint64_t>(framesCaptured) * 1000 / kRate;
}

uint8_t level() {
  return current_ == State::Recording ? peak : 0;
}

String current() {
  return name;
}

std::vector<Recording> list() {
  std::vector<Recording> out;
  File d = SD_MMC.open(dir());
  for (File f = d ? d.openNextFile() : File(); f; f = d.openNextFile()) {
    const String fileName = f.name();
    if (f.isDirectory() || !isRecording(fileName) || fileName == name) continue;
    Recording r;
    r.name = fileName;
    r.size = f.size();
    r.durationMs = r.size > kHeaderBytes ? static_cast<uint64_t>(r.size - kHeaderBytes) * 1000 / (kRate * 2) : 0;
    r.added = f.getLastWrite();
    out.push_back(r);
  }
  std::sort(out.begin(), out.end(), [](const Recording &a, const Recording &b) {
    return a.added != b.added ? a.added > b.added : a.name > b.name;
  });
  return out;
}

String path(const String &fileName) {
  return dir() + "/" + fileName;
}

bool remove(const String &fileName) {
  return fileName.length() && fileName != name && SD_MMC.remove(path(fileName));
}

bool rename(const String &fileName, const String &title) {
  String clean = title;
  clean.trim();
  if (clean.endsWith(".wav")) clean.remove(clean.length() - 4);
  const String to = storage::safeName(clean + ".wav");
  if (to.length() <= 4 || fileName == name || !SD_MMC.exists(path(fileName))) return false;
  if (to == fileName) return true;
  if (SD_MMC.exists(path(to))) return false;
  return SD_MMC.rename(path(fileName), path(to));
}

String displayName(const String &fileName) {
  static const char *kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  tm t = {};
  if (sscanf(fileName.c_str(), "%4d%2d%2d-%2d%2d%2d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min,
             &t.tm_sec) == 6 && t.tm_mon >= 1 && t.tm_mon <= 12) {
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    mktime(&t);  // weekday
    char text[32];
    snprintf(text, sizeof(text), "%s %d %s %02d:%02d", kDays[t.tm_wday % 7], t.tm_mday, kMonths[t.tm_mon], t.tm_hour,
             t.tm_min);
    return text;
  }
  String title = fileName;
  if (title.endsWith(".wav")) title.remove(title.length() - 4);
  return title;
}

}  // namespace tape
