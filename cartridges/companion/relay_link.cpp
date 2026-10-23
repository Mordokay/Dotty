#include "relay_link.h"

#include <freertos/message_buffer.h>

#include <map>
#include <vector>

#include "adpcm.h"
#include "core_ble.h"
#include "log.h"
#include "relay_messages.h"

namespace relay {
namespace {

constexpr size_t kBufferBytes = 64 * 1024;
size_t frameSamples() {
  return relayVoiceSamples(ble::mtu());
}

MessageBufferHandle_t incoming = nullptr;  // the phone's writes: BLE task → loop
bool helloOk = false;
uint32_t helloConnection = 0;  // the BLE connection the hello came on (a reconnect says hello again)
std::vector<String> hints;
PieceAssembler assembler;

adpcm::State encoder;
uint8_t voiceSeq = 0;
std::vector<int16_t> pendingVoice;

struct Fetch {
  uint32_t startedAt;
  bool done = false;
  int status = 0;
  std::string body;
};
std::map<uint32_t, Fetch> fetches;
uint32_t nextFetch = 1;

bool sendFrame(const int16_t *pcm, size_t count) {
  static uint8_t buf[3 + adpcm::kHeader + kRelayMaxVoiceSamples / 2 + 1];
  buf[0] = 0x1F;
  buf[1] = 0x01;
  buf[2] = voiceSeq++;
  const size_t n = 3 + adpcm::encodeFrame(pcm, count, encoder, buf + 3);
  for (int tries = 0; tries < 100; tries++) {  // the stack's buffers fill during a burst
    if (ble::notifyRaw(buf, n)) return true;
    if (!ble::connected()) return false;
    delay(2);
  }
  LOGW("relay", "voice frame dropped");
  return false;
}

// Everything the phone sent, in order. Gemini's messages and audio go to the handlers (none: dropped);
// fetch results are kept; a close is reported through `closed`.
void drain(const std::function<void(const std::string &)> *onJson,
           const std::function<void(const int16_t *, size_t)> *onAudio, bool *closed, uint16_t *code,
           std::string *reason) {
  if (!incoming) return;
  static uint8_t in[520];
  static int16_t pcm[1100];
  size_t n;
  std::string message;
  while ((n = xMessageBufferReceive(incoming, in, sizeof(in), 0)) > 0) {
    if (in[0] == 0x01 && n > 2) {
      const size_t k = adpcm::decodeFrame(in + 2, n - 2, pcm, sizeof(pcm) / sizeof(pcm[0]));
      if (onAudio && *onAudio) (*onAudio)(pcm, k);
    } else if (in[0] == 0x02 && assembler.add(in, n, message)) {
      const RelayIncoming m = parseRelayIncoming(message);
      if (m.kind == RelayIncoming::Gemini) {
        if (onJson && *onJson) (*onJson)(m.gemini);
      } else if (m.kind == RelayIncoming::Fetched) {
        auto it = fetches.find(m.id);
        if (it != fetches.end()) it->second.done = true, it->second.status = m.status, it->second.body = m.body;
      } else if (m.kind == RelayIncoming::Closed && closed) {
        *closed = true;
        *code = m.code;
        *reason = m.reason;
      }
    }
  }
}

}  // namespace

void begin() {
  if (incoming) return;
  static StaticMessageBuffer_t control;
  uint8_t *store = static_cast<uint8_t *>(heap_caps_malloc(kBufferBytes + 1, MALLOC_CAP_SPIRAM));
  if (!store) return LOGE("relay", "no PSRAM for the buffer");
  incoming = xMessageBufferCreateStatic(kBufferBytes, store, &control);
  ble::onData([](const uint8_t *data, size_t len) {  // on the BLE task: copy and return
    if (incoming && xMessageBufferSend(incoming, data, len, 0) != len) LOGW("relay", "buffer full: dropped %u bytes", static_cast<unsigned>(len));
  });
  ble::on("relay.hello", [](JsonObjectConst args, JsonObject reply) {
    helloOk = (args["version"] | 0) == kVersion;
    helloConnection = ble::connection();
    hints.clear();
    for (JsonVariantConst k : args["keys"].as<JsonArrayConst>()) hints.push_back(k | "");
    reply["version"] = kVersion;
    LOGI("relay", "the phone says hello (version %d, %u keys)%s", args["version"] | 0, static_cast<unsigned>(hints.size()),
         helloOk ? "" : ": not our version");
  });
}

void poll() {
  if (!ble::connected()) helloOk = false;  // a reconnect says hello again
  drain(nullptr, nullptr, nullptr, nullptr, nullptr);
}

bool phoneReady() {
  return helloOk && ble::connection() != 0 && ble::connection() == helloConnection;
}

bool phoneHasKey(const String &keyHint) {
  for (const String &h : hints)
    if (h == keyHint) return true;
  return false;
}

uint32_t fetch(const std::string &model, const std::string &body) {
  const uint32_t id = nextFetch++;
  fetches[id] = Fetch{millis()};
  ble::notify(relayFetchEvent(id, model, body));
  return id;
}

bool takeFetched(uint32_t id, int &status, std::string &body) {
  auto it = fetches.find(id);
  if (it == fetches.end()) return false;
  if (!it->second.done && millis() - it->second.startedAt < kFetchTimeoutMs && ble::connected()) return false;
  status = it->second.done ? it->second.status : 0;
  body = it->second.done ? it->second.body : (ble::connected() ? "timed out" : "lost the phone");
  fetches.erase(it);
  return true;
}

bool Transport::open(const std::string &apiKey, const std::string &setupJson, const std::string &resume, String &error) {
  if (!phoneReady()) {
    error = "no phone";
    return false;
  }
  encoder = adpcm::State();
  voiceSeq = 0;
  pendingVoice.clear();
  assembler.reset();
  closeCode_ = 0;
  closeReason_.clear();
  open_ = true;
  connection_ = ble::connection();
  const std::string hint = apiKey.size() >= 4 ? apiKey.substr(apiKey.size() - 4) : apiKey;
  ble::notify(relayOpenEvent(setupJson, hint, resume));
  return true;
}

bool Transport::sendText(const std::string &json) {
  if (!connected()) return false;
  ble::notify(relaySendEvent(json));
  return true;
}

bool Transport::sendVoice(const int16_t *pcm, size_t frames) {
  if (!connected()) return false;
  pendingVoice.insert(pendingVoice.end(), pcm, pcm + frames);
  size_t at = 0;
  const size_t per = frameSamples();
  while (pendingVoice.size() - at >= per) {
    if (!sendFrame(pendingVoice.data() + at, per)) break;
    at += per;
  }
  pendingVoice.erase(pendingVoice.begin(), pendingVoice.begin() + at);
  return true;
}

void Transport::flushVoice() {
  if (!pendingVoice.empty() && connected()) sendFrame(pendingVoice.data(), pendingVoice.size());
  pendingVoice.clear();
}

bool Transport::poll(const std::function<void(const std::string &)> &onJson,
                     const std::function<void(const int16_t *, size_t)> &onAudio) {
  if (!open_) return true;
  bool closed = false;
  drain(&onJson, &onAudio, &closed, &closeCode_, &closeReason_);
  if (ble::connection() != connection_) {  // the phone went away (even if it's back): the session with it
    closed = true;
    closeCode_ = 0;
    closeReason_ = "Lost the phone";
  }
  if (closed) {
    open_ = false;
    return false;
  }
  return true;
}

void Transport::close() {
  if (connected()) ble::notify(std::string(relayCloseEvent()));
  open_ = false;
}

bool Transport::connected() const {
  return open_ && ble::connection() == connection_;
}

}  // namespace relay
