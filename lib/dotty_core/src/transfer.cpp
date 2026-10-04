#include "transfer.h"

#include <SD_MMC.h>
#include <esp_http_server.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "core_ble.h"
#include "log.h"
#include "net.h"
#include "power.h"
#include "shell.h"
#include "storage.h"

namespace transfer {
namespace {

constexpr uint32_t kIdleTimeoutMs = 2 * 60 * 1000;
// Receiving and writing the card overlap: the HTTP task fills one block while a writer
// task writes the previous one (writes took ~40 % of the time when done in line).
constexpr size_t kBlock = 16384;
constexpr int kBlocks = 3;

httpd_handle_t server = nullptr;
char token[33] = {};
std::function<void()> finishedCallback;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

// Shared between the HTTP task and the main loop.
volatile uint32_t lastActivity = 0;
volatile size_t currentDone = 0, currentTotal = 0;
volatile int filesReceived = 0;
volatile int filesReported = 0;
// Fixed buffers (no heap allocation inside the spinlock), guarded by lock.
char currentFile[128] = {};
char lastDir[128] = {};
char lastName[128] = {};
size_t lastSize = 0;
bool stopRequested = false;
// Wi-Fi and BLE share the radio: with BLE on, uploads run at about half speed (measured
// 130 vs 240 KB/s). So BLE pauses shortly after transfer.start has been answered and
// comes back when the session ends.
constexpr uint32_t kBlePauseDelayMs = 400;
uint32_t blePauseAt = 0;
bool blePaused = false;

// The card writer. Blocks travel empty → (HTTP task fills) → full → (writer) → empty.
uint8_t *blocks[kBlocks] = {};
size_t blockLen[kBlocks] = {};
QueueHandle_t emptyBlocks = nullptr, fullBlocks = nullptr;
SemaphoreHandle_t writerIdle = nullptr;
TaskHandle_t writerTask = nullptr;
File writeTarget;
volatile bool writeFailed = false;
volatile uint32_t writingMs = 0;
constexpr int kFlush = -1;  // "no more blocks for this file"

void writerLoop(void *) {
  for (;;) {
    int i;
    xQueueReceive(fullBlocks, &i, portMAX_DELAY);
    if (i == kFlush) {
      xSemaphoreGive(writerIdle);
      continue;
    }
    const uint32_t start = millis();
    if (!writeFailed && writeTarget.write(blocks[i], blockLen[i]) != blockLen[i]) writeFailed = true;
    writingMs += millis() - start;
    xQueueSend(emptyBlocks, &i, portMAX_DELAY);
  }
}

bool startWriter() {
  if (writerTask) return true;
  for (int i = 0; i < kBlocks; i++) {
    blocks[i] = static_cast<uint8_t *>(heap_caps_malloc(kBlock, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!blocks[i]) return false;
  }
  emptyBlocks = xQueueCreate(kBlocks, sizeof(int));
  fullBlocks = xQueueCreate(kBlocks + 1, sizeof(int));
  writerIdle = xSemaphoreCreateBinary();
  for (int i = 0; i < kBlocks; i++) xQueueSend(emptyBlocks, &i, 0);
  return xTaskCreate(writerLoop, "card-writer", 4096, nullptr, 5, &writerTask) == pdPASS;
}

void stopWriter() {
  if (writerTask) vTaskDelete(writerTask);
  writerTask = nullptr;
  if (emptyBlocks) vQueueDelete(emptyBlocks);
  if (fullBlocks) vQueueDelete(fullBlocks);
  if (writerIdle) vSemaphoreDelete(writerIdle);
  emptyBlocks = fullBlocks = nullptr;
  writerIdle = nullptr;
  for (auto &b : blocks) {
    free(b);
    b = nullptr;
  }
}

String urlDecode(const char *in) {
  String out;
  for (const char *p = in; *p; p++) {
    if (*p == '+') {
      out += ' ';
    } else if (*p == '%' && isxdigit(p[1]) && isxdigit(p[2])) {
      const char hex[3] = {p[1], p[2], 0};
      out += static_cast<char>(strtol(hex, nullptr, 16));
      p += 2;
    } else {
      out += *p;
    }
  }
  return out;
}

String queryValue(httpd_req_t *req, const char *key) {
  const size_t len = httpd_req_get_url_query_len(req) + 1;
  if (len <= 1) return "";
  String result;
  char *query = static_cast<char *>(malloc(len));
  char *value = static_cast<char *>(malloc(len));
  if (query && value && httpd_req_get_url_query_str(req, query, len) == ESP_OK &&
      httpd_query_key_value(query, key, value, len) == ESP_OK) {
    result = urlDecode(value);
  }
  free(query);
  free(value);
  return result;
}

esp_err_t reply(httpd_req_t *req, const char *status, const char *json) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, json);
}

esp_err_t handleUpload(httpd_req_t *req) {
  char given[sizeof(token)] = {};
  if (httpd_req_get_hdr_value_str(req, "X-Dotty-Token", given, sizeof(given)) != ESP_OK ||
      strcmp(given, token) != 0) {
    return reply(req, "403 Forbidden", "{\"ok\":false,\"error\":\"bad token\"}");
  }
  const String dir = storage::safeName(queryValue(req, "dir"));
  const String name = storage::safeName(queryValue(req, "name"));
  if (name.isEmpty()) return reply(req, "400 Bad Request", "{\"ok\":false,\"error\":\"name is required\"}");

  const String folder = storage::myDataDir() + (dir.length() ? "/" + dir : "");
  storage::makeDirs(folder);
  const String path = folder + "/" + name;
  File out = SD_MMC.open(path + ".part", FILE_WRITE);
  if (!out) return reply(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"cannot write to the card\"}");

  portENTER_CRITICAL(&lock);
  strlcpy(currentFile, name.c_str(), sizeof(currentFile));
  portEXIT_CRITICAL(&lock);
  currentTotal = req->content_len;
  currentDone = 0;
  LOGI("transfer", "receiving %s/%s (%u bytes)", dir.c_str(), name.c_str(), static_cast<unsigned>(req->content_len));

  writeTarget = out;
  writeFailed = false;
  writingMs = 0;
  size_t remaining = req->content_len;
  bool ok = true;
  const uint32_t started = millis();
  while (remaining > 0 && ok && !writeFailed) {
    int i;
    xQueueReceive(emptyBlocks, &i, portMAX_DELAY);
    size_t filled = 0;
    const size_t want = min(remaining, kBlock);
    while (filled < want) {
      const int n = httpd_req_recv(req, reinterpret_cast<char *>(blocks[i] + filled), want - filled);
      if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
      if (n <= 0) {
        ok = false;
        break;
      }
      filled += n;
      currentDone = req->content_len - remaining + filled;
      lastActivity = millis();
    }
    blockLen[i] = filled;
    remaining -= filled;
    xQueueSend(fullBlocks, &i, portMAX_DELAY);
  }
  const int flush = kFlush;
  xQueueSend(fullBlocks, &flush, portMAX_DELAY);
  xSemaphoreTake(writerIdle, portMAX_DELAY);  // every block of this file is on the card
  ok = ok && !writeFailed;
  out.close();
  writeTarget = File();
  const uint32_t elapsed = max<uint32_t>(1, millis() - started);
  LOGI("transfer", "%u KB in %.1f s (%u KB/s), %.1f s of it writing the card",
       static_cast<unsigned>(req->content_len / 1024), elapsed / 1000.0f,
       static_cast<unsigned>(req->content_len / elapsed), writingMs / 1000.0f);  // writing overlaps receiving

  portENTER_CRITICAL(&lock);
  currentFile[0] = '\0';
  portEXIT_CRITICAL(&lock);
  if (!ok) {
    SD_MMC.remove(path + ".part");
    return reply(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"upload interrupted\"}");
  }
  SD_MMC.remove(path);
  SD_MMC.rename(path + ".part", path);
  portENTER_CRITICAL(&lock);
  strlcpy(lastDir, dir.c_str(), sizeof(lastDir));
  strlcpy(lastName, name.c_str(), sizeof(lastName));
  lastSize = req->content_len;
  portEXIT_CRITICAL(&lock);
  filesReceived = filesReceived + 1;
  lastActivity = millis();
  return reply(req, "200 OK", "{\"ok\":true}");
}

// POST /done: the phone has sent everything (BLE is paused, so this comes over HTTP).
esp_err_t handleDone(httpd_req_t *req) {
  char given[sizeof(token)] = {};
  if (httpd_req_get_hdr_value_str(req, "X-Dotty-Token", given, sizeof(given)) != ESP_OK ||
      strcmp(given, token) != 0) {
    return reply(req, "403 Forbidden", "{\"ok\":false,\"error\":\"bad token\"}");
  }
  stopRequested = true;  // handled by poll(), after this reply has gone out
  char json[48];
  snprintf(json, sizeof(json), "{\"ok\":true,\"files\":%d}", static_cast<int>(filesReceived));
  return reply(req, "200 OK", json);
}

bool startServer(String &error) {
  if (!startWriter()) {
    stopWriter();
    error = "not enough memory for the transfer";
    return false;
  }
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.stack_size = 8192;
  config.recv_wait_timeout = 15;
  config.lru_purge_enable = true;
  if (httpd_start(&server, &config) != ESP_OK) {
    stopWriter();
    error = "cannot start the transfer server";
    return false;
  }
  httpd_uri_t upload = {};
  upload.uri = "/upload";
  upload.method = HTTP_PUT;
  upload.handler = handleUpload;
  httpd_register_uri_handler(server, &upload);
  httpd_uri_t done = {};
  done.uri = "/done";
  done.method = HTTP_POST;
  done.handler = handleDone;
  httpd_register_uri_handler(server, &done);
  return true;
}

void stop() {
  if (!server) return;
  httpd_stop(server);
  server = nullptr;
  stopWriter();
  net::disconnect();
  blePauseAt = 0;
  if (blePaused && !shell::locked()) ble::start();  // locked: the shell restarts it on unlock
  blePaused = false;
  power::setWakeLock(power::kWakeLockNetwork, false);
  LOGI("transfer", "stopped after %d files", static_cast<int>(filesReceived));
  if (finishedCallback) finishedCallback();
}

}  // namespace

