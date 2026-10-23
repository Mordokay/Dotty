#include "last_line.h"

#include <ArduinoJson.h>

#include <cstring>

std::string LastLine::text() const {
  std::string out;
  for (const Piece &p : pieces) out += p.text;
  return out;
}

std::string LastLine::textAt(uint32_t positionMs) const {
  std::string out;
  for (const Piece &p : pieces)
    if (p.ms <= positionMs || out.empty()) out += p.text;  // the first piece shows from the start
  return out;
}

void LastLine::cut(uint32_t endMs) {
  if (endMs >= ms) return;
  ms = endMs;
  while (!pieces.empty() && pieces.back().ms > endMs) pieces.pop_back();
}

std::string lastLineJson(const LastLine &l) {
  JsonDocument doc;
  doc["t"] = l.t;
  doc["ms"] = l.ms;
  doc["mood"] = l.mood;
  JsonArray pieces = doc["pieces"].to<JsonArray>();
  for (const LastLine::Piece &p : l.pieces) {
    JsonArray a = pieces.add<JsonArray>();
    a.add(p.ms);
    a.add(p.text);
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool parseLastLine(const std::string &json, LastLine &out) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok || !doc["pieces"].is<JsonArrayConst>()) return false;
  LastLine l;
  l.t = doc["t"] | static_cast<int64_t>(0);
  l.ms = doc["ms"] | 0u;
  l.mood = doc["mood"] | "neutral";
  for (JsonArrayConst p : doc["pieces"].as<JsonArrayConst>()) l.add(p[0] | 0u, p[1] | "");
  out = l;
  return true;
}

void wavHeader(uint8_t h[44], uint32_t dataBytes, uint32_t rate) {
  static const uint8_t kStart[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                   16, 0, 0, 0, 1, 0, 1, 0};
  memset(h, 0, 44);
  memcpy(h, kStart, sizeof(kStart));
  auto put32 = [&](int at, uint32_t v) {
    for (int i = 0; i < 4; i++) h[at + i] = v >> (8 * i);
  };
  put32(4, 36 + dataBytes);
  put32(24, rate);
  put32(28, rate * 2);
  h[32] = 2, h[34] = 16;
  memcpy(h + 36, "data", 4);
  put32(40, dataBytes);
}
