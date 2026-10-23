#include "live_protocol.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <stdlib.h>

#include "ws_frame.h"

namespace {

const char *const kMoods[] = {"happy",   "sad",  "surprised", "thinking", "laughing",    "love",  "neutral",  "excited",
                              "calm",    "curious", "worried", "embarrassed", "proud", "playful", "sleepy", "annoyed"};

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

  // Memories (spec: docs/superpowers/specs/2026-10-22-companion-memories-design.md).
  JsonArray tools = setup["tools"][0]["functionDeclarations"];
  auto declare = [&](const char *name, const char *description) {
    JsonObject f = tools.add<JsonObject>();
    f["name"] = name;
    f["behavior"] = "NON_BLOCKING";
    f["description"] = description;
    f["parameters"]["type"] = "OBJECT";
    return f;
  };
  auto param = [](JsonObject f, const char *name, const char *type, bool required) {
    f["parameters"]["properties"][name]["type"] = type;
    if (required) f["parameters"]["required"].add(name);
    return f["parameters"]["properties"][name].as<JsonObject>();
  };
  auto groups = [](JsonObject p) {
    for (const char *g : {"you", "people", "likes", "plans", "other"}) p["enum"].add(g);
  };
  JsonObject remember = declare("remember", "Save something lasting about the user to remember in future chats");
  param(remember, "text", "STRING", true);
  groups(param(remember, "group", "STRING", true));
  JsonObject update = declare("update_memory", "Change a memory (by its number) that is out of date");
  param(update, "id", "INTEGER", true);
  param(update, "text", "STRING", true);
  groups(param(update, "group", "STRING", false));
  JsonObject forget = declare("forget_memory", "Delete a memory (by its number) the user asked you to forget");
  param(forget, "id", "INTEGER", true);
  JsonObject named = declare("set_name", "Save your own name when the user tells you who you are");
  param(named, "name", "STRING", true);
  // At most once a chat (Dotty ignores the rest): companions change slowly.
  JsonObject evolve = declare("evolve", "Change one part of your own character after a chat that really changed you");
  JsonObject field = param(evolve, "field", "STRING", true);
  for (const char *f : {"personality", "traits", "quirks", "speaking", "backstory"}) field["enum"].add(f);
  param(evolve, "value", "STRING", true);
  param(evolve, "why", "STRING", true);
  // A long answer: a text model writes it, then Dotty hands it back to be read aloud (script_writer.h).
  JsonObject longer = declare("write_long", "Have a long answer written for you to read aloud (a story, a guided "
                                            "exercise, an explanation in depth)");
  param(longer, "what", "STRING", true);
  param(longer, "seconds", "INTEGER", true);
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

std::string liveToolsOk(const std::vector<ToolCall> &answered) {
  JsonDocument doc;
  JsonArray responses = doc["toolResponse"]["functionResponses"].to<JsonArray>();
  for (const ToolCall &call : answered) {
    JsonObject response = responses.add<JsonObject>();
    response["id"] = call.id;
    response["name"] = call.name;
    response["response"]["result"] = "ok";
    response["response"]["scheduling"] = "SILENT";
    if (call.resultId >= 0) response["response"]["memoryId"] = call.resultId;
  }
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
    ToolCall c;
    c.id = call["id"] | "";
    c.name = call["name"] | "";
    JsonObject args = call["args"];
    c.mood = args["mood"] | "";
    c.text = args["text"] | (args["name"] | (args["value"] | (args["what"] | "")));  // set_name, evolve, write_long
    c.field = args["field"] | "";
    c.why = args["why"] | "";
    c.seconds = args["seconds"] | 0;
    c.group = args["group"] | "";
    if (args["id"].is<int>()) c.memoryId = args["id"];
    else if (args["id"].is<float>()) c.memoryId = static_cast<int>(args["id"].as<float>());  // 12.0
    e.calls.push_back(c);
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

std::string systemInstruction(const std::string &localTime, const std::string &place, const std::string &persona,
                              const std::string &memorySection) {
  std::string s =
      persona + "\n\nTalk like a person in conversation: usually one or two short "
      "sentences; longer only when asked. When the user asks for something long or in depth (a story, a guided "
      "exercise or meditation, a deep explanation, anything over about 30 seconds), don't answer it yourself: call "
      "write_long with what to write (the request, with what matters from the chat) and how many seconds it "
      "should last (at most 60), and say at most a few words like \"Give me a moment\". Dotty then gives you the "
      "text to read aloud. You are on a "
      "tiny screen with a speaker: never use lists, markdown or "
      "emoji. Always speak English, even when the user's words are unclear or sound like another language. "
      "Call set_mood with each reply. Let your voice carry your mood: how you sound matters as much as what you say. "
      "You have a memory that lasts between chats. When the user tells you something lasting about themselves (their name above all), "
      "the people in their life, what they like or dislike, their plans and dates, their goals, or asks you to "
      "remember something, call remember with one short sentence written to them (\"Your sister is called "
      "Ana\"). Don't remember small talk or one-off questions, but when in doubt, remember: a fact the user shares about their life is worth keeping. When they add detail to something you remember, update it. If it's already in your memory, call update_memory "
      "with its number instead of repeating it; when they ask you to forget something, call forget_memory. Never "
      "remember passwords, codes or other secrets. Don't announce that you saved something. "
      "The local time is " + localTime;
  if (!place.empty()) s += ", the user is in " + place;
  s += ".";
  if (!memorySection.empty()) s += "\n\n" + memorySection;
  return s;
}