void registerCommands(std::function<void()> onFinished) {
  finishedCallback = std::move(onFinished);

  ble::on("transfer.start", [](JsonObjectConst, JsonObject reply) {
    String error;
    if (!storage::available()) error = "no SD card";
    else if (!server && !net::connect(error)) {
    } else if (!server && !startServer(error)) {
      net::disconnect();
    }
    if (error.length()) {
      reply["ok"] = false;
      reply["error"] = error;
      return;
    }
    if (token[0] == 0 || filesReceived == 0) {
      for (int i = 0; i < 16; i++) snprintf(token + 2 * i, 3, "%02x", static_cast<unsigned>(esp_random() & 0xFF));
    }
    power::setWakeLock(power::kWakeLockNetwork, true);
    lastActivity = millis();
    reply["url"] = "http://" + net::ip();
    reply["token"] = token;
    reply["ssid"] = net::ssid();
    reply["bluetooth"] = "paused";  // the app finishes with POST /done and reconnects
    blePauseAt = millis() + kBlePauseDelayMs;
    LOGI("transfer", "ready at %s", net::ip().c_str());
  });

  ble::on("transfer.stop", [](JsonObjectConst, JsonObject reply) {
    reply["files"] = static_cast<int>(filesReceived);
    stopRequested = true;  // stopped from poll(), after this reply has gone out
  });
}

