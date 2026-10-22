// uses: cartridges/companion/ws_frame.cpp
// WebSocket frames (RFC 6455) for the companion's own client.
#include <vector>

#include "check.h"
#include "ws_frame.h"

static std::vector<uint8_t> serverFrame(uint8_t opcode, bool fin, const std::string &payload) {
  std::vector<uint8_t> f;
  f.push_back((fin ? 0x80 : 0) | opcode);
  const size_t n = payload.size();
  if (n < 126) {
    f.push_back(static_cast<uint8_t>(n));
  } else if (n < 65536) {
    f.push_back(126);
    f.push_back(n >> 8);
    f.push_back(n & 0xFF);
  } else {
    f.push_back(127);
    for (int i = 7; i >= 0; i--) f.push_back(static_cast<uint8_t>(static_cast<uint64_t>(n) >> (8 * i)));
  }
  f.insert(f.end(), payload.begin(), payload.end());
  return f;
}

int main() {
  CHECK_EQ("ws: accept key (RFC 6455 example)", wsAcceptFor("dGhlIHNhbXBsZSBub25jZQ=="),
           std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
  CHECK("ws: handshake checks the accept header",
        wsCheckAccept("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n",
                      "dGhlIHNhbXBsZSBub25jZQ=="));
  CHECK("ws: a wrong accept is refused",
        !wsCheckAccept("HTTP/1.1 101 Switching Protocols\r\nSec-WebSocket-Accept: nope\r\n\r\n", "dGhlIHNhbXBsZSBub25jZQ=="));
  const std::string hs = wsHandshake("example.com", "/chat?key=1", "dGhlIHNhbXBsZSBub25jZQ==");
  CHECK("ws: handshake request line", hs.rfind("GET /chat?key=1 HTTP/1.1\r\n", 0) == 0);
  CHECK("ws: handshake has the key and ends with a blank line",
        hs.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != std::string::npos &&
            hs.size() >= 4 && hs.compare(hs.size() - 4, 4, "\r\n\r\n") == 0);

  const uint8_t mask[4] = {1, 2, 3, 4};
  const std::vector<uint8_t> hi = wsEncode(0x1, reinterpret_cast<const uint8_t *>("Hi"), 2, mask);
  CHECK("ws: encode masked text", hi == (std::vector<uint8_t>{0x81, 0x82, 1, 2, 3, 4, 'H' ^ 1, 'i' ^ 2}));
  std::vector<uint8_t> big(70000, 'a');
  const std::vector<uint8_t> bigFrame = wsEncode(0x2, big.data(), big.size(), mask);
  CHECK("ws: 70000 bytes use the 64-bit length", bigFrame[1] == (0x80 | 127) && bigFrame.size() == 2 + 8 + 4 + 70000);

  // A text message in two fragments, fed one byte at a time.
  WsDecoder d;
  std::vector<uint8_t> stream = serverFrame(0x1, false, "Hel");
  const std::vector<uint8_t> rest = serverFrame(0x0, true, "lo");
  stream.insert(stream.end(), rest.begin(), rest.end());
  int messages = 0;
  for (size_t i = 0; i < stream.size();) {
    size_t used = 0;
    const WsDecoder::Result r = d.feed(&stream[i], 1, used);
    i += used ? used : 1;
    if (r == WsDecoder::Message) messages++;
  }
  CHECK("ws: fragments reassemble", messages == 1 && d.message == "Hello" && d.opcode == 0x1);

  WsDecoder ping;
  const std::vector<uint8_t> p = serverFrame(0x9, true, "x");
  size_t used = 0;
  CHECK("ws: ping", ping.feed(p.data(), p.size(), used) == WsDecoder::Ping && used == p.size());

  WsDecoder closer;
  const std::vector<uint8_t> c = serverFrame(0x8, true, std::string("\x03\xE8", 2));
  CHECK("ws: close with code 1000", closer.feed(c.data(), c.size(), used) == WsDecoder::Close && closer.closeCode == 1000);

  WsDecoder large;
  const std::string payload(300 * 1024, 'z');
  const std::vector<uint8_t> l = serverFrame(0x2, true, payload);
  size_t offset = 0;
  WsDecoder::Result r = WsDecoder::NeedMore;
  while (offset < l.size() && r == WsDecoder::NeedMore) {
    size_t n = 0;
    r = large.feed(&l[offset], std::min<size_t>(4096, l.size() - offset), n);
    offset += n;
  }
  CHECK("ws: a 300 KB message arrives intact", r == WsDecoder::Message && large.message.size() == payload.size() && large.opcode == 0x2);
  // Closing says so (code 1000), so the server ends the session at once instead of timing out.
  const std::vector<uint8_t> bye = wsCloseFrame(1000, mask);
  CHECK("ws: close frame, code 1000, masked", bye == (std::vector<uint8_t>{0x88, 0x82, 1, 2, 3, 4, 0x03 ^ 1, 0xE8 ^ 2}));
  return failures ? 1 : 0;
}
