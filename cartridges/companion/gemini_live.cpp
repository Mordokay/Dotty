#include "gemini_live.h"

#include <ArduinoJson.h>

#include "log.h"

namespace {
constexpr const char *kHost = "generativelanguage.googleapis.com";
constexpr const char *kPath = "/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=";
}  // namespace

bool GeminiLive::begin(const std::string &apiKey, const LiveConfig &config, Callbacks callbacks) {
  key_ = apiKey;
  config_ = config;
  cb_ = std::move(callbacks);
  return open();
}

bool GeminiLive::open() {
  ready_ = false;
  goAway_ = false;
  turns_ = TurnTracker();
  String error;
  const std::string path = std::string(kPath) + key_;
  const uint32_t start = millis();
  if (!ws_.connect(kHost, path.c_str(), error)) {
    LOGW("live", "connect: %s", error.c_str());
    if (cb_.failed) cb_.failed(error.c_str(), -1, false, false);
    return false;
  }
  lastMessageAt_ = millis();
  LOGI("live", "connected in %lu ms%s", millis() - start, config_.resumeHandle.empty() ? "" : " (resuming)");
  ws_.sendText(liveSetup(config_));
  return true;
}

void GeminiLive::startTurn() {
  turns_.start();
  lastMessageAt_ = millis();
  ws_.sendText(liveActivityStart());
}

void GeminiLive::sendAudio(const int16_t *pcm, size_t frames) {
  ws_.sendText(liveAudio(pcm, frames));
}

void GeminiLive::endTurn() {
  lastMessageAt_ = millis();  // the reply's silence counts from here
  ws_.sendText(liveActivityEnd());
}

void GeminiLive::sendText(const std::string &text) {
  JsonDocument doc;
  JsonObject turn = doc["clientContent"]["turns"].add<JsonObject>();
  turn["role"] = "user";
  turn["parts"][0]["text"] = text;
  doc["clientContent"]["turnComplete"] = true;
  std::string out;
  serializeJson(doc, out);
  turns_.start();
  lastMessageAt_ = millis();
  ws_.sendText(out);
}

void GeminiLive::poll() {
  if (!ws_.connected()) return;
  const bool alive = ws_.poll([this](const std::string &m) { onMessage(m); });
  if (!ws_.connected()) return;  // a message handler closed it (runaway guard)
  if (alive) {
    // The server is ending this connection: close after the reply; the next press opens a new
    // one with the resumption handle (reconnecting here froze the screen for seconds).
    if (goAway_ && !turns_.turn) {
      LOGI("live", "goAway: closing, the next press resumes");
      ws_.close();
      ready_ = false;
    }
    return;
  }
  const LiveClose why = liveCloseInfo(ws_.closeCode(), ws_.closeReason());
  ready_ = false;
  turns_ = TurnTracker();
  LOGW("live", "closed %u: %s", ws_.closeCode(), why.message.c_str());
  if (cb_.failed) cb_.failed(why.message.empty() ? "connection lost" : why.message, why.retryDelaySeconds, why.quota, why.badKey);
}

void GeminiLive::close() {
  ws_.close();
  ready_ = false;
  turns_ = TurnTracker();
}

void GeminiLive::onMessage(const std::string &message) {
  lastMessageAt_ = millis();
  const LiveEvent e = liveParse(message);
  if (e.setupComplete) {
    ready_ = true;
    LOGI("live", "ready");
    if (cb_.ready) cb_.ready();
  }
  if (!e.resumeHandle.empty()) config_.resumeHandle = e.resumeHandle;
  if (e.goAway) goAway_ = true;
  // Your words belong to the question being asked, whatever is still draining.
  if (!e.inputText.empty() && cb_.heard) cb_.heard(e.inputText);
  const bool content = !e.outputText.empty() || !e.text.empty() || !e.audioPcm.empty();
  const TurnTracker::Out o = turns_.message(content, !e.toolCallId.empty(), e.turnComplete, e.totalTokens);
  if (!e.toolCallId.empty()) {
    LOGI("live", "%s(%s)%s", e.toolName.c_str(), e.mood.c_str(), o.answerTool ? "" : " - not answered");
    if (o.answerTool) ws_.sendText(liveToolOk(e.toolCallId, e.toolName));
    if (!ws_.connected()) return;  // the send failed and dropped the connection
    if (o.showMood && !e.mood.empty() && cb_.mood) cb_.mood(e.mood);
  }
  if (o.deliver) {
    if (!e.outputText.empty() && cb_.said) cb_.said(e.outputText);
    if (!e.text.empty() && cb_.said) cb_.said(e.text);
    if (!e.audioPcm.empty() && cb_.audio)
      cb_.audio(reinterpret_cast<const int16_t *>(e.audioPcm.data()), e.audioPcm.size() / 2);
  }
  if (e.turnComplete) LOGI("live", "turn complete (%d tokens)%s", e.totalTokens, o.done ? "" : " - not the reply");
  if (e.interrupted) LOGI("live", "interrupted");
  if (e.errorCode) LOGW("live", "error %d", e.errorCode);
  if (o.spent && cb_.spent) cb_.spent(o.spent);
  if (o.close) {  // turns nobody asked for (a loop on Google's side) cost tokens and go nowhere
    LOGW("live", "turns nobody asked for: closing the session");
    close();
    return;
  }
  if (o.done && cb_.turnDone) cb_.turnDone(o.tokens);
}
