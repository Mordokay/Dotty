// uses: cartridges/companion/live_protocol.cpp cartridges/companion/ws_frame.cpp
// The Gemini Live messages: what Dotty sends and how it reads what comes back
// (samples in live_samples/ were captured from a real session on 2026-10-22).
#include <fstream>
#include <sstream>

#include <ArduinoJson.h>

#include "check.h"
#include "live_protocol.h"

static std::string sample(const char *name) {
  std::ifstream f(std::string("live_samples/") + name);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

int main() {
  LiveConfig config;
  config.model = "models/gemini-3.8-live";
  config.voice = "Zephyr";
  config.systemInstruction = "Be brief.";
  JsonDocument setup;
  deserializeJson(setup, liveSetup(config));
  CHECK_EQ("setup: model", std::string(setup["setup"]["model"] | ""), std::string("models/gemini-3.8-live"));
  CHECK_EQ("setup: voice", std::string(setup["setup"]["generationConfig"]["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"] | ""),
           std::string("Zephyr"));
  CHECK_EQ("setup: audio replies", std::string(setup["setup"]["generationConfig"]["responseModalities"][0] | ""), std::string("AUDIO"));
  CHECK("setup: push-to-talk (no automatic voice detection)",
        setup["setup"]["realtimeInputConfig"]["automaticActivityDetection"]["disabled"] == true);
  CHECK("setup: transcripts both ways", setup["setup"]["inputAudioTranscription"].is<JsonObject>() &&
                                            setup["setup"]["outputAudioTranscription"].is<JsonObject>());
  JsonObject mood = setup["setup"]["tools"][0]["functionDeclarations"][0];
  CHECK_EQ("setup: set_mood declared", std::string(mood["name"] | ""), std::string("set_mood"));
  CHECK_EQ("setup: set_mood doesn't block speech", std::string(mood["behavior"] | ""), std::string("NON_BLOCKING"));
  std::string moods;
  for (JsonVariant m : mood["parameters"]["properties"]["mood"]["enum"].as<JsonArray>()) moods += std::string(m.as<const char *>()) + ",";
  CHECK_EQ("setup: the seven moods", moods, std::string("happy,sad,surprised,thinking,laughing,love,neutral,"));
  JsonArray fns = setup["setup"]["tools"][0]["functionDeclarations"];
  std::string names;
  bool allNonBlocking = true;
  for (JsonObject f : fns) {
    names += std::string(f["name"] | "") + ",";
    allNonBlocking &= std::string(f["behavior"] | "") == "NON_BLOCKING";
  }
  CHECK_EQ("setup: the four tools", names, std::string("set_mood,remember,update_memory,forget_memory,"));
  CHECK("setup: every tool non-blocking", allNonBlocking);
  std::string groups;
  for (JsonVariant g : fns[1]["parameters"]["properties"]["group"]["enum"].as<JsonArray>()) groups += std::string(g.as<const char *>()) + ",";
  CHECK_EQ("remember: the five groups", groups, std::string("you,people,likes,plans,other,"));
  CHECK("update_memory: needs id and text", fns[2]["parameters"]["required"][0] == "id" && fns[2]["parameters"]["required"][1] == "text");
  CHECK("forget_memory: needs id", fns[3]["parameters"]["required"][0] == "id");
  CHECK("setup: asks for resumption handles", setup["setup"]["sessionResumption"].is<JsonObject>());
  CHECK_EQ("setup: no handle on a fresh session", std::string(setup["setup"]["sessionResumption"]["handle"] | "none"), std::string("none"));

  config.textOnly = true;
  config.resumeHandle = "abc";
  JsonDocument textSetup;
  deserializeJson(textSetup, liveSetup(config));
  CHECK_EQ("setup: text-only replies", std::string(textSetup["setup"]["generationConfig"]["responseModalities"][0] | ""), std::string("TEXT"));
  CHECK_EQ("setup: resumes a session", std::string(textSetup["setup"]["sessionResumption"]["handle"] | ""), std::string("abc"));

  const int16_t pcm[2] = {1, -1};
  JsonDocument audio;
  deserializeJson(audio, liveAudio(pcm, 2));
  CHECK_EQ("audio chunk: base64 data", std::string(audio["realtimeInput"]["audio"]["data"] | ""), std::string("AQD//w=="));
  CHECK_EQ("audio chunk: format", std::string(audio["realtimeInput"]["audio"]["mimeType"] | ""), std::string("audio/pcm;rate=16000"));
  CHECK("turn start/end", liveActivityStart().find("activityStart") != std::string::npos &&
                              liveActivityEnd().find("activityEnd") != std::string::npos);
  JsonDocument ok;
  ToolCall a, b;
  a.id = "call_1", a.name = "set_mood";
  b.id = "call_2", b.name = "remember", b.resultId = 7;
  deserializeJson(ok, liveToolsOk({a, b}));
  JsonArray responses = ok["toolResponse"]["functionResponses"];
  CHECK_EQ("tools ok: remember tells the model the new memory's number",
           int(ok["toolResponse"]["functionResponses"][1]["response"]["memoryId"] | -1), 7);
  CHECK("tools ok: no number when there's none", ok["toolResponse"]["functionResponses"][0]["response"]["memoryId"].isNull());
  CHECK_EQ("tools ok: one response per call", responses.size(), size_t(2));
  CHECK("tools ok: ids, names, silent", std::string(responses[0]["id"] | "") == "call_1" &&
                                            std::string(responses[1]["name"] | "") == "remember" &&
                                            std::string(responses[1]["response"]["scheduling"] | "") == "SILENT");

  CHECK("parse: setup complete", liveParse(sample("setup_complete.json")).setupComplete);
  CHECK_EQ("parse: resumption handle", liveParse(sample("resumption.json")).resumeHandle,
           std::string("aa9db77f-d5dd-4dee-a85f-a5e81dcab1e8"));
  const LiveEvent call = liveParse(sample("tool_call.json"));
  CHECK("parse: set_mood call", call.calls.size() == 1 && call.calls[0].name == "set_mood" && call.calls[0].mood == "happy" &&
                                    call.calls[0].id == "call_495057");
  const LiveEvent many = liveParse(sample("tool_calls_memory.json"));
  CHECK_EQ("parse: every call, in order", many.calls.size(), size_t(3));
  CHECK("parse: remember's text and group", many.calls.size() == 3 && many.calls[1].text == "Your sister is called Ana" &&
                                                many.calls[1].group == "people");
  CHECK("parse: forget_memory's id", many.calls.size() == 3 && many.calls[2].memoryId == 12);
  const LiveEvent said = liveParse(sample("said.json"));
  CHECK_EQ("parse: reply text", said.outputText, std::string("That is absolutely "));
  CHECK_EQ("parse: reply audio decoded", said.audioPcm.size(), size_t(2));
  CHECK_EQ("parse: audio piece", liveParse(sample("audio.json")).audioPcm.size(), size_t(12));
  CHECK_EQ("parse: your words", liveParse(sample("heard.json")).inputText, std::string("Can you tell me a joke?"));
  const LiveEvent done = liveParse(sample("turn_complete_usage.json"));
  CHECK("parse: turn complete with tokens", done.turnComplete && done.totalTokens == 662);
  CHECK("parse: go away", liveParse(sample("go_away.json")).goAway);
  CHECK("parse: empty message", !liveParse(sample("empty.json")).turnComplete);
  const LiveEvent quotaJson = liveParse(R"({"error":{"code":429,"status":"RESOURCE_EXHAUSTED","details":[{"retryDelay":"37s"}]}})");
  CHECK("parse: quota error with retry delay", quotaJson.errorCode == 429 && quotaJson.retryDelaySeconds == 37);

  const LiveClose quota = liveCloseInfo(1011, "Resource has been exhausted (e.g. check quota).");
  CHECK("close: quota", quota.quota && !quota.badKey);
  const LiveClose badKey = liveCloseInfo(1007, "API key not valid. Please pass a valid API key.");
  CHECK("close: bad key", badKey.badKey && !badKey.quota);
  CHECK_EQ("close: retry delay in the reason", liveCloseInfo(1011, "Quota exceeded. Please retry in 21s.").retryDelaySeconds, 21);

  const std::string withPlace = systemInstruction("14:05", "Lisbon", "");
  const std::string ending = "The local time is 14:05, the user is in Lisbon.";
  CHECK("instruction: with a place", withPlace.compare(withPlace.size() - ending.size(), ending.size(), ending) == 0);
  const std::string noPlace = systemInstruction("14:05", "", "");
  CHECK("instruction: no place clause", noPlace.find("user is in") == std::string::npos &&
                                            noPlace.compare(noPlace.size() - 24, 24, "The local time is 14:05.") == 0);
  CHECK("instruction: memory guidance always there", noPlace.find("Never remember passwords") != std::string::npos);
  CHECK("instruction: no memories, no memory list", noPlace.find("What you remember") == std::string::npos);
  const std::string section = "What you remember about the user:\nPeople: [12] Your sister is called Ana.";
  const std::string withMemories = systemInstruction("14:05", "", section);
  CHECK("instruction: memories at the end",
        withMemories.compare(withMemories.size() - section.size(), section.size(), section) == 0);
  CHECK("instruction: short replies", noPlace.rfind("You are Dotty, a small desk companion. Talk like a person in conversation:", 0) == 0);
  CHECK("instruction: always English", noPlace.find("Always speak English") != std::string::npos);
  return failures ? 1 : 0;
}
