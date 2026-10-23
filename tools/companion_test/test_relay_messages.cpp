// uses: cartridges/companion/relay_messages.cpp
// The relay through the iPhone: JSON from the phone in 0x02 pieces, the events Dotty sends, and
// what comes back (a fetch result, Gemini's own close, or a Gemini message passed through).
#include <ArduinoJson.h>

#include <vector>

#include "check.h"
#include "relay_messages.h"

static std::vector<uint8_t> piece(bool more, const std::string &bytes) {
  std::vector<uint8_t> p = {0x02, static_cast<uint8_t>(more ? 1 : 0)};
  p.insert(p.end(), bytes.begin(), bytes.end());
  return p;
}

int main() {
  PieceAssembler a;
  std::string out;
  std::vector<uint8_t> p1 = piece(true, R"({"serverCon)"), p2 = piece(true, R"(tent":{"turnC)"), p3 = piece(false, R"(omplete":true}})");
  CHECK("pieces: not done after the first", !a.add(p1.data(), p1.size(), out));
  CHECK("pieces: not done after the second", !a.add(p2.data(), p2.size(), out));
  CHECK("pieces: three make the message", a.add(p3.data(), p3.size(), out) && out == R"({"serverContent":{"turnComplete":true}})");
  std::vector<uint8_t> one = piece(false, R"({"setupComplete":{}})");
  CHECK("pieces: a single one", a.add(one.data(), one.size(), out) && out == R"({"setupComplete":{}})");

  // A 60 KB message (a recall's chats) in 290-byte pieces arrives whole.
  std::string big = R"({"relay":"fetched","id":3,"status":200,"body":")" + std::string(60000, 'x') + R"("})";
  bool done = false;
  for (size_t at = 0; at < big.size(); at += 288) {
    const std::string chunk = big.substr(at, 288);
    std::vector<uint8_t> p = piece(at + 288 < big.size(), chunk);
    done = a.add(p.data(), p.size(), out);
  }
  CHECK("pieces: 60 KB whole", done && out == big);

  // Too big: refused, and the next message starts clean.
  std::vector<uint8_t> huge = piece(true, std::string(4000, 'y'));
  bool refused = false;
  for (size_t sent = 0; sent < kRelayMaxMessage + 8000; sent += 4000) refused |= !a.add(huge.data(), huge.size(), out) && out.empty() && a.size() == 0;
  CHECK("pieces: past the limit, dropped", refused);
  std::vector<uint8_t> last = piece(false, "end");
  CHECK("pieces: the oversized one's last piece completes nothing", !a.add(last.data(), last.size(), out) && out.empty());
  CHECK("pieces: the next one is clean", a.add(one.data(), one.size(), out) && out == R"({"setupComplete":{}})");
  std::vector<uint8_t> wrong = {0x01, 0x00, 0x05};
  CHECK("pieces: not a 0x02 value: refused", !a.add(wrong.data(), wrong.size(), out));

  // Voice frames fit one notification: 0x1F 0x01 seq + the ADPCM header + 2 samples a byte ≤ MTU - 3
  // (570 samples made 291 bytes at MTU 293, and the stack silently cut the last byte).
  CHECK_EQ("voice: 568 samples at MTU 293", relayVoiceSamples(293), size_t(568));
  CHECK("voice: the frame fits", 3 + 3 + relayVoiceSamples(293) / 2 <= 293 - 3);
  CHECK_EQ("voice: at most 570 (the frame buffer)", relayVoiceSamples(517), size_t(570));
  CHECK("voice: a tiny MTU still gets samples", relayVoiceSamples(23) >= 2 && 3 + 3 + relayVoiceSamples(23) / 2 <= 20);

  // Events Dotty sends.
  JsonDocument e;
  deserializeJson(e, relayOpenEvent(R"({"setup":{"model":"m"}})", "OJig", ""));
  CHECK("open: the setup as a string, the key's last 4, no resume",
        std::string(e["event"] | "") == "relay.open" && std::string(e["setup"] | "") == R"({"setup":{"model":"m"}})" &&
            std::string(e["key"] | "") == "OJig" && e["resume"].isNull());
  deserializeJson(e, relayOpenEvent("{}", "OJig", "handle-1"));
  CHECK("open: a resume handle when continuing", std::string(e["resume"] | "") == "handle-1");
  deserializeJson(e, relaySendEvent(R"({"realtimeInput":{"activityEnd":{}}})"));
  CHECK("send: the message as a string", std::string(e["event"] | "") == "relay.send" &&
                                              std::string(e["message"] | "") == R"({"realtimeInput":{"activityEnd":{}}})");
  deserializeJson(e, relayFetchEvent(7, "gemini-flash-lite-latest", "{\"contents\":[]}"));
  CHECK("fetch: id, model, body", (e["id"] | 0) == 7 && std::string(e["model"] | "") == "gemini-flash-lite-latest" &&
                                      std::string(e["body"] | "") == "{\"contents\":[]}");
  deserializeJson(e, relayCloseEvent());
  CHECK("close", std::string(e["event"] | "") == "relay.close");

  // What comes back.
  RelayIncoming f = parseRelayIncoming(R"({"relay":"fetched","id":7,"status":200,"body":"{\"x\":1}"})");
  CHECK("incoming: a fetch result", f.kind == RelayIncoming::Fetched && f.id == 7 && f.status == 200 && f.body == "{\"x\":1}");
  RelayIncoming c = parseRelayIncoming(R"({"relay":"closed","code":1011,"reason":"quota"})");
  CHECK("incoming: Gemini closed", c.kind == RelayIncoming::Closed && c.code == 1011 && c.reason == "quota");
  const std::string msg = R"({"serverContent":{"outputTranscription":{"text":"Hi"}}})";
  RelayIncoming g = parseRelayIncoming(msg);
  CHECK("incoming: a Gemini message, untouched", g.kind == RelayIncoming::Gemini && g.gemini == msg);
  CHECK("incoming: not JSON", parseRelayIncoming("{oops").kind == RelayIncoming::Bad);
  return failures ? 1 : 0;
}
