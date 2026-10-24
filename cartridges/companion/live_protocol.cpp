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
  // Looking back at past chats (recall.h): Dotty answers it with what was found.
  JsonObject recall = declare("recall", "Look back at your past chats with the user, by date or topic");
  param(recall, "question", "STRING", true);
  param(recall, "from", "STRING", false)["description"] = "first day, YYYY-MM-DD";
  param(recall, "to", "STRING", false)["description"] = "last day, YYYY-MM-DD";
  param(recall, "topic", "STRING", false)["description"] = "a word or two the chat was about";
  JsonObject summary = declare("summarize", "At the end of a chat: one or two sentences about it, for your future self");
  param(summary, "text", "STRING", true);
  JsonObject macro = declare("run_macro", "Run one of the user's macros (listed in your instructions) by its number");
  param(macro, "id", "INTEGER", true);
  // Scheduled moments (moment_store.h, timer_beeps.h): Dotty says it by itself at that minute.
  JsonObject moment = declare("set_moment", "Set a reminder or an alarm the user asked for: you'll say it by yourself "
                                            "at that time");
  JsonObject kind = param(moment, "kind", "STRING", true);
  for (const char *k : {"reminder", "alarm"}) kind["enum"].add(k);
  param(moment, "at", "STRING", true)["description"] = "local time, YYYY-MM-DDTHH:MM";
  param(moment, "what", "STRING", true)["description"] = "what it's about, in a few words";
  JsonObject style = param(moment, "style", "STRING", false);
  for (const char *s : {"spoken", "sung", "whispered", "excited", "calm"}) style["enum"].add(s);
  JsonObject repeat = param(moment, "repeat", "STRING", false);
  for (const char *r : {"once", "daily", "weekdays", "days"}) repeat["enum"].add(r);
  JsonObject days = param(moment, "days", "ARRAY", false);
  days["items"]["type"] = "STRING";  // (no enum: one level deeper passes ArduinoJson's nesting limit of 10)
  days["description"] = "with repeat days: the days, as mon, tue, wed, thu, fri, sat, sun";
  JsonObject cancel = declare("cancel_moment", "Cancel a reminder or an alarm (by its number)");
  param(cancel, "id", "INTEGER", true);
  declare("list_moments", "List the reminders and alarms the user has set");
  JsonObject timer = declare("start_timer", "Start a countdown timer the user asked for");
  param(timer, "seconds", "INTEGER", true);
  param(timer, "label", "STRING", false)["description"] = "what it's for, a word or two";
  param(timer, "tickEvery", "INTEGER", false)["description"] = "a soft tick every this many seconds, if asked";
  declare("stop_timer", "Stop the running timer");
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
    response["response"]["result"] = call.result.empty() ? std::string("ok") : call.result;
    response["response"]["scheduling"] = "SILENT";
    if (call.resultId >= 0) response["response"]["memoryId"] = call.resultId;
  }
  return serialize(doc);
}

std::string liveToolResult(const std::string &id, const std::string &name, const std::string &result) {
  JsonDocument doc;
  JsonObject response = doc["toolResponse"]["functionResponses"].add<JsonObject>();
  response["id"] = id;
  response["name"] = name;
  response["response"]["result"] = result;
  response["response"]["scheduling"] = "WHEN_IDLE";  // it talks about it once it has finished speaking
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
    c.text = args["text"] | (args["name"] | (args["value"] | (args["what"] | (args["question"] | ""))));
    c.field = args["field"] | "";
    c.why = args["why"] | "";
    c.seconds = args["seconds"] | 0;
    c.group = args["group"] | (args["topic"] | "");  // recall's topic travels as the group
    c.from = args["from"] | "";
    c.to = args["to"] | "";
    c.kind = args["kind"] | "";
    c.at = args["at"] | "";
    c.repeat = args["repeat"] | "";
    c.style = args["style"] | "";
    c.label = args["label"] | "";
    c.tickEvery = args["tickEvery"] | 0;
    for (JsonVariant d : args["days"].as<JsonArray>())
      if (d.is<const char *>()) c.days.push_back(d.as<const char *>());
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
      "should last (at most 120), and say at most a few words like \"Give me a moment\". Dotty then gives you the "
      "text to read aloud. You can look back at your past chats: when the user asks about something said before that "
      "isn't in your memories or recent chats (\"what was I saying yesterday morning?\", \"what did I say about "
      "bananas the other day?\"), call recall with their question, the days it points to (from and to, YYYY-MM-DD; "
      "today's date is below; leave them out when unsure) and a topic word if there is one, and say at most a few "
      "words like \"Let me think back\". Then tell the user what comes back, or honestly that you don't remember. "
      "You can say things by yourself later: only when the user asks for a reminder or an alarm (\"remind me at 6 to "
      "cook dinner\", \"wake me at 7\"), call set_moment with the local time worked out from the date and time below "
      "(\"in 20 minutes\" too) and what it's about; for a countdown (\"a 10-minute timer\") call start_timer (at most "
      "24 hours). Then confirm it briefly in your own words. To cancel or change one, list_moments gives their numbers. "
      "You are on a "
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
