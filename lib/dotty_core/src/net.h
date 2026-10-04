#pragma once

#include <Arduino.h>

#include <functional>
#include <vector>

// Wi-Fi + HTTPS, shared by every firmware.
//
// Dotty remembers up to kMaxNetworks networks in NVS (namespace "wifi", kept across
// cartridges and reboots). Whenever it needs the internet it scans and joins the
// preferred network if one is set and in range, otherwise the strongest one it knows.
// Wi-Fi is switched on only while something needs it.
// HTTPS checks certificates against ESP-IDF's built-in CA bundle and follows redirects
// (GitHub release downloads redirect to a storage host).
namespace net {

constexpr size_t kMaxNetworks = 8;

struct Network {
  String ssid;
  int rssi = 0;         // dBm, from the last scan
  bool secure = false;  // needs a password
  bool known = false;   // Dotty has its password
};

void begin();

// Saved networks (passwords never leave the device).
std::vector<String> saved();
bool remember(const String &ssid, const String &password);  // false if the list is full
void forget(const String &ssid);

// The network to join first when it's in range ("" = automatic: the strongest).
String preferred();
void setPreferred(const String &ssid);

// The last network Dotty joined and its signal then (kept across reboots; "" if none).
String lastSsid();
int lastRssi();

// Visible networks, strongest first, one entry per name.
std::vector<Network> scan();

// Joins one network (to test new credentials).
bool connectTo(const String &ssid, const String &password, String &error);
// Joins the strongest saved network in range.
bool connect(String &error);
void disconnect();
bool connected();
String ssid();
String ip();
int rssi();

bool getString(const String &url, String &body, String &error);

// Streams a response body into `sink` (return false to stop). `progress(done, total)`;
// total is 0 when the server doesn't say.
using Sink = std::function<bool(const uint8_t *data, size_t len)>;
using Progress = std::function<void(size_t done, size_t total)>;
bool download(const String &url, Sink sink, Progress progress, String &error);

// BLE commands: wifi.scan, wifi.add, wifi.list, wifi.remove, wifi.prefer, wifi.status.
void registerCommands();

}  // namespace net
