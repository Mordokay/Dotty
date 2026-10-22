#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

// WebSocket (RFC 6455) pieces for the companion's own client (ws_client.h), with no I/O so they
// can be tested on a computer (tools/companion_test). Client frames are masked; server frames
// aren't. Messages have no size limit (Gemini's audio replies run to hundreds of KB).

// The opening request: GET path, Host, Upgrade, Sec-WebSocket-Key = key (16 random bytes, base64).
std::string wsHandshake(const std::string &host, const std::string &path, const std::string &key);
// The Sec-WebSocket-Accept value the server must answer for `key`.
std::string wsAcceptFor(const std::string &key);
// True if the response headers are a 101 with the right Sec-WebSocket-Accept.
bool wsCheckAccept(const std::string &responseHeaders, const std::string &key);
// One frame: FIN set, the payload masked with `mask`.
std::vector<uint8_t> wsEncode(uint8_t opcode, const uint8_t *data, size_t len, const uint8_t mask[4]);
// A close frame with `code` (1000 = normal).
std::vector<uint8_t> wsCloseFrame(uint16_t code, const uint8_t mask[4]);
std::string base64Encode(const uint8_t *data, size_t len);

// Reads server frames from a byte stream, any number of bytes at a time. feed() consumes bytes
// (`used`) up to the end of a complete message or control frame and says what it got.
struct WsDecoder {
  enum Result { NeedMore, Message, Ping, Close, Error };
  Result feed(const uint8_t *data, size_t len, size_t &used);

  std::string message;     // the last complete message (text or binary, fragments joined)
  uint8_t opcode = 0;      // its opcode: 1 text, 2 binary
  std::string control;     // a ping's payload (to echo in the pong)
  uint16_t closeCode = 0;  // a close frame's code

 private:
  uint8_t header_[14];
  size_t headerLen_ = 0, headerNeed_ = 2;
  uint64_t payloadLeft_ = 0;
  uint8_t frameOpcode_ = 0;
  bool fin_ = false, inPayload_ = false, building_ = false;
  std::string partial_;    // the message being assembled
};
