#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

// The Gemini Live API's messages (https://ai.google.dev/gemini-api/docs/live), with no I/O so
// they can be tested on a computer (tools/companion_test, against messages captured from a real
// session). gemini_live.h sends and receives them over the WebSocket.
//
// What the server does, as captured on 2026-10-22: set_mood comes in a turn of its own (a
// toolCall, then turnComplete with no audio); after Dotty's toolResponse the speech comes as a
// second turn (modelTurn audio pieces with outputTranscription text, generationComplete, then
// turnComplete with usageMetadata). Errors (quota, a bad key) close the WebSocket with a reason.

struct LiveConfig {
  std::string model = "models/gemini-3.8-live";
  std::string voice = "Zephyr";
  std::string systemInstruction;
  std::string resumeHandle;  // continue an earlier session ("" = new)
  bool textOnly = false;     // replies as text instead of audio
};

std::string liveSetup(const LiveConfig &config);
std::string liveActivityStart();
std::string liveActivityEnd();
// 16 kHz 16-bit mono PCM, base64 in a realtimeInput message.
std::string liveAudio(const int16_t *pcm, size_t frames);
// Dotty's answer to a function call: done, and don't start a new reply because of it.
std::string liveToolOk(const std::string &callId, const std::string &name);

struct LiveEvent {
  bool setupComplete = false, turnComplete = false, generationComplete = false, interrupted = false, goAway = false;
  std::string audioPcm;    // decoded 24 kHz 16-bit mono PCM bytes in this message
  std::string inputText;   // the user's words (transcript piece)
  std::string outputText;  // the reply's words (transcript piece)
  std::string text;        // a text-only reply's text piece
  std::string toolCallId, toolName, mood;  // a set_mood call
  std::string resumeHandle;
  int totalTokens = -1;
  int errorCode = 0;
  int retryDelaySeconds = -1;
};
LiveEvent liveParse(const std::string &message);

// Why the server closed the WebSocket (its close code and reason text).
struct LiveClose {
  bool quota = false;   // the free allowance (or a rate limit) ran out
  bool badKey = false;  // the API key was refused
  int retryDelaySeconds = -1;
  std::string message;
};
LiveClose liveCloseInfo(uint16_t code, const std::string &reason);

// The companion's instructions; place "" leaves the place out.
std::string systemInstruction(const std::string &localTime, const std::string &place);

std::string base64Decode(const char *data, size_t len);
