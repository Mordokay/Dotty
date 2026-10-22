#include "live_protocol.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <stdlib.h>

#include "ws_frame.h"

namespace {

const char *const kMoods[] = {"happy", "sad", "surprised", "thinking", "laughing", "love", "neutral"};

std::string serialize(const JsonDocument &doc) {
  std::string out;
  serializeJson(doc, out);
  return out;
}

// "37s" or "37.5s" → 37.
int seconds(const char *duration) {
  return duration && isdigit(static_cast<unsigned char>(duration[0])) ? atoi(duration) : -1;
}

std::string lower(std::string s) {
  for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

std::string base64Decode(const char *data, size_t len) {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  std::string out;
  out.reserve(len * 3 / 4);
  uint32_t bits = 0;
  int count = 0;
  for (size_t i = 0; i < len; i++) {
    const int v = value(data[i]);
    if (v < 0) continue;  // '=' padding, whitespace
    bits = bits << 6 | v;
    if (++count == 4) {
      out += static_cast<char>(bits >> 16);
      out += static_cast<char>(bits >> 8);
      out += static_cast<char>(bits);
      bits = 0;
      count = 0;
    }
  }
  if (count == 3) {
    out += static_cast<char>(bits >> 10);
    out += static_cast<char>(bits >> 2);
  } else if (count == 2) {
    out += static_cast<char>(bits >> 4);
  }
  return out;
}

std::string liveSetup(const LiveConfig &config) {
  JsonDocument doc;
  JsonObject setup = doc["setup"].to<JsonObject>();
  setup["model"] = config.model;
  JsonObject gen = setup["generationConfig"].to<JsonObject>();
  gen["responseModalities"].add(config.textOnly ? "TEXT" : "AUDIO");
  gen["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"] = config.voice;
  setup["systemInstruction"]["parts"][0]["text"] = config.systemInstruction;
  setup["realtimeInputConfig"]["automaticActivityDetection"]["disabled"] = true;
  setup["inputAudioTranscription"].to<JsonObject>();
  setup["outputAudioTranscription"].to<JsonObject>();
  JsonObject sessionResumption = setup["sessionResumption"].to<JsonObject>();
  if (!config.resumeHandle.empty()) sessionResumption["handle"] = config.resumeHandle;
  JsonObject mood = setup["tools"][0]["functionDeclarations"].add<JsonObject>();
  mood["name"] = "set_mood";
  mood["behavior"] = "NON_BLOCKING";
  mood["description"] = "Show how you feel about your reply on Dotty's face. Call it with every reply.";
  mood["parameters"]["type"] = "OBJECT";
  JsonObject moodParam = mood["parameters"]["properties"]["mood"].to<JsonObject>();
  moodParam["type"] = "STRING";
  for (const char *m : kMoods) moodParam["enum"].add(m);
  mood["parameters"]["required"].add("mood");
  return serialize(doc);
}

std::string liveActivityStart() {
  return R"({"realtimeInput":{"activityStart":{}}})";
}

std::string liveActivityEnd() {
  return R"({"realtimeInput":{"activityEnd":{}}})";
}

std::string liveAudio(const int16_t *pcm, size_t frames) {
  return R"({"realtimeInput":{"audio":{"mimeType":"audio/pcm;rate=16000","data":")" +
         base64Encode(reinterpret_cast<const uint8_t *>(pcm), frames * 2) + R"("}}})";
}

std::string liveToolOk(const std::string &callId, const std::string &name) {
  JsonDocument doc;
  JsonObject response = doc["toolResponse"]["functionResponses"].add<JsonObject>();
  response["id"] = callId;
  response["name"] = name;
  response["response"]["result"] = "ok";
  response["response"]["scheduling"] = "SILENT";
  return serialize(doc);
}

LiveEvent liveParse(const std::string &message) {
  LiveEvent e;
  JsonDocument doc;
  if (deserializeJson(doc, message) != DeserializationError::Ok) return e;
  e.setupComplete = doc["setupComplete"].is<JsonObject>();
  e.goAway = !doc["goAway"].isNull();
  if (const char *handle = doc["sessionResumptionUpdate"]["newHandle"]) e.resumeHandle = handle;
  if (!doc["usageMetadata"]["totalTokenCount"].isNull()) e.totalTokens = doc["usageMetadata"]["totalTokenCount"];
  for (JsonObject call : doc["toolCall"]["functionCalls"].as<JsonArray>()) {
    e.toolCallId = call["id"] | "";
    e.toolName = call["name"] | "";
    if (e.toolName == "set_mood") e.mood = call["args"]["mood"] | "";
  }
  JsonObject content = doc["serverContent"];
  if (!content.isNull()) {
    e.turnComplete = content["turnComplete"] | false;
    e.generationComplete = content["generationComplete"] | false;
    e.interrupted = content["interrupted"] | false;
    if (const char *t = content["inputTranscription"]["text"]) e.inputText = t;
    if (const char *t = content["outputTranscription"]["text"]) e.outputText = t;
    for (JsonObject part : content["modelTurn"]["parts"].as<JsonArray>()) {
      if (const char *data = part["inlineData"]["data"]) e.audioPcm += base64Decode(data, strlen(data));
      if (const char *text = part["text"]) e.text += text;
    }
  }
  if (!doc["error"].isNull()) {
    e.errorCode = doc["error"]["code"] | -1;
    for (JsonObject detail : doc["error"]["details"].as<JsonArray>()) {
      if (detail["retryDelay"].is<const char *>()) e.retryDelaySeconds = seconds(detail["retryDelay"]);
    }
  }
  return e;
}

LiveClose liveCloseInfo(uint16_t code, const std::string &reason) {
  LiveClose c;
  c.message = reason;
  const std::string r = lower(reason);
  c.quota = r.find("exhausted") != std::string::npos || r.find("quota") != std::string::npos ||
            r.find("rate limit") != std::string::npos;
  c.badKey = r.find("api key") != std::string::npos && (r.find("not valid") != std::string::npos ||
                                                        r.find("invalid") != std::string::npos ||
                                                        r.find("expired") != std::string::npos);
  const size_t retry = r.find("retry in ");
  if (retry != std::string::npos) c.retryDelaySeconds = seconds(reason.c_str() + retry + 9);
  (void)code;
  return c;
}

std::string systemInstruction(const std::string &localTime, const std::string &place) {
  std::string s =
      "You are Dotty, a small desk companion. Talk like a person in conversation: usually one or two short "
      "sentences; longer only when asked. You are on a tiny screen with a speaker: never use lists, markdown or "
      "emoji. Call set_mood with each reply. The local time is " + localTime;
  if (!place.empty()) s += ", the user is in " + place;
  return s + ".";
}
