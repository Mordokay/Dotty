#include "net.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include "log.h"

namespace net {
namespace {

constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr int kMaxRedirects = 5;

String storedSsid, storedPassword;

// Opens `url`, following redirects; on success the client is positioned at the body.
esp_http_client_handle_t open(const String &url, int64_t &length, String &error) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 15000;
  config.buffer_size = 4096;     // GitHub's redirect Location URLs are long
  config.buffer_size_tx = 2048;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    error = "out of memory";
    return nullptr;
  }
  for (int i = 0; i <= kMaxRedirects; i++) {
    if (esp_http_client_open(client, 0) != ESP_OK) {
      error = "cannot reach the server";
      break;
    }
    length = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
      esp_http_client_set_redirection(client);
      esp_http_client_close(client);
      continue;
    }
    if (status == 200) return client;
    error = "HTTP " + String(status);
    break;
  }
  if (error.isEmpty()) error = "too many redirects";
  esp_http_client_cleanup(client);
  return nullptr;
}

void close(esp_http_client_handle_t client) {
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
}

}  // namespace

void begin() {
  Preferences prefs;
  prefs.begin("wifi", true);
  storedSsid = prefs.getString("ssid", "");
  storedPassword = prefs.getString("password", "");
  prefs.end();
}

bool hasCredentials() {
  return storedSsid.length() > 0;
}

String ssid() {
  return storedSsid;
}

void setCredentials(const String &ssid, const String &password) {
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("password", password);
  prefs.end();
  storedSsid = ssid;
  storedPassword = password;
  LOGI("wifi", "credentials saved for '%s'", ssid.c_str());
}

void forget() {
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  storedSsid = storedPassword = "";
}

bool connect(String &error) {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (!hasCredentials()) {
    error = "no Wi-Fi set up";
    return false;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(storedSsid.c_str(), storedPassword.c_str());
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < kConnectTimeoutMs) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    error = "cannot join '" + storedSsid + "'";
    disconnect();
    return false;
  }
  LOGI("wifi", "connected to '%s', %s, %d dBm", storedSsid.c_str(), WiFi.localIP().toString().c_str(),
       WiFi.RSSI());
  return true;
}

void disconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool connected() {
  return WiFi.status() == WL_CONNECTED;
}

String ip() {
  return WiFi.localIP().toString();
}

int rssi() {
  return WiFi.RSSI();
}

bool getString(const String &url, String &body, String &error) {
  int64_t length = 0;
  esp_http_client_handle_t client = open(url, length, error);
  if (!client) return false;
  body = "";
  char buffer[1024];
  int n;
  while ((n = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) body.concat(buffer, n);
  close(client);
  if (n < 0) error = "download interrupted";
  return n >= 0;
}

bool download(const String &url, Sink sink, Progress progress, String &error) {
  int64_t length = 0;
  esp_http_client_handle_t client = open(url, length, error);
  if (!client) return false;
  static uint8_t buffer[4096];
  size_t done = 0;
  int n;
  bool ok = true;
  while ((n = esp_http_client_read(client, reinterpret_cast<char *>(buffer), sizeof(buffer))) > 0) {
    if (!sink(buffer, n)) {
      ok = false;
      break;
    }
    done += n;
    if (progress) progress(done, length > 0 ? length : 0);
  }
  close(client);
  if (n < 0) {
    error = "download interrupted";
    ok = false;
  }
  return ok;
}

}  // namespace net
