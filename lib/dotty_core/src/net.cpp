#include "net.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <algorithm>

#include "core_ble.h"
#include "log.h"

namespace net {
namespace {

constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr int kMaxRedirects = 5;

struct Saved {
  String ssid, password;
};
std::vector<Saved> networks;
String preferredSsid;
String lastJoinedSsid;
int lastJoinedRssi = 0;

void load() {
  networks.clear();
  Preferences prefs;
  prefs.begin("wifi", true);
  const String json = prefs.getString("networks", "[]");
  prefs.end();
  JsonDocument doc;
  if (deserializeJson(doc, json) == DeserializationError::Ok) {
    for (JsonObjectConst n : doc.as<JsonArrayConst>()) networks.push_back({n["ssid"] | "", n["password"] | ""});
  }
  prefs.begin("wifi", true);
  preferredSsid = prefs.getString("preferred", "");
  lastJoinedSsid = prefs.getString("last", "");
  lastJoinedRssi = prefs.getInt("lastRssi", 0);
  prefs.end();
}

void putString(const char *key, const String &value) {
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.putString(key, value);
  prefs.end();
}

void store() {
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  for (const Saved &n : networks) {
    JsonObject o = list.add<JsonObject>();
    o["ssid"] = n.ssid;
    o["password"] = n.password;
  }
  String json;
  serializeJson(doc, json);
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.putString("networks", json);
  prefs.end();
}

const Saved *findSaved(const String &ssid) {
  for (const Saved &n : networks) {
    if (n.ssid == ssid) return &n;
  }
  return nullptr;
}

bool join(const String &ssid, const String &password, String &error) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < kConnectTimeoutMs) delay(100);
  if (WiFi.status() == WL_CONNECTED) {
    LOGI("wifi", "joined '%s', %s, %d dBm", ssid.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    lastJoinedRssi = WiFi.RSSI();
    if (ssid != lastJoinedSsid) {
      lastJoinedSsid = ssid;
      putString("last", ssid);
    }
    Preferences prefs;  // the signal changes every time; one small NVS write per join
    prefs.begin("wifi", false);
    prefs.putInt("lastRssi", lastJoinedRssi);
    prefs.end();
    return true;
  }
  error = WiFi.status() == WL_CONNECT_FAILED ? "wrong password?" : "cannot join '" + ssid + "'";
  disconnect();
  return false;
}

// Opens `url`, following redirects; on success the client is positioned at the body.
esp_http_client_handle_t open(const String &url, int64_t &length, String &error) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 15000;
  config.buffer_size = 4096;  // GitHub's redirect Location URLs are long
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
  load();
  LOGI("wifi", "%u saved networks", static_cast<unsigned>(networks.size()));
}

std::vector<String> saved() {
  std::vector<String> names;
  for (const Saved &n : networks) names.push_back(n.ssid);
  return names;
}

bool remember(const String &ssid, const String &password) {
  for (Saved &n : networks) {
    if (n.ssid == ssid) {
      n.password = password;
      store();
      return true;
    }
  }
  if (networks.size() >= kMaxNetworks) return false;
  networks.push_back({ssid, password});
  store();
  LOGI("wifi", "saved '%s'", ssid.c_str());
  return true;
}

void forget(const String &ssid) {
  networks.erase(std::remove_if(networks.begin(), networks.end(),
                                [&](const Saved &n) { return n.ssid == ssid; }),
                 networks.end());
  store();
  if (preferredSsid == ssid) setPreferred("");
}

String preferred() {
  return preferredSsid;
}

void setPreferred(const String &ssid) {
  preferredSsid = ssid;
  putString("preferred", ssid);
  LOGI("wifi", "preferred network: %s", ssid.length() ? ssid.c_str() : "automatic");
}

String lastSsid() {
  return lastJoinedSsid;
}

int lastRssi() {
  return lastJoinedRssi;
}

