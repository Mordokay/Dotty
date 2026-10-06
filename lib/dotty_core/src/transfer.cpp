#include "transfer.h"

#include <SD_MMC.h>
#include <esp_http_server.h>
#include <mbedtls/sha256.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "cartridge.h"
#include "core_ble.h"
#include "log.h"
#include "net.h"
#include "power.h"
#include "shell.h"
#include "storage.h"

namespace transfer {
namespace {

// Ends a session nobody uses (e.g. the app was closed). BLE is paused meanwhile, so the
// phone can't reach Dotty until then: keep it short.
constexpr uint32_t kIdleTimeoutMs = 60 * 1000;
constexpr int kRecvWaitSeconds = 15;
constexpr int kMaxRecvTimeouts = 2;  // 30 s without data aborts an upload
// Receiving and writing the card overlap: the HTTP task fills one block while a writer
// task writes the previous one (writes took ~40 % of the time when done in line).
constexpr size_t kBlock = 16384;
constexpr int kBlocks = 3;

httpd_handle_t server = nullptr;
char token[33] = {};
std::function<void(const Summary &)> finishedCallback;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

// Shared between the HTTP task and the main loop.
volatile uint32_t lastActivity = 0;
volatile size_t currentDone = 0, currentTotal = 0;
volatile int filesReceived = 0;
volatile int filesFailed = 0;
volatile int filesReported = 0;
// Fixed buffers (no heap allocation inside the spinlock), guarded by lock.
char currentFile[128] = {};
char lastDir[128] = {};
char lastName[128] = {};
size_t lastSize = 0;
bool stopRequested = false;
// Whole-card mode (backups, launcher only): ?path= may name any file on the card and
// GET /card/list lists them all.
bool cardScope = false;
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
    if (!writeFailed) {
      const size_t wrote = writeTarget.write(blocks[i], blockLen[i]);
      if (wrote != blockLen[i]) {
        writeFailed = true;
        LOGE("transfer", "card write: %u of %u bytes (errno %d)", static_cast<unsigned>(wrote),
             static_cast<unsigned>(blockLen[i]), errno);
      }
    }
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

bool tokenMatches(httpd_req_t *req) {
  char given[sizeof(token)] = {};
  return httpd_req_get_hdr_value_str(req, "X-Dotty-Token", given, sizeof(given)) == ESP_OK &&
         strcmp(given, token) == 0;
}

void setCurrentFile(const char *name) {
  portENTER_CRITICAL(&lock);
  strlcpy(currentFile, name, sizeof(currentFile));
  portEXIT_CRITICAL(&lock);
}

// Whole-card mode: an absolute path on the card ("/cartridges/music/data/x.mp3"), checked
// so it stays on the card (no "..", no empty segments). False if it isn't usable.
bool cardPath(httpd_req_t *req, String &folder, String &name) {
  if (!cardScope) return false;
  const String path = queryValue(req, "path");
  if (!path.startsWith("/") || path.endsWith("/") || path.indexOf("..") >= 0 || path.indexOf("//") >= 0) return false;
  const int slash = path.lastIndexOf('/');
  folder = slash > 0 ? path.substring(0, slash) : String("/");
  name = path.substring(slash + 1);
  return name.length() > 0;
}

// POST (or PUT) /upload?dir=&name=. Replies {"ok":true,"name":<name as stored>}.
// Phones should POST: iOS silently re-sends a PUT whose connection drops, so a failing
// upload repeats without the app knowing.
esp_err_t handleUpload(httpd_req_t *req) {
  if (!tokenMatches(req)) return reply(req, "403 Forbidden", "{\"ok\":false,\"error\":\"bad token\"}");
  String dir = storage::safeName(queryValue(req, "dir"));
  String name = storage::safeName(queryValue(req, "name"));
  String folder = storage::myDataDir() + (dir.length() ? "/" + dir : "");
  if (cardPath(req, folder, name)) dir = folder;  // a restore puts files back where they were
  if (name.isEmpty()) return reply(req, "400 Bad Request", "{\"ok\":false,\"error\":\"name is required\"}");

  storage::makeDirs(folder);
  const String path = folder + "/" + name;
  File out = SD_MMC.open(path + ".part", FILE_WRITE);
  if (!out) {
    LOGE("transfer", "cannot create %s.part", path.c_str());
    return reply(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"cannot write to the card\"}");
  }

  const size_t total = req->content_len;
  setCurrentFile(name.c_str());
  currentTotal = total;
  currentDone = 0;
  LOGI("transfer", "receiving %s/%s (%u bytes)", dir.c_str(), name.c_str(), static_cast<unsigned>(total));

  writeTarget = out;
  writeFailed = false;
  writingMs = 0;
  size_t received = 0;
  int timeouts = 0;
  bool connectionOk = true;
  const uint32_t started = millis();
  while (received < total && connectionOk && !writeFailed) {
    int i;
    xQueueReceive(emptyBlocks, &i, portMAX_DELAY);
    size_t filled = 0;
    const size_t want = min(total - received, kBlock);
    while (filled < want) {
      const int n = httpd_req_recv(req, reinterpret_cast<char *>(blocks[i] + filled), want - filled);
      if (n == HTTPD_SOCK_ERR_TIMEOUT) {
        // Nothing for recv_wait_timeout seconds: the phone went away (locked, closed, out
        // of range). Give up rather than wait forever with the transfer screen up.
        if (++timeouts >= kMaxRecvTimeouts) {
          connectionOk = false;
          break;
        }
        continue;
      }
      if (n <= 0) {
        connectionOk = false;
        break;
      }
      timeouts = 0;
      filled += n;
      currentDone = received + filled;
      lastActivity = millis();
    }
    blockLen[i] = filled;
    received += filled;
    xQueueSend(fullBlocks, &i, portMAX_DELAY);
  }
  const int flush = kFlush;
  xQueueSend(fullBlocks, &flush, portMAX_DELAY);
  xSemaphoreTake(writerIdle, portMAX_DELAY);  // every block of this file is on the card
  out.close();
  writeTarget = File();
  const bool complete = connectionOk && !writeFailed && received == total;

  const uint32_t elapsed = max<uint32_t>(1, millis() - started);
  LOGI("transfer", "%s: %u of %u KB in %.1f s (%u KB/s), card writes %.1f s", complete ? "done" : "FAILED",
       static_cast<unsigned>(received / 1024), static_cast<unsigned>(total / 1024), elapsed / 1000.0f,
       static_cast<unsigned>(received / elapsed), writingMs / 1000.0f);
  setCurrentFile("");
  currentTotal = 0;
  currentDone = 0;
  lastActivity = millis();

  if (!complete) {
    filesFailed = filesFailed + 1;
    SD_MMC.remove(path + ".part");
    return reply(req, "500 Internal Server Error",
                 writeFailed ? "{\"ok\":false,\"error\":\"writing to the card failed\"}"
                             : "{\"ok\":false,\"error\":\"upload interrupted\"}");
  }
  SD_MMC.remove(path);
  if (!SD_MMC.rename(path + ".part", path)) {
    LOGE("transfer", "cannot rename %s.part", path.c_str());
    SD_MMC.remove(path + ".part");
    return reply(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"cannot write to the card\"}");
  }
  portENTER_CRITICAL(&lock);
  strlcpy(lastDir, dir.c_str(), sizeof(lastDir));
  strlcpy(lastName, name.c_str(), sizeof(lastName));
  lastSize = total;
  portEXIT_CRITICAL(&lock);
  filesReceived = filesReceived + 1;

  JsonDocument doc;
  doc["ok"] = true;
  doc["name"] = name;
  String json;
  serializeJson(doc, json);
  return reply(req, "200 OK", json.c_str());
}

// GET /download?dir=&name=: a file from the cartridge's data folder to the phone (e.g. a
// recording), in 16 KB chunks.
esp_err_t handleDownload(httpd_req_t *req) {
  if (!tokenMatches(req)) return reply(req, "403 Forbidden", "{\"ok\":false,\"error\":\"bad token\"}");
  const String dir = storage::safeName(queryValue(req, "dir"));
  String name = storage::safeName(queryValue(req, "name"));
  String path = storage::myDataDir() + (dir.length() ? "/" + dir : "") + "/" + name;
  String folder;
  if (cardPath(req, folder, name)) path = (folder == "/" ? String() : folder) + "/" + name;
  File in = name.length() ? SD_MMC.open(path) : File();
  if (!in || in.isDirectory()) return reply(req, "404 Not Found", "{\"ok\":false,\"error\":\"no such file\"}");
  const size_t total = in.size();
  setCurrentFile(name.c_str());
  currentTotal = total;
  currentDone = 0;
  LOGI("transfer", "sending %s/%s (%u bytes)", dir.c_str(), name.c_str(), static_cast<unsigned>(total));
  // Chunked: the server sends the headers with the first chunk (the app knows the size
  // from the cartridge's own listing).
  httpd_resp_set_type(req, "application/octet-stream");
  // The server handles one request at a time, so an upload block is free to borrow.
  char *chunk = reinterpret_cast<char *>(blocks[0]);
  constexpr size_t kChunk = kBlock;
  bool ok = true;
  size_t sent = 0;
  while (ok && sent < total) {
    const size_t n = in.read(reinterpret_cast<uint8_t *>(chunk), kChunk);
    if (n == 0) break;
    ok = httpd_resp_send_chunk(req, chunk, n) == ESP_OK;
    sent += n;
    currentDone = sent;
    lastActivity = millis();
  }
  if (ok) ok = httpd_resp_send_chunk(req, nullptr, 0) == ESP_OK;
  in.close();
  setCurrentFile("");
  currentTotal = 0;
  currentDone = 0;
  lastActivity = millis();
  LOGI("transfer", "%s: %u of %u bytes", ok && sent == total ? "sent" : "send FAILED", static_cast<unsigned>(sent),
       static_cast<unsigned>(total));
  return ok ? ESP_OK : ESP_FAIL;
}

// Whole-card mode: GET /card/list[?hash=1] → [{"path": "/…", "size": n, "sha256"?: "…"}, …],
// every file on the card, streamed as it goes (fingerprinting 160 MB takes a while: a
// silent response would look dead to the phone). The SHA-256s let a restore send only what
// changed.
struct CardLister {
  httpd_req_t *req;
  bool hash;
  bool first = true;
  bool ok = true;
};

String fileSha256(File &f) {
  uint8_t *buffer = blocks[0];  // the server handles one request at a time: borrow a block
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  size_t n;
  while ((n = f.read(buffer, kBlock)) > 0) {
    mbedtls_sha256_update(&sha, buffer, n);
    lastActivity = millis();
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return hex;
}

void listCard(const String &folder, CardLister &out) {
  File dir = SD_MMC.open(folder.length() ? folder : String("/"));
  for (File f = dir ? dir.openNextFile() : File(); f && out.ok; f = dir.openNextFile()) {
    const String path = folder + "/" + f.name();
    if (f.isDirectory()) {
      f.close();
      listCard(path, out);
      continue;
    }
    JsonDocument item;
    item["path"] = path;
    item["size"] = f.size();
    if (out.hash) item["sha256"] = fileSha256(f);
    f.close();
    String json;
    serializeJson(item, json);  // into a String it replaces the content, so separately
    const String entry = (out.first ? "" : ",") + json;
    out.first = false;
    out.ok = httpd_resp_send_chunk(out.req, entry.c_str(), entry.length()) == ESP_OK;
    lastActivity = millis();
  }
}

esp_err_t handleCardList(httpd_req_t *req) {
  if (!tokenMatches(req)) return reply(req, "403 Forbidden", "{\"ok\":false,\"error\":\"bad token\"}");
  if (!cardScope) return reply(req, "403 Forbidden", "{\"ok\":false,\"error\":\"not in whole-card mode\"}");
  httpd_resp_set_type(req, "application/json");
  CardLister out{req, queryValue(req, "hash") == "1"};
  httpd_resp_send_chunk(req, "[", 1);
  listCard("", out);
  if (out.ok) httpd_resp_send_chunk(req, "]", 1);
  httpd_resp_send_chunk(req, nullptr, 0);
  return out.ok ? ESP_OK : ESP_FAIL;
}

// Whole-card mode: POST /card/delete?path= removes a file (a restore drops what the backup
// doesn't have).
esp_err_t handleCardDelete(httpd_req_t *req) {
  if (!tokenMatches(req)) return reply(req, "403 Forbidden", "{\"ok\":false,\"error\":\"bad token\"}");
  String folder, name;
  if (!cardPath(req, folder, name)) return reply(req, "400 Bad Request", "{\"ok\":false,\"error\":\"bad path\"}");
  const String path = (folder == "/" ? String() : folder) + "/" + name;
  lastActivity = millis();
  if (!SD_MMC.remove(path)) return reply(req, "404 Not Found", "{\"ok\":false,\"error\":\"no such file\"}");
  LOGI("transfer", "deleted %s", path.c_str());
  return reply(req, "200 OK", "{\"ok\":true}");
}

// POST /done: the phone has sent everything (BLE is paused, so this comes over HTTP).
esp_err_t handleDone(httpd_req_t *req) {
  if (!tokenMatches(req)) return reply(req, "403 Forbidden", "{\"ok\":false,\"error\":\"bad token\"}");
  stopRequested = true;  // handled by poll(), after this reply has gone out
  char json[48];
  snprintf(json, sizeof(json), "{\"ok\":true,\"files\":%d}", static_cast<int>(filesReceived));
  return reply(req, "200 OK", json);
}

// Uploads cut short by a reboot leave <name>.part files behind.
void removePartFiles(const String &folder) {
  File dir = SD_MMC.open(folder);
  for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) {
    const String path = folder + "/" + f.name();
    const bool isDir = f.isDirectory();
    f.close();
    if (isDir) removePartFiles(path);
    else if (path.endsWith(".part")) SD_MMC.remove(path);
  }
}

bool startServer(String &error) {
  removePartFiles(storage::myDataDir());
  if (!startWriter()) {
    stopWriter();
    error = "not enough memory for the transfer";
    return false;
  }
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.stack_size = 8192;
  config.recv_wait_timeout = kRecvWaitSeconds;
  config.lru_purge_enable = true;
  if (httpd_start(&server, &config) != ESP_OK) {
    stopWriter();
    error = "cannot start the transfer server";
    return false;
  }
  httpd_uri_t upload = {};
  upload.uri = "/upload";
  upload.handler = handleUpload;
  upload.method = HTTP_POST;
  httpd_register_uri_handler(server, &upload);
  upload.method = HTTP_PUT;  // older apps
  httpd_register_uri_handler(server, &upload);
  httpd_uri_t download = {};
  download.uri = "/download";
  download.method = HTTP_GET;
  download.handler = handleDownload;
  httpd_register_uri_handler(server, &download);
  httpd_uri_t list = {};
  list.uri = "/card/list";
  list.method = HTTP_GET;
  list.handler = handleCardList;
  httpd_register_uri_handler(server, &list);
  httpd_uri_t remove = {};
  remove.uri = "/card/delete";
  remove.method = HTTP_POST;
  remove.handler = handleCardDelete;
  httpd_register_uri_handler(server, &remove);
  httpd_uri_t done = {};
  done.uri = "/done";
  done.method = HTTP_POST;
  done.handler = handleDone;
  httpd_register_uri_handler(server, &done);
  return true;
}

void stop(bool appFinished) {
  if (!server) return;
  httpd_stop(server);
  server = nullptr;
  stopWriter();
  net::disconnect();
  blePauseAt = 0;
  cardScope = false;
  if (blePaused && !shell::locked()) ble::start();  // locked: the shell restarts it on unlock
  blePaused = false;
  power::setWakeLock(power::kWakeLockNetwork, false);
  Summary summary;
  summary.received = filesReceived;
  summary.failed = filesFailed;
  summary.appFinished = appFinished;
  LOGI("transfer", "stopped (%s): %d files, %d failed", appFinished ? "by the app" : "idle",
       summary.received, summary.failed);
  filesReceived = 0;
  filesFailed = 0;
  filesReported = 0;
  if (finishedCallback) finishedCallback(summary);
}

}  // namespace

void registerCommands(std::function<void(const Summary &)> onFinished) {
  finishedCallback = std::move(onFinished);

  // {scope?: "card"}: whole-card mode (backups and restores; the launcher only).
  ble::on("transfer.start", [](JsonObjectConst args, JsonObject reply) {
    String error;
    const bool wholeCard = strcmp(args["scope"] | "", "card") == 0;
    if (wholeCard && !cartridge::isLauncher()) error = "whole-card transfers run in the launcher";
    else if (!storage::available()) error = "no SD card";
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
    cardScope = wholeCard;
    power::setWakeLock(power::kWakeLockNetwork, true);
    lastActivity = millis();
    reply["url"] = "http://" + net::ip();
    reply["token"] = token;
    reply["ssid"] = net::ssid();
    reply["rssi"] = net::rssi();  // the app suggests a hotspot when this is weak
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
    const bool byApp = stopRequested;
    stopRequested = false;
    stop(byApp);
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
