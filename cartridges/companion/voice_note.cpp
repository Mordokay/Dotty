#include "voice_note.h"

#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <unistd.h>

#include <algorithm>
#include <map>

#include "companions.h"
#include "core_ble.h"
#include "log.h"
#include "rec_messages.h"
#include "relay_link.h"
#include "storage.h"
#include "voice.h"

namespace note {
namespace {

constexpr size_t kHeaderBytes = 44, kStreamBytes = 64 * 1024, kWriteBytes = 4096;
constexpr uint32_t kRate = voice::kInRate;  // 16 kHz

StreamBufferHandle_t stream = nullptr;
SemaphoreHandle_t fileLock = nullptr;
TaskHandle_t writer = nullptr;
File file;
volatile uint32_t dataBytes = 0;
std::vector<uint32_t> partStarts;
bool active_ = false, inPart = false, streaming_ = false;
std::string companion_;
RecordingMeta meta;
std::vector<int16_t> pending;  // samples waiting to fill a frame for the phone
// Each companion's list, read once (every read opened and parsed every .json): dropped on any change.
std::map<std::string, std::vector<RecordingMeta>> cache;

void header(uint8_t *h, uint32_t bytes) {
  auto put32 = [&](int at, uint32_t v) { memcpy(h + at, &v, 4); };
  auto put16 = [&](int at, uint16_t v) { memcpy(h + at, &v, 2); };
  memcpy(h, "RIFF", 4);
  put32(4, 36 + bytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 1);
  put16(22, 1);
  put32(24, kRate);
  put32(28, kRate * 2);
  put16(32, 2);
  put16(34, 16);
  memcpy(h + 36, "data", 4);
  put32(40, bytes);
}

void writerLoop(void *) {
  static uint8_t chunk[kWriteBytes];  // card writes through internal RAM
  for (;;) {
    const size_t n = xStreamBufferReceive(stream, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
    if (n == 0) continue;
    xSemaphoreTake(fileLock, portMAX_DELAY);
    if (file) dataBytes = dataBytes + file.write(chunk, n);
    xSemaphoreGive(fileLock);
  }
}

bool ready() {
  if (writer) return true;
  uint8_t *storage = static_cast<uint8_t *>(heap_caps_malloc(kStreamBytes + 1, MALLOC_CAP_SPIRAM));
  static StaticStreamBuffer_t control;
  if (!storage) return false;
  stream = xStreamBufferCreateStatic(kStreamBytes, 1, storage, &control);
  fileLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(writerLoop, "note-card", 6144, nullptr, 3, &writer, 1);
  return stream && fileLock && writer;
}

// Everything on the card and the header saying how much.
void settle() {
  for (int i = 0; i < 300 && xStreamBufferBytesAvailable(stream) > 0; i++) delay(10);
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
  meta.ms = dataBytes / (kRate * 2 / 1000);
}

String dir(const std::string &companion) {
  return companions::folder(companion) + "/recordings";
}

void sendPending(bool all) {
  const size_t per = relay::recordingFrameSamples();
  size_t at = 0;
  while (streaming_ && (pending.size() - at >= per || (all && at < pending.size()))) {
    const size_t n = std::min(per, pending.size() - at);
    if (!relay::sendRecordingFrame(pending.data() + at, n)) {
      LOGW("note", "the phone stopped getting %s: the card copy goes on", meta.name.c_str());
      streaming_ = false;
    }
    at += n;
  }
  pending.erase(pending.begin(), pending.begin() + std::min(at, pending.size()));
  if (!streaming_) pending.clear();
}

}  // namespace

String path(const std::string &companion, const std::string &name, const char *ext) {
  return dir(companion) + "/" + name.c_str() + "." + ext;
}

bool saveMeta(const std::string &companion, const RecordingMeta &m) {
  cache.erase(companion);
  return companions::writeText(path(companion, m.name, "json"), recordingMetaJson(m));
}

bool begin(const std::string &companion, const std::string &title) {
  if (active_ || !storage::available() || !ready()) return false;
  storage::makeDirs(dir(companion));
  companion_ = companion;
  meta = RecordingMeta();
  meta.created = time(nullptr);
  meta.name = recordingName(meta.created);
  meta.title = title.empty() ? "Voice note" : title;
  meta.open = true;  // until finish: a power cut leaves it open, and boot repairs it
  file = SD_MMC.open(path(companion, meta.name, "wav"), FILE_WRITE);
  if (!file) return false;
  uint8_t h[kHeaderBytes];
  header(h, 0);
  file.write(h, sizeof(h));
  dataBytes = 0;
  partStarts.clear();
  pending.clear();
  active_ = true;
  inPart = false;
  saveMeta(companion, meta);
  streaming_ = relay::phoneReady();
  if (streaming_) {
    relay::resetRecording();
    ble::notify(recBeginEvent(meta.name, companion, meta.title));
  }
  LOGI("note", "recording %s \"%s\" for %s%s", meta.name.c_str(), meta.title.c_str(), companion.c_str(),
       streaming_ ? " (to the phone too)" : "");
  return true;
}

void add(const int16_t *pcm, size_t n) {
  if (!active_ || !inPart) return;
  const size_t bytes = n * 2;
  if (xStreamBufferSend(stream, pcm, bytes, 0) != bytes) LOGW("note", "card writes fell behind: a piece lost");
  if (streaming_) {
    pending.insert(pending.end(), pcm, pcm + n);
    sendPending(false);
  }
}

void partStart() {
  if (!active_ || inPart) return;
  partStarts.push_back(static_cast<uint32_t>(dataBytes));
  inPart = true;
  meta.parts = static_cast<int>(partStarts.size());
  if (streaming_) ble::notify(recPartEvent(meta.name, meta.parts));
}

void partEnd() {
  if (!active_ || !inPart) return;
  inPart = false;
  sendPending(true);
  settle();
  saveMeta(companion_, meta);
}

uint32_t undo() {
  if (!active_ || inPart || partStarts.empty()) return 0;
  settle();
  const uint32_t keep = partStarts.back();
  partStarts.pop_back();
  const uint32_t removed = dataBytes - keep;
  xSemaphoreTake(fileLock, portMAX_DELAY);
  file.close();
  if (truncate(("/sdcard" + path(companion_, meta.name, "wav")).c_str(), kHeaderBytes + keep) != 0)
    LOGE("note", "undo: truncate failed (errno %d)", errno);
  dataBytes = keep;
  file = SD_MMC.open(path(companion_, meta.name, "wav"), "r+");
  if (file) file.seek(kHeaderBytes + keep);
  xSemaphoreGive(fileLock);
  settle();
  meta.parts = static_cast<int>(partStarts.size());
  saveMeta(companion_, meta);
  if (streaming_) ble::notify(recUndoEvent(meta.name));
  return removed / (kRate * 2 / 1000);
}

std::string finish(bool keep) {
  if (!active_) return "";
  if (inPart) partEnd();
  settle();
  xSemaphoreTake(fileLock, portMAX_DELAY);
  file.close();
  xSemaphoreGive(fileLock);
  active_ = false;
  const bool kept = keep && dataBytes > 0;  // nothing recorded: the phone drops it too
  if (streaming_) ble::notify(recEndEvent(meta.name, kept));
  meta.open = false;
  if (!kept) {
    remove(companion_, meta.name);
    LOGI("note", "%s discarded", meta.name.c_str());
    return "";
  }
  saveMeta(companion_, meta);
  LOGI("note", "saved %s: %.1f s in %d part(s)%s", meta.name.c_str(), meta.ms / 1000.0f, meta.parts,
       streaming_ ? ", streamed" : ", not on the phone yet");
  return meta.name;
}

bool active() {
  return active_;
}

bool streaming() {
  return streaming_;
}

const RecordingMeta &current() {
  return meta;
}

std::vector<RecordingMeta> list(const std::string &companion) {
  const auto hit = cache.find(companion);
  if (hit != cache.end()) return hit->second;
  std::vector<RecordingMeta> out;
  File d = SD_MMC.open(dir(companion));
  if (!d || !d.isDirectory()) return out;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String name = f.name();
    f.close();
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!name.endsWith(".json") || name.startsWith(".")) continue;
    RecordingMeta m;
    if (parseRecordingMeta(companions::readText(dir(companion) + "/" + name), m) && !m.name.empty()) out.push_back(m);
  }
  d.close();
  std::sort(out.begin(), out.end(), [](const RecordingMeta &a, const RecordingMeta &b) { return a.created > b.created; });
  cache[companion] = out;
  return out;
}