std::vector<Network> scan() {
  const bool wasOff = WiFi.getMode() == WIFI_OFF;
  WiFi.mode(WIFI_STA);
  const int count = WiFi.scanNetworks();
  std::vector<Network> found;
  for (int i = 0; i < count; i++) {
    const String name = WiFi.SSID(i);
    if (name.isEmpty()) continue;  // hidden network
    auto same = std::find_if(found.begin(), found.end(), [&](const Network &n) { return n.ssid == name; });
    if (same != found.end()) {
      same->rssi = max(same->rssi, static_cast<int>(WiFi.RSSI(i)));
      continue;
    }
    found.push_back({name, WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN, findSaved(name) != nullptr});
  }
  WiFi.scanDelete();
  if (wasOff) disconnect();
  std::sort(found.begin(), found.end(), [](const Network &a, const Network &b) { return a.rssi > b.rssi; });
  return found;
}

bool connectTo(const String &ssid, const String &password, String &error) {
  disconnect();
  return join(ssid, password, error);
}

bool connect(String &error) {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (networks.empty()) {
    error = "no Wi-Fi set up";
    return false;
  }
  std::vector<Network> visible = scan();  // strongest first
  // The preferred network goes first when it's in range; the rest stay strongest first.
  std::stable_partition(visible.begin(), visible.end(), [](const Network &n) { return n.ssid == preferredSsid; });
  for (const Network &n : visible) {
    const Saved *s = findSaved(n.ssid);
    if (s && join(s->ssid, s->password, error)) return true;
  }
  if (error.isEmpty()) error = "no saved network in range";
  return false;
}

void disconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool connected() {
  return WiFi.status() == WL_CONNECTED;
}

String ssid() {
  return connected() ? WiFi.SSID() : String();
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

void registerCommands() {
  begin();

  // Visible networks for the app's picker: {ssid, rssi, secure, known}.
  ble::on("wifi.scan", [](JsonObjectConst, JsonObject reply) {
    JsonArray list = reply["networks"].to<JsonArray>();
    for (const Network &n : scan()) {
      JsonObject o = list.add<JsonObject>();
      o["ssid"] = n.ssid;
      o["rssi"] = n.rssi;
      o["secure"] = n.secure;
      o["known"] = n.known;
    }
  });

  // {ssid, password}: joins it to check the password, and saves it only if that works.
  ble::on("wifi.add", [](JsonObjectConst args, JsonObject reply) {
    const String name = args["ssid"] | "";
    const String password = args["password"] | "";
    String error;
    if (name.isEmpty()) error = "ssid is required";
    else if (!connectTo(name, password, error)) {
    } else if (!remember(name, password)) error = "Dotty already knows 8 networks";
    if (error.isEmpty()) reply["ip"] = ip();
    disconnect();
    if (error.length()) {
      reply["ok"] = false;
      reply["error"] = error;
    }
  });

  // Saved networks, the preferred one ("" = automatic), the last one joined and, while Wi-Fi
  // is on, the current one. No radio use: cheap enough for the dashboard.
  ble::on("wifi.list", [](JsonObjectConst, JsonObject reply) {
    JsonArray list = reply["networks"].to<JsonArray>();
    for (const String &name : saved()) list.add(name);
    reply["preferred"] = preferredSsid;
    if (lastJoinedSsid.length()) {
      reply["last"]["ssid"] = lastJoinedSsid;
      reply["last"]["rssi"] = lastJoinedRssi;
    }
    if (connected()) {
      reply["current"]["ssid"] = ssid();
      reply["current"]["rssi"] = rssi();
    }
  });

  // {ssid}: join this saved network first whenever it's in range; "" = automatic.
  ble::on("wifi.prefer", [](JsonObjectConst args, JsonObject reply) {
    const String name = args["ssid"] | "";
    if (name.length() && !findSaved(name)) {
      reply["ok"] = false;
      reply["error"] = "Dotty doesn't know that network";
      return;
    }
    setPreferred(name);
  });

  ble::on("wifi.remove", [](JsonObjectConst args, JsonObject) { forget(args["ssid"] | ""); });

  // Joins the best saved network and reports it (then switches Wi-Fi off again).
  ble::on("wifi.status", [](JsonObjectConst, JsonObject reply) {
    reply["saved"] = networks.size();
    String error;
    if (connect(error)) {
      reply["ssid"] = ssid();
      reply["rssi"] = rssi();
      reply["ip"] = ip();
    } else {
      reply["error"] = error;
    }
    disconnect();
  });
}

}  // namespace net
