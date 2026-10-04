#include "transfer.h"

#include <SD_MMC.h>
#include <esp_http_server.h>

#include "core_ble.h"
#include "log.h"
#include "net.h"
#include "power.h"
#include "storage.h"

namespace transfer {
namespace {

constexpr uint32_t kIdleTimeoutMs = 2 * 60 * 1000;
constexpr size_t kChunk = 8192;

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

  static uint8_t buffer[kChunk];
  size_t remaining = req->content_len;
  bool ok = true;
  while (remaining > 0) {
    const int n = httpd_req_recv(req, reinterpret_cast<char *>(buffer), min(remaining, kChunk));
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0 || out.write(buffer, n) != static_cast<size_t>(n)) {
      ok = false;
      break;
    }
    remaining -= n;
    currentDone = req->content_len - remaining;
    lastActivity = millis();
  }
  out.close();

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

bool startServer(String &error) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.stack_size = 8192;
  config.recv_wait_timeout = 15;
  config.lru_purge_enable = true;
  if (httpd_start(&server, &config) != ESP_OK) {
    error = "cannot start the transfer server";
    return false;
  }
  httpd_uri_t upload = {};
  upload.uri = "/upload";
  upload.method = HTTP_PUT;
  upload.handler = handleUpload;
  httpd_register_uri_handler(server, &upload);
  return true;
}

void stop() {
  if (!server) return;
  httpd_stop(server);
  server = nullptr;
  net::disconnect();
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
    LOGI("transfer", "ready at %s", net::ip().c_str());
  });

  ble::on("transfer.stop", [](JsonObjectConst, JsonObject reply) {
    reply["files"] = static_cast<int>(filesReceived);
    stopRequested = true;  // stopped from poll(), after this reply has gone out
  });
}

void poll() {
  if (!server) return;
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