void poll() {
  if (!server) return;
  if (blePauseAt && millis() >= blePauseAt) {
    blePauseAt = 0;
    blePaused = ble::running();
    if (blePaused) ble::stop();
  }
  if (filesReported != filesReceived) {
    filesReported = filesReceived;
    char dir[sizeof(lastDir)], name[sizeof(lastName)];
    portENTER_CRITICAL(&lock);
    memcpy(dir, lastDir, sizeof(dir));
    memcpy(name, lastName, sizeof(name));
    const size_t size = lastSize;
    portEXIT_CRITICAL(&lock);
    JsonDocument event;
    event["event"] = "transfer.received";
    event["dir"] = dir;
    event["name"] = name;
    event["size"] = size;
    ble::notify(event);
  }
  const bool idle = currentTotal == 0 || currentDone >= currentTotal;
  if (stopRequested || (idle && millis() - lastActivity > kIdleTimeoutMs)) {
    stopRequested = false;
    filesReceived = 0;
    filesReported = 0;
    stop();
  }
}

bool active() {
  return server != nullptr;
}

Status status() {
  Status s;
  s.active = server != nullptr;
  char file[sizeof(currentFile)];
  portENTER_CRITICAL(&lock);
  memcpy(file, currentFile, sizeof(file));
  portEXIT_CRITICAL(&lock);
  s.file = file;
  s.done = currentDone;
  s.total = currentTotal;
  s.filesReceived = filesReceived;
  return s;
}

}  // namespace transfer
