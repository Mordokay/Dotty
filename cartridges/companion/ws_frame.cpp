#include "ws_frame.h"

#include <string.h>

namespace {

// SHA-1 (only for the handshake's Sec-WebSocket-Accept).
void sha1(const uint8_t *msg, size_t len, uint8_t out[20]) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::vector<uint8_t> m(msg, msg + len);
  const uint64_t bits = static_cast<uint64_t>(len) * 8;
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 7; i >= 0; i--) m.push_back(static_cast<uint8_t>(bits >> (8 * i)));
  auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
  for (size_t chunk = 0; chunk < m.size(); chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
      w[i] = static_cast<uint32_t>(m[chunk + 4 * i]) << 24 | m[chunk + 4 * i + 1] << 16 | m[chunk + 4 * i + 2] << 8 |
             m[chunk + 4 * i + 3];
    }
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
      else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
      else f = b ^ c ^ d, k = 0xCA62C1D6;
      const uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
  }
  for (int i = 0; i < 5; i++) {
    for (int j = 0; j < 4; j++) out[4 * i + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
  }
}

}  // namespace

std::string base64Encode(const uint8_t *data, size_t len) {
  static const char *kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t v = data[i] << 16 | (i + 1 < len ? data[i + 1] << 8 : 0) | (i + 2 < len ? data[i + 2] : 0);
    out += kChars[v >> 18 & 63];
    out += kChars[v >> 12 & 63];
    out += i + 1 < len ? kChars[v >> 6 & 63] : '=';
    out += i + 2 < len ? kChars[v & 63] : '=';
  }
  return out;
}

std::string wsHandshake(const std::string &host, const std::string &path, const std::string &key) {
  return "GET " + path + " HTTP/1.1\r\nHost: " + host +
         "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
         "\r\nSec-WebSocket-Version: 13\r\n\r\n";
}

std::string wsAcceptFor(const std::string &key) {
  const std::string s = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t digest[20];
  sha1(reinterpret_cast<const uint8_t *>(s.data()), s.size(), digest);
  return base64Encode(digest, sizeof(digest));
}

bool wsCheckAccept(const std::string &headers, const std::string &key) {
  if (headers.compare(0, 12, "HTTP/1.1 101") != 0) return false;
  std::string lower = headers;
  for (char &c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  const size_t at = lower.find("sec-websocket-accept:");
  if (at == std::string::npos) return false;
  size_t start = at + 21;
  while (start < headers.size() && headers[start] == ' ') start++;
  const size_t end = headers.find("\r\n", start);
  return headers.substr(start, end == std::string::npos ? std::string::npos : end - start) == wsAcceptFor(key);
}

std::vector<uint8_t> wsEncode(uint8_t opcode, const uint8_t *data, size_t len, const uint8_t mask[4]) {
  std::vector<uint8_t> f;
  f.reserve(len + 14);
  f.push_back(0x80 | opcode);
  if (len < 126) {
    f.push_back(0x80 | static_cast<uint8_t>(len));
  } else if (len < 65536) {
    f.push_back(0x80 | 126);
    f.push_back(static_cast<uint8_t>(len >> 8));
    f.push_back(static_cast<uint8_t>(len));
  } else {
    f.push_back(0x80 | 127);
    for (int i = 7; i >= 0; i--) f.push_back(static_cast<uint8_t>(static_cast<uint64_t>(len) >> (8 * i)));
  }
  f.insert(f.end(), mask, mask + 4);
  for (size_t i = 0; i < len; i++) f.push_back(data[i] ^ mask[i % 4]);
  return f;
}

WsDecoder::Result WsDecoder::feed(const uint8_t *data, size_t len, size_t &used) {
  used = 0;
  while (used < len) {
    if (!inPayload_) {
      header_[headerLen_++] = data[used++];
      if (headerLen_ == 2) {
        const uint8_t n = header_[1] & 0x7F;
        if (header_[1] & 0x80) return Error;  // servers never mask
        headerNeed_ = 2 + (n == 126 ? 2 : n == 127 ? 8 : 0);
      }
      if (headerLen_ < headerNeed_) continue;
      fin_ = header_[0] & 0x80;
      frameOpcode_ = header_[0] & 0x0F;
      const uint8_t n = header_[1] & 0x7F;
      payloadLeft_ = n;
      if (n == 126) payloadLeft_ = header_[2] << 8 | header_[3];
      if (n == 127) {
        payloadLeft_ = 0;
        for (int i = 0; i < 8; i++) payloadLeft_ = payloadLeft_ << 8 | header_[2 + i];
      }
      headerLen_ = 0;
      headerNeed_ = 2;
      inPayload_ = true;
      if (frameOpcode_ >= 0x8) control.clear();
      else if (frameOpcode_ != 0x0) {  // a new message (0 continues one)
        partial_.clear();
        opcode = frameOpcode_;
        building_ = true;
      }
      if (frameOpcode_ < 0x8 && payloadLeft_ > partial_.capacity() - partial_.size()) {
        partial_.reserve(partial_.size() + payloadLeft_);
      }
    }
    // Payload bytes.
    const size_t take = static_cast<size_t>(payloadLeft_ < len - used ? payloadLeft_ : len - used);
    if (frameOpcode_ >= 0x8) control.append(reinterpret_cast<const char *>(data + used), take);
    else partial_.append(reinterpret_cast<const char *>(data + used), take);
    used += take;
    payloadLeft_ -= take;
    if (payloadLeft_ > 0) continue;
    inPayload_ = false;
    if (frameOpcode_ == 0x9) return Ping;
    if (frameOpcode_ == 0x8) {
      closeCode = control.size() >= 2 ? static_cast<uint8_t>(control[0]) << 8 | static_cast<uint8_t>(control[1]) : 0;
      return Close;
    }
    if (frameOpcode_ == 0xA) continue;  // pong
    if (fin_ && building_) {
      message.swap(partial_);
      partial_.clear();
      building_ = false;
      return Message;
    }
  }
  return NeedMore;
}

std::vector<uint8_t> wsCloseFrame(uint16_t code, const uint8_t mask[4]) {
  const uint8_t payload[2] = {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code & 0xFF)};
  return wsEncode(0x8, payload, sizeof(payload), mask);
}
