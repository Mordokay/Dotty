#include "installer.h"

#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <freertos/stream_buffer.h>
#include <mbedtls/sha256.h>

#include "cartridge.h"
#include "log.h"

namespace installer {
namespace {

constexpr size_t kBufferSize = 64 * 1024;  // > kWindow, so a well-behaved app never overflows
constexpr size_t kChunk = 4096;

StreamBufferHandle_t stream = nullptr;
StaticStreamBuffer_t streamStruct;
uint8_t *streamStorage = nullptr;

Meta current;
esp_ota_handle_t ota = 0;
const esp_partition_t *target = nullptr;
mbedtls_sha256_context sha;
uint8_t iconBuffer[kIconBytes];

volatile bool isActive = false;
volatile bool overflowed = false;
volatile bool writeFailed = false;
volatile size_t iconReceived = 0;
volatile size_t imageWritten = 0;
volatile size_t accepted = 0;  // payload bytes accepted in order
volatile bool resendPending = false;
volatile size_t resendFrom = 0;

size_t iconBytes() {
  return current.hasIcon ? kIconBytes : 0;
}

void writerTask(void *) {
  static uint8_t chunk[kChunk];
  for (;;) {
    const size_t n = xStreamBufferReceive(stream, chunk, sizeof(chunk), portMAX_DELAY);
    if (!isActive || writeFailed) continue;
    size_t offset = 0;
    if (iconReceived < iconBytes()) {
      const size_t take = min(n, iconBytes() - iconReceived);
      memcpy(iconBuffer + iconReceived, chunk, take);
      iconReceived += take;
      offset = take;
    }
    if (offset < n) {
      const size_t len = n - offset;
      if (esp_ota_write(ota, chunk + offset, len) != ESP_OK) {
        LOGE("install", "flash write failed at %u", static_cast<unsigned>(imageWritten));
        writeFailed = true;
        continue;
      }
      mbedtls_sha256_update(&sha, chunk + offset, len);
      imageWritten += len;
    }
  }
}

// Leftovers from an aborted install are discarded by the writer (it drops data while
// no install is active); wait for them to drain. The buffer can't be reset while the
// writer is blocked reading it.
void reset() {
  isActive = false;
  const uint32_t start = millis();
  while (xStreamBufferBytesAvailable(stream) > 0 && millis() - start < 2000) delay(5);
  overflowed = false;
  writeFailed = false;
  iconReceived = 0;
  imageWritten = 0;
  accepted = 0;
  resendPending = false;
}

}  // namespace

void begin() {
  streamStorage = static_cast<uint8_t *>(heap_caps_malloc(kBufferSize + 1, MALLOC_CAP_SPIRAM));
  stream = xStreamBufferCreateStatic(kBufferSize, 1, streamStorage, &streamStruct);
  xTaskCreatePinnedToCore(writerTask, "installer", 6144, nullptr, 3, nullptr, 1);
}

bool start(const Meta &meta, String &error) {
  if (isActive) abort();
  target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  if (!target) {
    error = "no cartridge partition";
    return false;
  }
  if (meta.size == 0 || meta.size > target->size) {
    error = "image too big for the cartridge slot";
    return false;
  }
  // A launcher just switched to is on trial for its first seconds (bootloader rollback),
  // and esp_ota_begin refuses to write while the running app is unconfirmed
  // (ESP_ERR_OTA_ROLLBACK_INVALID_STATE). The app installs right after switching, so the
  // first install used to fail with "flash busy" and the second worked. Being asked to
  // install over Bluetooth is proof enough that this launcher runs: confirm it now.
  cartridge::confirmHealthy();
  // Sequential mode erases each sector just before writing it, so starting is instant.
  const esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
  if (err != ESP_OK) {
    LOGE("install", "esp_ota_begin: %s", esp_err_to_name(err));
    error = String("can't write the cartridge slot (") + esp_err_to_name(err) + ")";
    return false;
  }
  reset();
  current = meta;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  isActive = true;
  LOGI("install", "%s %s, %u bytes", meta.name.c_str(), meta.version.c_str(),
       static_cast<unsigned>(meta.size));
  return true;
}

void feed(const uint8_t *data, size_t len) {
  if (!isActive || len <= kOffsetBytes) return;
  const size_t offset = data[0] | data[1] << 8 | data[2] << 16 | static_cast<size_t>(data[3]) << 24;
  const uint8_t *payload = data + kOffsetBytes;
  size_t n = len - kOffsetBytes;

  if (offset != accepted) {
    // A gap means writes were lost: ask once for a rewind and ignore data until the
    // app comes back to `accepted`. Offsets below it are leftovers from before a rewind.
    if (offset > accepted && !resendPending) {
      resendFrom = accepted;
      resendPending = true;
    }
    return;
  }
  n = min(n, payloadSize() - accepted);
  if (xStreamBufferSend(stream, payload, n, 0) != n) {
    overflowed = true;
    return;
  }
  accepted += n;
}

bool takeResendRequest(size_t &from) {
  if (!resendPending) return false;
  from = resendFrom;
  // Out-of-order writes keep raising it until the app rewinds, so a missed request is
  // repeated; the main loop rate-limits them.
  resendPending = false;
  return true;
}

Result finish(String &error, size_t &missingFrom) {
  if (!isActive) {
    error = "no install in progress";
    return Result::Failed;
  }
  if (accepted < payloadSize()) {
    missingFrom = accepted;
    return Result::Missing;
  }
  const uint32_t start = millis();
  while (imageWritten < current.size && !writeFailed && !overflowed && millis() - start < 15000) {
    delay(20);
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  isActive = false;

  if (overflowed) error = "data arrived faster than flash (buffer overflow)";
  else if (writeFailed) error = "flash write failed";
  else if (imageWritten != current.size) error = "image incomplete";
  else if (memcmp(digest, current.sha256, sizeof(digest)) != 0) error = "SHA-256 mismatch";
  if (error.length()) {
    esp_ota_abort(ota);
    LOGE("install", "failed: %s", error.c_str());
    return Result::Failed;
  }
  if (esp_ota_end(ota) != ESP_OK) {
    error = "not a valid firmware image";
    return Result::Failed;
  }
  if (esp_ota_set_boot_partition(target) != ESP_OK) {
    error = "could not select the new cartridge";
    return Result::Failed;
  }
  LOGI("install", "done, %u bytes verified", static_cast<unsigned>(imageWritten));
  return Result::Done;
}

void abort() {
  if (!isActive) return;
  isActive = false;
  mbedtls_sha256_free(&sha);
  esp_ota_abort(ota);
  LOGW("install", "aborted at %u bytes", static_cast<unsigned>(imageWritten));
}

bool active() {
  return isActive;
}

const Meta &meta() {
  return current;
}

size_t payloadSize() {
  return iconBytes() + current.size;
}

size_t received() {
  return accepted;
}

size_t written() {
  return imageWritten;
}

bool iconReady() {
  return current.hasIcon && iconReceived >= kIconBytes;
}

const uint8_t *icon() {
  return iconBuffer;
}

}  // namespace installer
