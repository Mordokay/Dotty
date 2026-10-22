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
  config_.resumeHandle.clear();
  return open();
}

bool GeminiLive::open() {
  ready_ = false;
  turn_ = speech_ = goAway_ = false;
  tokens_ = 0;
  String error;
  const std::string path = std::string(kPath) + key_;
  const uint32_t start = millis();
  if (!ws_.connect(kHost, path.c_str(), error)) {
    LOGW("live", "connect: %s", error.c_str());
    if (cb_.failed) cb_.failed(error.c_str(), -1, false, false);
    return false;
  }
  LOGI("live", "connected in %lu ms%s", millis() - start, config_.resumeHandle.empty() ? "" : " (resuming)");
  ws_.sendText(liveSetup(config_));
  return true;
}

void GeminiLive::startTurn() {
  turn_ = true;
  speech_ = false;
  tokens_ = 0;
  ws_.sendText(liveActivityStart());
}

void GeminiLive::sendAudio(const int16_t *pcm, size_t frames) {
  ws_.sendText(liveAudio(pcm, frames));
}

void GeminiLive::endTurn() {
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
  turn_ = true;
  speech_ = false;
  tokens_ = 0;
  ws_.sendText(out);
}

void GeminiLive::poll() {
  if (!ws_.connected()) return;
  const bool alive = ws_.poll([this](const std::string &m) { onMessage(m); });
  if (alive) {
    if (goAway_ && !turn_) {  // the server is ending this connection: carry on in a new one
      ws_.close();
      LOGI("live", "goAway: reconnecting");
      open();
    }
    return;
  }
  const LiveClose why = liveCloseInfo(ws_.closeCode(), ws_.closeReason());
  ready_ = false;
  const bool wasTurn = turn_;
  turn_ = false;
  LOGW("live", "closed %u: %s", ws_.closeCode(), why.message.c_str());
  // Dropped between turns with a session to resume (e.g. right after a goAway): reconnect quietly.
  if (!wasTurn && !why.quota && !why.badKey && !config_.resumeHandle.empty() && goAway_) {
    open();
    return;
  }
  if (cb_.failed) cb_.failed(why.message.empty() ? "connection lost" : why.message, why.retryDelaySeconds, why.quota, why.badKey);
}

void GeminiLive::close() {
  ws_.close();
  ready_ = false;
  turn_ = false;
}

void GeminiLive::onMessage(const std::string &message) {
  const LiveEvent e = liveParse(message);
  if (e.setupComplete) {
    ready_ = true;
    LOGI("live", "ready");
    if (cb_.ready) cb_.ready();
  }
  if (!e.resumeHandle.empty()) config_.resumeHandle = e.resumeHandle;
  if (e.goAway) goAway_ = true;
  if (!e.toolCallId.empty()) {
    LOGI("live", "%s(%s)", e.toolName.c_str(), e.mood.c_str());
    ws_.sendText(liveToolOk(e.toolCallId, e.toolName));
    if (!e.mood.empty() && cb_.mood) cb_.mood(e.mood);
  }
  if (!e.inputText.empty() && cb_.heard) cb_.heard(e.inputText);
  if (!e.outputText.empty() && cb_.said) cb_.said(e.outputText);
  if (!e.text.empty()) {
    speech_ = true;
    if (cb_.said) cb_.said(e.text);
  }
  if (!e.audioPcm.empty()) {
    speech_ = true;
    if (cb_.audio) cb_.audio(reinterpret_cast<const int16_t *>(e.audioPcm.data()), e.audioPcm.size() / 2);
  }
  if (e.totalTokens > 0) tokens_ += e.totalTokens;
  if (e.turnComplete) LOGI("live", "turn complete%s (%d tokens)", speech_ ? "" : " without speech", e.totalTokens);
  if (e.interrupted) LOGI("live", "interrupted");
  if (e.errorCode) LOGW("live", "error %d", e.errorCode);
  // A turn with only a set_mood call isn't the reply: the speech follows in the next turn.
  if (e.turnComplete && turn_ && speech_) {
    turn_ = false;
    if (cb_.turnDone) cb_.turnDone(tokens_);
  }
}