bool remove(const std::string &companion, const std::string &name) {
  cache.erase(companion);
  bool any = false;
  for (const char *ext : {"wav", "json", "txt"}) any = SD_MMC.remove(path(companion, name, ext)) || any;
  return any;
}

void repairAll() {
  for (const std::string &id : companions::list().order()) {
    for (const RecordingMeta &m : list(id)) {
      if (!m.open) continue;  // closed properly: its header is right (only an interrupted one needs a look)
      File f = SD_MMC.open(path(id, m.name, "wav"), "r+");
      if (!f || f.size() < kHeaderBytes) continue;
      uint8_t h[kHeaderBytes];
      f.read(h, sizeof(h));
      uint32_t stated;
      memcpy(&stated, h + 40, 4);
      const HeaderFix fix = headerFix(f.size(), stated);
      RecordingMeta fixed = m;
      fixed.open = false;
      fixed.ms = fix.dataBytes / (kRate * 2 / 1000);
      if (memcmp(h, "RIFF", 4) == 0 && fix.needed) {
        header(h, fix.dataBytes);
        f.seek(0);
        f.write(h, sizeof(h));
      }
      f.close();
      saveMeta(id, fixed);
      LOGI("note", "repaired %s/%s (%.1f s)", id.c_str(), m.name.c_str(), fixed.ms / 1000.0f);
    }
  }
}

}  // namespace note
