#pragma once

#include <vector>

#include <Arduino.h>

#include <functional>
#include <string>

#include "ws_frame.h"

struct esp_tls;

// A WebSocket client over TLS (esp_tls with ESP-IDF's certificate bundle, port 443), built on
// ws_frame.h. Messages have no size limit (they're assembled in std::string; anything over 4 KB
// lands in PSRAM). poll() never waits long: it reads what has arrived and hands over each
// complete message; pings are answered.
class WsClient {
 public:
  bool connect(const char *host, const char *path, String &error);
  bool sendText(const char *data, size_t len);
  bool sendText(const std::string &s) { return sendText(s.data(), s.size()); }
  // Reads what's there; calls onMessage for each complete message. False once the connection
  // is gone (closed by the server, or an error).
  bool poll(const std::function<void(const std::string &)> &onMessage);
  void close();
  bool connected() const { return tls_ != nullptr; }
  uint16_t closeCode() const { return closeCode_; }
  const std::string &closeReason() const { return closeReason_; }

 private:
  bool sendFrame(uint8_t opcode, const uint8_t *data, size_t len);
  bool writeAll(const std::vector<uint8_t> &frame, uint32_t timeoutMs);
  void drop();  // closes without a goodbye (the connection is already broken)
  esp_tls *tls_ = nullptr;
  int fd_ = -1;
  WsDecoder decoder_;
  uint16_t closeCode_ = 0;
  std::string closeReason_;  // the server's close reason text (e.g. a quota message)
};
