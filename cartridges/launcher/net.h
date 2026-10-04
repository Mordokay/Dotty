#pragma once

#include <Arduino.h>

#include <functional>

// Wi-Fi + HTTPS for the launcher. Credentials live in NVS (namespace "wifi"), shared by
// every firmware. HTTPS checks certificates against ESP-IDF's built-in CA bundle and
// follows redirects (GitHub release downloads redirect to a storage host).
// Wi-Fi is switched on only for a fetch and off again afterwards.
namespace net {

void begin();
bool hasCredentials();
String ssid();
void setCredentials(const String &ssid, const String &password);
void forget();

bool connect(String &error);
void disconnect();
bool connected();
String ip();
int rssi();

bool getString(const String &url, String &body, String &error);

// Streams a response body into `sink` (return false to stop). `progress(done, total)`;
// total is 0 when the server doesn't say.
using Sink = std::function<bool(const uint8_t *data, size_t len)>;
using Progress = std::function<void(size_t done, size_t total)>;
bool download(const String &url, Sink sink, Progress progress, String &error);

}  // namespace net
