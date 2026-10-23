#include "relay_messages.h"

#include <ArduinoJson.h>

bool PieceAssembler::add(const uint8_t *data, size_t len, std::string &complete) {
  complete.clear();
  if (len < 2 || data[0] != 0x02) return false;
  const bool more = data[1] & 1;
  if (!overflow_) {
    if (buffer_.size() + len - 2 > kRelayMaxMessage) {
      overflow_ = true;
      buffer_.clear();
      buffer_.shrink_to_fit();
    } else {
      if (buffer_.capacity() < 4096) buffer_.reserve(4096);  // ≥ 4 KB: PSRAM on the device
      buffer_.append(reinterpret_cast<const char *>(data + 2), len - 2);
    }
  }
  if (more) return false;
  const bool ok = !overflow_;
  if (ok) complete.swap(buffer_);
  reset();
  return ok;
}

void PieceAssembler::reset() {
  buffer_.clear();
  overflow_ = false;
}

namespace {

std::string serialize(JsonDocument &doc) {
  std::string out;
  serializeJson(doc, out);
  return out;
}

}  // namespace

std::string relayOpenEvent(const std::string &setupJson, const std::string &keyHint, const std::string &resume) {
  JsonDocument doc;
  doc["event"] = "relay.open";
  doc["setup"] = setupJson;
  doc["key"] = keyHint;
  if (!resume.empty()) doc["resume"] = resume;
  return serialize(doc);
}

std::string relaySendEvent(const std::string &message) {
  JsonDocument doc;
  doc["event"] = "relay.send";
  doc["message"] = message;
  return serialize(doc);
}

std::string relayFetchEvent(uint32_t id, const std::string &model, const std::string &body) {
  JsonDocument doc;
  doc["event"] = "relay.fetch";
  doc["id"] = id;
  doc["model"] = model;
  doc["body"] = body;
  return serialize(doc);
}

std::string relayCloseEvent() {
  return R"({"event":"relay.close"})";
}

RelayIncoming parseRelayIncoming(const std::string &json) {
  RelayIncoming in;
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok || !doc.is<JsonObject>()) return in;
  const std::string relay = doc["relay"] | "";
  if (relay.empty()) {
    in.kind = RelayIncoming::Gemini;
    in.gemini = json;
  } else if (relay == "fetched") {
    in.kind = RelayIncoming::Fetched;
    in.id = doc["id"] | 0u;
    in.status = doc["status"] | 0;
    in.body = doc["body"] | "";
  } else if (relay == "closed") {
    in.kind = RelayIncoming::Closed;
    in.code = doc["code"] | 0;
    in.reason = doc["reason"] | "";
  }
  return in;
}
