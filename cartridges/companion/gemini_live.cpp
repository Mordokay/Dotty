#include "gemini_live.h"

#include <ArduinoJson.h>

#include "log.h"

bool GeminiLive::begin(const std::string &apiKey, const LiveConfig &config, Callbacks callbacks, LiveTransport &transport) {
  t_ = &transport;
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
  const uint32_t start = millis();
  if (!t_->open(key_, liveSetup(config_), config_.resumeHandle, error)) {
    LOGW("live", "connect: %s", error.c_str());
    if (cb_.failed) cb_.failed(error.c_str(), -1, false, false);
    return false;
  }
  lastMessageAt_ = millis();
  LOGI("live", "connected in %lu ms%s%s", millis() - start, overRelay() ? " through the iPhone" : "",
       config_.resumeHandle.empty() ? "" : " (resuming)");
  return true;
}

void GeminiLive::startTurn() {
  turns_.start();
  turns_.speaking(true);
  lastMessageAt_ = millis();
  t_->sendText(liveActivityStart());
}

void GeminiLive::sendAudio(const int16_t *pcm, size_t frames) {
  t_->sendVoice(pcm, frames);
}

void GeminiLive::endTurn() {
  lastMessageAt_ = millis();  // the reply's silence counts from here
  turns_.speaking(false);
  t_->flushVoice();
  t_->sendText(liveActivityEnd());
}

void GeminiLive::answerTool(const std::string &id, const std::string &name, const std::string &result) {
  if (!turns_.turn) turns_.start();  // still open (nothing said yet): the reply is that question's
  turns_.speaking(false);
  lastMessageAt_ = millis();
  t_->sendText(liveToolResult(id, name, result));
}

void GeminiLive::sendText(const std::string &text, bool continueQuestion) {
  JsonDocument doc;
  JsonObject turn = doc["clientContent"]["turns"].add<JsonObject>();
  turn["role"] = "user";
  turn["parts"][0]["text"] = text;
  doc["clientContent"]["turnComplete"] = true;
  std::string out;
  serializeJson(doc, out);
  if (!continueQuestion || !turns_.turn) turns_.start();
  turns_.speaking(false);
  lastMessageAt_ = millis();
  t_->sendText(out);
}

void GeminiLive::poll() {
  if (!t_->connected()) return;
  const bool alive = t_->poll([this](const std::string &m) { onMessage(m); },
                              [this](const int16_t *pcm, size_t frames) { onAudio(pcm, frames); });
  // poll() returns true when a message handler closed the socket (runaway guard): nothing to
  // report. false = the server closed it: its reason (quota, bad key) must reach `failed` (an
  // early return here once skipped it, and a quota refusal showed as a plain failed connect).
  if (alive && !t_->connected()) return;
  if (alive) {
    // The server is ending this connection: close after the reply; the next press opens a new
    // one with the resumption handle (reconnecting here froze the screen for seconds).
    if (goAway_ && !turns_.turn) {
      LOGI("live", "goAway: closing, the next press resumes");
      t_->close();
      ready_ = false;
    }
    return;
  }
  // iOS cuts a background app's sockets once it's suspended (35-90 s after a reply): over the relay
  // that's 1006 with nobody talking. Not an error: the next press resumes the conversation.
  if (overRelay() && !turns_.turn && t_->closeCode() == 1006 && !config_.resumeHandle.empty()) {
    LOGI("live", "the iPhone dropped the idle connection: the next press resumes");
    ready_ = false;
    goAway_ = true;
    turns_ = TurnTracker();
    return;
  }
  const LiveClose why = liveCloseInfo(t_->closeCode(), t_->closeReason());
  ready_ = false;
  turns_ = TurnTracker();
  LOGW("live", "closed %u: %s", t_->closeCode(), why.message.c_str());
  if (cb_.failed) cb_.failed(why.message.empty() ? "connection lost" : why.message, why.retryDelaySeconds, why.quota, why.badKey);
}

void GeminiLive::close() {
  t_->close();
  ready_ = false;
  turns_ = TurnTracker();
}

// Tools whose answer Dotty writes (cb_.tool): silent "ok" or a result the model talks about.
static bool isMomentTool(const std::string &name) {
  return name == "evolve" || name == "set_moment" || name == "cancel_moment" || name == "list_moments" || name == "start_timer" ||
         name == "stop_timer" || name == "start_recording" || name == "play_recording" || name == "delete_recording" ||
         name == "show_recordings" || name == "new_conversation";
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
  std::vector<std::string> names;
  for (const ToolCall &c : e.calls) names.push_back(c.name);
  const TurnTracker::Out o = turns_.message(content, names, e.turnComplete, e.totalTokens);
  std::vector<ToolCall> answered, spoken;
  std::vector<ToolCall> calls = e.calls;
  for (size_t i = 0; i < calls.size(); i++) {
    ToolCall &c = calls[i];
    LOGI("live", "%s(%s)%s%s", c.name.c_str(), c.name == "set_mood" ? c.mood.c_str() : c.group.c_str(),
         o.apply[i] ? "" : " - not applied", o.apply[i] && !o.answer[i] ? " - not answered" : "");
    if (o.apply[i]) {
      if (c.name == "set_mood") {
        if (!c.mood.empty() && cb_.mood) cb_.mood(c.mood);
      } else if (isMomentTool(c.name)) {
        const ToolAnswer a = cb_.tool ? cb_.tool(c) : ToolAnswer();
        c.result = a.result;
        if (a.speak) {
          spoken.push_back(c);
          continue;
        }
      } else if (cb_.memory) {
        c.resultId = cb_.memory(c);
      }
    }
    if (o.answer[i]) answered.push_back(c);
  }
  if (!answered.empty()) {
    t_->sendText(liveToolsOk(answered));
    if (!t_->connected()) return;  // the send failed and dropped the connection
  }
  for (const ToolCall &c : spoken) {  // a result to talk about (WHEN_IDLE): its reply belongs to the question
    answerTool(c.id, c.name, c.result);
    if (!t_->connected()) return;
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

// The relay hands the reply audio over decoded, in order with the JSON: it's the content of the turn
// it arrives in, like inline audio over the WebSocket.
void GeminiLive::onAudio(const int16_t *pcm, size_t frames) {
  lastMessageAt_ = millis();
  const TurnTracker::Out o = turns_.message(true, {}, false, 0);
  if (o.deliver && cb_.audio) cb_.audio(pcm, frames);
}
