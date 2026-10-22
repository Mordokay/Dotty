#include "ws_client.h"

#include <esp_crt_bundle.h>
#include <esp_random.h>
#include <esp_tls.h>
#include <lwip/sockets.h>

#include "log.h"

namespace {
constexpr int kTimeoutMs = 15000;
}

bool WsClient::connect(const char *host, const char *path, String &error) {
  close();
  closeCode_ = 0;
  closeReason_.clear();
  decoder_ = WsDecoder();
  esp_tls_cfg_t cfg = {};
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = kTimeoutMs;
  tls_ = esp_tls_init();
  if (!tls_) {
    error = "out of memory";
    return false;
  }
  if (esp_tls_conn_new_sync(host, strlen(host), 443, &cfg, tls_) != 1) {
    error = "cannot reach the server";
    close();
    return false;
  }
  esp_tls_get_conn_sockfd(tls_, &fd_);

  uint8_t raw[16];
  esp_fill_random(raw, sizeof(raw));
  const std::string key = base64Encode(raw, sizeof(raw));
  const std::string request = wsHandshake(host, path, key);
  if (esp_tls_conn_write(tls_, request.data(), request.size()) != static_cast<ssize_t>(request.size())) {
    error = "handshake failed";
    close();
    return false;
  }
  // The response headers, then maybe the first frame bytes.
  std::string response;
  const uint32_t start = millis();
  char buf[512];
  while (response.find("\r\n\r\n") == std::string::npos) {
    if (millis() - start > kTimeoutMs) {
      error = "no answer";
      close();
      return false;
    }
    const int n = esp_tls_conn_read(tls_, buf, sizeof(buf));
    if (n > 0) response.append(buf, n);
    else if (n != ESP_TLS_ERR_SSL_WANT_READ && n != ESP_TLS_ERR_SSL_WANT_WRITE) {
      error = "connection closed";
      close();
      return false;
    }
  }
  const size_t end = response.find("\r\n\r\n") + 4;
  if (!wsCheckAccept(response.substr(0, end), key)) {
    const size_t line = response.find("\r\n");
    error = String("refused: ") + response.substr(0, line).c_str();
    close();
    return false;
  }
  // Leftover bytes are the start of the first frame: keep them for poll().
  if (end < response.size()) {
    size_t used = 0;
    decoder_.feed(reinterpret_cast<const uint8_t *>(response.data() + end), response.size() - end, used);
  }
  // From now on reads must not wait: poll() checks the socket first.
  timeval tv = {0, 50 * 1000};
  setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return true;
}

bool WsClient::sendFrame(uint8_t opcode, const uint8_t *data, size_t len) {
  uint8_t mask[4];
  esp_fill_random(mask, sizeof(mask));
  return writeAll(wsEncode(opcode, data, len, mask), kTimeoutMs);
}

bool WsClient::writeAll(const std::vector<uint8_t> &frame, uint32_t timeoutMs) {
  if (!tls_) return false;
  size_t done = 0;
  const uint32_t start = millis();
  while (done < frame.size()) {
    const ssize_t n = esp_tls_conn_write(tls_, frame.data() + done, frame.size() - done);
    if (n > 0) {
      done += n;
      continue;
    }
    if ((n != ESP_TLS_ERR_SSL_WANT_READ && n != ESP_TLS_ERR_SSL_WANT_WRITE) || millis() - start > timeoutMs) {
      // A send that can't finish means the connection is gone (or memory ran out): drop it, so
      // the caller sees it closed instead of every later send waiting out its own timeout (that
      // stalled the loop until the watchdog reset Dotty).
      LOGW("ws", "send failed after %lu ms (%d): dropping the connection", millis() - start, static_cast<int>(n));
      drop();
      return false;
    }
    delay(1);
  }
  return true;
}

void WsClient::drop() {
  if (!tls_) return;
  esp_tls_conn_destroy(tls_);
  tls_ = nullptr;
  fd_ = -1;
}

bool WsClient::sendText(const char *data, size_t len) {
  return sendFrame(0x1, reinterpret_cast<const uint8_t *>(data), len);
}

bool WsClient::poll(const std::function<void(const std::string &)> &onMessage) {
  if (!tls_) return false;
  static uint8_t buf[4096];
  for (int rounds = 0; rounds < 64; rounds++) {
    // Data decrypted already, or the socket readable?
    if (esp_tls_get_bytes_avail(tls_) <= 0) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(fd_, &set);
      timeval zero = {0, 0};
      if (select(fd_ + 1, &set, nullptr, nullptr, &zero) <= 0) return true;
    }
    const int n = esp_tls_conn_read(tls_, buf, sizeof(buf));
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) return true;
    if (n <= 0) {
      LOGW("ws", "connection closed (%d)", n);
      close();
      return false;
    }
    size_t offset = 0;
    while (offset < static_cast<size_t>(n)) {
      size_t used = 0;
      const WsDecoder::Result r = decoder_.feed(buf + offset, n - offset, used);
      offset += used;
      if (r == WsDecoder::Message) onMessage(decoder_.message);
      else if (r == WsDecoder::Ping) sendFrame(0xA, reinterpret_cast<const uint8_t *>(decoder_.control.data()), decoder_.control.size());
      else if (r == WsDecoder::Close) {
        closeCode_ = decoder_.closeCode;
        closeReason_ = decoder_.control.size() > 2 ? decoder_.control.substr(2) : "";
        LOGW("ws", "server closed: %u %s", closeCode_, decoder_.control.size() > 2 ? decoder_.control.c_str() + 2 : "");
        close();
        return false;
      } else if (r == WsDecoder::Error) {
        LOGW("ws", "bad frame");
        close();
        return false;
      }
      if (used == 0) break;
    }
  }
  return true;
}

void WsClient::close() {
  if (!tls_) return;
  // Say goodbye first: without it Google keeps the session open until it times out, and
  // reconnecting seconds later was refused as over quota (too many open sessions).
  uint8_t mask[4];
  esp_fill_random(mask, sizeof(mask));
  writeAll(wsCloseFrame(1000, mask), 1000);
  drop();
}
