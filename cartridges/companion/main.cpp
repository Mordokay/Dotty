// Companion: talk to an AI (Gemini Live) by holding BOOT; it answers out loud in about a second,
// with the text on screen and an animated avatar. Spec:
// docs/superpowers/specs/2026-10-22-companion-talking-design.md.
//
// Opening the cartridge or unlocking connects in the background (Wi-Fi, TLS, setup: ~10 s, in a
// task of its own so the screen stays alive; Bluetooth is off meanwhile: TLS needs its RAM).
// Hold BOOT (≥ 120 ms) = listen, your voice streams up in 100 ms chunks; release = the reply
// streams back and plays as it arrives. BOOT while it speaks interrupts it. 3 minutes without a
// turn, or locking, disconnects (Bluetooth back on). Top-left corner = text-only replies,
// top-right = today's chats.

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <time.h>

#include <map>
#include <string>
#include <vector>

#include "audio_player.h"
#include "avatar.h"
#include "board_pins.h"
#include "cartridge.h"
#include "core_ble.h"
#include "gemini_live.h"
#include "history.h"
#include "companions.h"
#include "key_ring.h"
#include "last_line.h"
#include "macro_store.h"
#include "recall.h"
#include "script_writer.h"
#include "piece_buffer.h"
#include "ui.h"
#include "memories.h"
#include "persona.h"
#include "images/sleep_panda.h"
#include "log.h"
#include "nav_bar.h"
#include "net.h"
#include "power.h"
#include "screens.h"
#include "shell.h"
#include "storage.h"
#include "usage_rules.h"
#include "voice.h"
#include "voice_pack.h"
#include <Fonts/FreeSans9pt7b.h>
#include "voices.h"
#include <SD_MMC.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#if __has_include("dev_key.h")
#include "dev_key.h"  // bench only (gitignored): COMPANION_DEV_KEY seeds an empty key
#endif

DOTTY_CARTRIDGE("companion", "Companion", "0.3.0");

namespace {

using shell::epd;

constexpr uint32_t kHoldMs = 120;                   // BOOT held this long = listen (as in Tape)
constexpr uint32_t kIdleMs = 3 * 60 * 1000;         // no turn for this long: disconnect
constexpr uint32_t kAfterMs = 4000;                 // the mood face after a reply
constexpr uint32_t kFrameMs = 1500;                 // the avatar's animation clock
constexpr uint32_t kErrorMs = 6000;                 // a connection error shows this long
constexpr uint32_t kFullRefreshMs = 2 * 60 * 1000;  // clear ghosting (as the Pet does)

AudioPlayer player;
GeminiLive live;

// Settings (NVS companion; set from the app with companion.settings).
struct Settings {
  std::vector<String> keys;  // up to 3 Gemini keys, one per session in turn (KeyRing)
  String voice = "Zephyr", avatar = "robot";
  bool textOnly = false;
  int volume = 70;
  String favourites = kDefaultFavourites;  // the voices Dotty steps through (picked in the app)
} settings;
bool settingsOpen = false;
bool settingsChanged = false;  // voice or speech changed: the session restarts with them
KeyRing ring;                  // which key the next session uses, and which are out for now
int sessionKey = -1;           // the key this session opened with (a goAway resume keeps it)

ScreenInfo info;
bool redraw = true;
bool historyOpen = false;
int historyScroll = 0;

uint32_t lastTurnAt = 0, stateSince = 0, releasedAt = 0, firstAudioAt = 0, lastFrameAt = 0, lastFullAt = 0;
// Talking before the session is ready (it takes ~10 s to connect): the voice is kept here and
// sent the moment it is, so you never wait to speak, only for the reply.
std::string early;            // PCM bytes recorded before the turn could open (PSRAM: > 16 KB)
constexpr size_t kEarlyMaxBytes = 60 * 16000 * 2;  // a minute
size_t earlyAt = 0;           // how much of `early` has gone out (a few chunks per loop pass)
bool turnSent = false;        // activityStart went out for this recording
bool awaitingSend = false;    // released before the session was ready: send it all then
bool awaitingConnect = false; // a connect is under way (the screen may be listening meanwhile)
bool simulatedHold = false;  // dev key r
std::string heard, said;    // this turn (std::string: they also go to history)
String turnMood = "neutral";
// The reply's text arrives faster than its voice plays: each piece waits until the speaker has
// played the audio that had arrived with it, so the words appear as they're spoken.
struct TextPiece {
  size_t atFrame;
  std::string text;
};
std::vector<TextPiece> textQueue;
size_t replyFrames = 0;  // reply audio received so far
uint32_t lastTextAt = 0;
std::string turnMemory;        // what this exchange changed in the memories (history)
std::string memorySection;     // the memories for the next session (built on the loop, read by the task)
std::string personaSection;    // the companion's persona for the next session (same)
std::string sessionCompanion;  // whose session is open: memory and name calls only land on them
String sessionKeyText;         // its key (copied for the connect task)
String sessionVoice;           // its voice (same: the settings screen may change settings.voice meanwhile)
uint32_t noteUntil = 0;        // "Remembered"… shows until then
// Bluetooth comes back a moment after a session's Wi-Fi is off: once, with it started in the same
// instant, the iPhone's reconnect hung the Bluetooth controller (interrupt watchdog in
// ll_connection_update_ind_handler, 2026-10-22).
uint32_t bleBackAt = 0;
// The last thing said, for replay (per companion: last.wav + last.json in its folder). The reply's
// audio is copied as it arrives (up to a minute, like the voice backlog) and saved once it has played.
constexpr size_t kLastMaxFrames = 60 * 24000;
int16_t *lastRec = nullptr;  // PSRAM, only while a reply is being kept
size_t lastRecFrames = 0;
LastLine lastPending;  // this reply's text pieces
LastLine lastLine;     // the active companion's saved one
bool hasLast = false;
Replay replay;
std::string replayShown;  // the replay's text on screen
int historyTurns = 0, countedTurns = -1;
// New exchanges since the app last heard: told (companion.history) once Bluetooth is back and the
// app has reconnected, so an open chat screen reloads (near-live: Bluetooth is off during a chat).
bool historyNews = false;
std::string historyNewsCompanion;  // turns appended to history, and when talksToday last counted
// A long answer (write_long) or a look back (recall): a text model works in a task (a second TLS
// connection), then Live reads the text aloud word for word / talks about what was found. The
// follow-up goes out once the question's turn and the job are both done (or it has been quiet for a
// moment: a recall may come without words); the speaker's stream stays open, so it's one reply.
MacroList macroList;  // the user's macros (data/macros.json), shared by every companion
enum class Job { Write, Recall };
Job jobKind = Job::Write;
std::string jobCallId;           // recall's function call, answered with what was found
bool writing = false;            // a job asked, its follow-up not sent yet
bool reading = false;            // the reading turn is under way
bool readingStarts = false;      // its first text piece is next
bool questionDone = false;       // the question's own turn (often "Give me a moment") has ended
volatile bool written = false;   // the task has finished (writtenResult is ready)
volatile uint32_t writerJob = 0; // a job's number: BOOT or a disconnect makes a running one stale
Written writtenResult;
std::string writerBody;          // the request, built on the loop for the task
String writerKey;
int longTokens = 0;              // the question's tokens (the exchange is counted once, at its end)
bool keepBle = false;  // dev T: Bluetooth stays on during sessions (the soak test)
// The session's lowest internal RAM (sampled every 250 ms while connected), logged when it closes.
size_t lowFree = SIZE_MAX, lowLargest = SIZE_MAX;
uint32_t lastHeapSample = 0;
bool evolvedThisChat = false;  // one change per chat (session): companions evolve slowly
uint32_t reviewStart = 0;      // the end-of-chat memory review was sent (0 = not reviewing)
uint32_t longestPause = 0;     // the longest loop pass while speaking (choppy replies: was the loop stuck?)
size_t sentFrames = 0;  // this turn's voice, for the log
int peakDb = -60;

// The background connect (a task: TLS takes seconds and must not freeze the screen).
TaskHandle_t connectTask = nullptr;
volatile bool connecting = false;
volatile bool cancelConnect = false;  // disconnected while connecting: the task undoes it
String connectError;

void setState(Talk s, const String &status = String()) {
  info.state = s;
  info.status = status;
  stateSince = millis();
  redraw = true;
}

void loadSettings() {
  Preferences p;
  p.begin("companion", true);
  settings.keys.clear();
  JsonDocument keys;
  if (deserializeJson(keys, p.getString("keys", "[]")) == DeserializationError::Ok)
    for (JsonVariant k : keys.as<JsonArray>())
      if (settings.keys.size() < KeyRing::kMax && strlen(k | "")) settings.keys.push_back(k.as<const char *>());
  if (settings.keys.empty() && p.getString("key", "").length()) settings.keys.push_back(p.getString("key", ""));  // 0.2.0
  std::vector<int64_t> outs;
  for (int i = 0; i < KeyRing::kMax; i++) outs.push_back(p.getLong64(("keyOut" + String(i)).c_str(), 0));
  const int keyNext = p.getInt("keyNext", 0);
  settings.voice = p.getString("voice", "Zephyr");
  settings.avatar = p.getString("avatar", "robot");
  if (!avatar::known(settings.avatar)) settings.avatar = "robot";
  settings.textOnly = p.getBool("textOnly", false);
  settings.volume = p.getInt("volume", 70);
  settings.favourites = p.getString("favourites", kDefaultFavourites);
  p.end();
  if (voiceIndex(settings.voice.c_str()) < 0) settings.voice = kVoices[0].name;
  player.setVolume(settings.volume);
#ifdef COMPANION_DEV_KEY
  if (settings.keys.empty()) settings.keys.push_back(COMPANION_DEV_KEY);
#endif
  ring.setCount(settings.keys.size());
  ring.restore(keyNext, outs);
  info.textOnly = settings.textOnly;
  info.avatar = settings.avatar;
}

// The keys, which one comes next and which are out, kept across restarts.
void saveKeys() {
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  for (const String &k : settings.keys) list.add(k);
  String json;
  serializeJson(doc, json);
  Preferences p;
  p.begin("companion", false);
  p.putString("keys", json);
  p.remove("key");
  p.putInt("keyNext", ring.next());
  for (int i = 0; i < KeyRing::kMax; i++) p.putLong64(("keyOut" + String(i)).c_str(), ring.outUntil(i));
  p.end();
}

String keyHint(const String &k) {  // only the last 4 characters ever leave Dotty
  return k.length() >= 4 ? k.substring(k.length() - 4) : String("");
}

int64_t nowUtc() {
  int32_t offset = 0;
  shell::utcOffset(offset);
  return static_cast<int64_t>(time(nullptr)) - offset;
}

// "08:00" for a UTC instant, in local time.
String localClock(int64_t utc) {
  int32_t offset = 0;
  shell::utcOffset(offset);
  const time_t local = utc + offset;
  tm t;
  gmtime_r(&local, &t);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  return buf;
}

String outOfWords(int64_t untilUtc) {
  return "I'm out of words for today. Back at " + localClock(untilUtc) + ".";
}

String placeName() {
  Preferences p;
  p.begin("weather", true);
  const String name = p.getString("name", "");
  p.end();
  return name;
}

void cancelWriter();
void startWriter(const std::string &what, int seconds);
void startRecall(const ToolCall &c);
void startMacro(const Macro &m);
void loadMacros();

void disconnect(const char *why) {
  reviewStart = 0;
  cancelWriter();
  early.clear();
  earlyAt = 0;
  turnSent = awaitingSend = awaitingConnect = false;
  voice::stopListening();
  voice::playStop();
  if (connecting) {  // the connect task is using the socket and Wi-Fi: it closes them when done
    cancelConnect = true;
    LOGI("companion", "connect cancelled (%s)", why);
    return;
  }
  if (lowFree != SIZE_MAX)
    LOGI("companion", "session low: internal %u KB free, largest %u KB (Bluetooth %s)", lowFree / 1024, lowLargest / 1024,
         keepBle ? "on" : "off");
  lowFree = lowLargest = SIZE_MAX;
  live.close();
  net::disconnect();
  power::setWakeLock(power::kWakeLockNetwork, false);
  bleBackAt = millis() + 1000;  // Bluetooth back a second after Wi-Fi is off (see bleBackAt)
  LOGI("companion", "disconnected (%s)", why);
}

void showText(const std::string &t) {
  said += t;
  info.said = said.c_str();
  redraw = true;
}

// Moves the pieces whose audio has now been heard onto the screen (all of them with `all`).
void releaseText(bool all) {
  const size_t played = voice::playedFrames();
  size_t n = 0;
  while (n < textQueue.size() && (all || textQueue[n].atFrame <= played)) n++;
  if (!n) return;
  for (size_t i = 0; i < n; i++) said += textQueue[i].text;
  textQueue.erase(textQueue.begin(), textQueue.begin() + n);
  info.said = said.c_str();
  redraw = true;
}

// The first piece of a reply: time it, start the speaker (once per reply).
void beginReply() {
  if (firstAudioAt) return;
  firstAudioAt = millis();
  LOGI("companion", "first %s %lu ms after release", settings.textOnly ? "text" : "audio", firstAudioAt - releasedAt);
  if (!settings.textOnly) voice::playStart();
  setState(Talk::Speaking);
}

String idleStatus();
void applyCompanion();
void stopReplay();
void notifyReplay();
void beginLast();
void saveLast();
bool canReplay();
void toggleReplay();
void setReplayLoop(bool on);
void pollReplay();
void loadLast();
void notifyChanged();

// Another companion: its own persona, voice, avatar, memories and history from the next session.
void switchCompanion(const std::string &id) {
  if (id == companions::active().id || !companions::find(id)) return;
  if (live.connected() || connecting) disconnect("companion changed");
  live.forgetResume();  // a goAway handle holds the old companion's conversation (and its memory numbers)
  stopReplay();
  companions::select(id);
  applyCompanion();
  loadLast();  // its own last line
  setState(Talk::Offline, idleStatus());
  notifyChanged();  // the app's pager follows a switch made on Dotty
}

// The idle line under the avatar: a companion that hasn't been named yet is still asleep.
// A name for the screen: the e-paper fonts are ASCII (ui::printable), and a name with nothing
// printable at all (Korean, emoji) shows as "Companion".
String screenName(const Profile &p) {
  const String n = ui::printable(p.name.c_str());
  return n.length() ? n : String("Companion");
}

String idleStatus() {
  return companions::active().awake() ? "Hold BOOT to talk" : "Not awake yet. Hold BOOT";
}

// A photo avatar's two circles (PSRAM), read from the active companion's photo.bin.
uint8_t *photo = nullptr;

void loadPhoto() {
  avatar::setPhoto(nullptr, nullptr);
  if (companions::active().avatar != "photo") return;
  if (!photo) photo = static_cast<uint8_t *>(heap_caps_malloc(avatar::kPhotoBig + avatar::kPhotoSmall, MALLOC_CAP_SPIRAM));
  File f = SD_MMC.open(companions::folder(companions::active().id) + "/photo.bin");
  static uint8_t chunk[512];
  size_t got = 0;
  while (f && photo && got < avatar::kPhotoBig + avatar::kPhotoSmall) {
    const int n = f.read(chunk, min(sizeof(chunk), avatar::kPhotoBig + avatar::kPhotoSmall - got));
    if (n <= 0) break;
    memcpy(photo + got, chunk, n);
    got += n;
  }
  f.close();
  if (got == avatar::kPhotoBig + avatar::kPhotoSmall) avatar::setPhoto(photo, photo + avatar::kPhotoBig);
  else LOGW("companion", "photo.bin missing or short (%u bytes): the robot instead", static_cast<unsigned>(got));
}

// The active companion's voice and avatar into the settings the screens and the session use.
void applyCompanion() {
  settings.voice = companions::active().voice.c_str();
  settings.avatar = companions::active().avatar.c_str();
  info.avatar = settings.avatar;
  loadPhoto();
  redraw = true;
}

// A voice or avatar changed (settings screen, app): it belongs to the active companion.
void syncProfile() {
  Profile p = companions::active();
  setField(p, "voice", settings.voice.c_str());
  setField(p, "avatar", settings.avatar.c_str());
  companions::save(p);
}

// A memory call from the model: change the store, save it, show a note for 3 s, note it for the
// history line. Returns the memory's id for remember (sent back so the model can name it).
int applyMemory(const ToolCall &c) {
  // Switched companion since this session opened: nothing lands on the new one.
  if (sessionCompanion != companions::active().id) return -1;
  MemoryStore &store = memories::store();
  const int64_t now = time(nullptr);
  int id = -1;
  String note;
  std::string change;
  if (c.name == "remember") {
    const MemoryStore::Added r = store.add(c.text, c.group, "dotty", now);
    if (r.ok) {
      id = r.id;
      note = "Remembered";
      change = "remembered: " + store.find(r.id)->text;
    } else if (r.error == "full") {
      note = "Memory full";
    }
  } else if (c.name == "update_memory") {
    const std::string *group = c.group.empty() ? nullptr : &c.group;
    if (store.update(c.memoryId, &c.text, group, now)) {
      id = c.memoryId;
      note = "Updated";
      change = "updated: " + store.find(c.memoryId)->text;
    }
  } else if (c.name == "set_name") {  // the first wake-up: it learns who it is
    Profile p = companions::active();
    if (setField(p, "name", c.text) && !p.name.empty() && companions::save(p)) {
      note = String("Named: ") + screenName(p);
      change = "named: " + p.name;
    }
  } else if (c.name == "evolve") {  // once a chat: mid-chat when told it changed, or in the review
    Profile p = companions::active();
    const std::string before = fieldText(p, c.field);
    if (!evolvedThisChat && applyEvolution(p, c.field, c.text) && fieldText(p, c.field) != before &&
        companions::save(p)) {
      evolvedThisChat = true;
      companions::logEvolution(p.id, evolutionLine(time(nullptr), c.field, before, fieldText(p, c.field), c.why));
      note = "Changed a little";
      change = "evolved " + c.field + ": " + c.why;
      notifyChanged();
    }
  } else if (c.name == "write_long") {  // a text model writes it, then Live reads it (see writerPoll)
    if (!writing && !reading) startWriter(c.text, c.seconds);
  } else if (c.name == "run_macro") {  // a prepared prompt through the writer, then read aloud
    const Macro *m = macroList.find(c.memoryId);
    if (m && !writing && !reading) startMacro(*m);
    if (m) change = "macro: " + m->name;
  } else if (c.name == "recall") {  // a text model reads its past chats (see writerPoll)
    if (!writing && !reading) startRecall(c);
  } else if (c.name == "summarize") {  // the end-of-chat review's few words about this chat
    if (reviewStart) {
      File f = SD_MMC.open(companions::folder(sessionCompanion) + "/summaries.jsonl", FILE_APPEND);
      if (f) f.println(summaryLine(time(nullptr), c.text).c_str());
      f.close();
      change = "summary: " + c.text;
    }
  } else if (c.name == "forget_memory") {
    const Memory *m = store.find(c.memoryId);
    const std::string text = m ? m->text : "";
    if (store.remove(c.memoryId)) {
      note = "Forgotten";
      change = "forgot: " + text;
    }
  }
  LOGI("companion", "%s %d: %s %.40s", c.name.c_str(), c.memoryId, note.length() ? note.c_str() : "ignored", change.c_str());
  if (!change.empty()) {
    if (c.name != "set_name" && c.name != "evolve" && c.name != "summarize") memories::save();
    turnMemory += (turnMemory.empty() ? "" : "; ") + change;
  }
  if (note.length()) {
    info.note = note;
    noteUntil = millis() + 3000;
    redraw = true;
  }
  return id;
}

// ---------- the writer (write_long) ----------

void writerTask(void *arg) {
  const uint32_t job = reinterpret_cast<uintptr_t>(arg);
  const String url = String("https://generativelanguage.googleapis.com/v1beta/models/") + kWriterModel +
                     ":generateContent?key=" + writerKey;
  std::string body;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 25000;
  config.event_handler = [](esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA) static_cast<std::string *>(e->user_data)->append(static_cast<const char *>(e->data), e->data_len);
    return ESP_OK;
  };
  config.user_data = &body;
  const uint32_t started = millis();
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, writerBody.c_str(), writerBody.size());
  const esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  Written w = err == ESP_OK ? parseWritten(status, body) : Written();
  if (err != ESP_OK) w.error = esp_err_to_name(err);
  LOGI(jobKind == Job::Recall ? "recall" : "writer", "%s in %lu ms: %u characters, %d tokens%s%s",
       w.ok ? (jobKind == Job::Recall ? w.text.substr(0, 120).c_str() : "written") : "failed",
       static_cast<unsigned long>(millis() - started), static_cast<unsigned>(w.text.size()), w.tokens,
       w.ok ? "" : ": ", w.error.c_str());
  if (job == writerJob) {  // nobody gave up on it meanwhile
    writtenResult = w;
    written = true;
  }
  vTaskDelete(nullptr);
}

void startJob() {
  writerKey = sessionKeyText;
  writing = true;
  written = false;
  questionDone = false;
  const uint32_t job = ++writerJob;
  if (xTaskCreatePinnedToCore(writerTask, "writer", 12288, reinterpret_cast<void *>(static_cast<uintptr_t>(job)), 1,
                              nullptr, 0) != pdPASS) {
    writtenResult = Written();
    writtenResult.error = "no task";
    written = true;
  }
  info.writing = jobKind == Job::Write;
  info.recalling = jobKind == Job::Recall;
  redraw = true;
}

void startWriter(const std::string &what, int seconds) {
  writerBody = writerRequest(companions::active(), memorySection, what, seconds);
  jobKind = Job::Write;
  LOGI("writer", "%d s: %.100s", seconds, what.c_str());
  startJob();
}

void startMacro(const Macro &m) {
  writerBody = macroRequest(companions::active(), memorySection, m);
  jobKind = Job::Write;
  LOGI("writer", "macro %d \"%s\"", m.id, m.name.c_str());
  startJob();
}

// Those days of this companion's own history, the topic's exchanges (with their neighbours), to
// the text model with the question.
void startRecall(const ToolCall &c) {
  const Profile &who = companions::active();
  const std::string name = who.awake() ? who.name : "the companion";
  const int64_t now = time(nullptr);
  std::vector<std::string> all;
  const std::vector<std::string> days = recallDays(c.from, c.to, now);
  for (const std::string &day : days)
    for (const std::string &e : exchanges(history::raw(day.c_str()), name)) all.push_back(e);
  const std::vector<std::string> kept = byTopic(all, c.group);
  writerBody = recallRequest(name, c.text, now, historyBlock(kept));
  jobKind = Job::Recall;
  jobCallId = c.id;
  LOGI("recall", "\"%.80s\": %s..%s (%u days), topic \"%s\": %u of %u exchanges, %u bytes", c.text.c_str(),
       days.front().c_str(), days.back().c_str(), static_cast<unsigned>(days.size()), c.group.c_str(),
       static_cast<unsigned>(kept.size()), static_cast<unsigned>(all.size()), static_cast<unsigned>(writerBody.size()));
  startJob();
}

void cancelWriter() {
  writerJob++;  // a running task's result is dropped
  writing = reading = questionDone = written = false;
  longTokens = 0;
  if (info.writing || info.recalling) redraw = true;
  info.writing = info.recalling = false;
}

// Both done (the text, and the question's own turn): Live reads it, or answers itself if it failed.
void writerPoll() {
  if (!writing || !written) return;
  // The question's own turn has ended; or it has said nothing at all and nothing has come for a
  // moment (a recall without words never ends its turn: the reply to the result is that question's).
  // Once it has started speaking ("Ooh, you're pushing my buttons"), wait for that turn to end: a
  // reading sent before it ended was taken for a turn nobody asked for, and dropped.
  const bool spoke = firstAudioAt || !said.empty() || !textQueue.empty();
  if (!questionDone && (spoke || millis() - live.lastMessageAt() < 1500)) return;
  writing = questionDone = written = false;
  info.writing = info.recalling = false;
  if (writtenResult.tokens) usage::addTokens(writtenResult.tokens, sessionKey);
  if (!live.ready()) return cancelWriter();
  reading = readingStarts = true;
  if (jobKind == Job::Recall) live.answerTool(jobCallId, "recall", recallResult(writtenResult));
  else live.sendText(writtenResult.ok ? readAloudNote(writtenResult.text) : writerFailedNote(), true);
  if (info.state != Talk::Speaking) setState(Talk::Thinking);
  lastTurnAt = millis();
  redraw = true;
}

// ---------- Gemini callbacks (run inside live.poll(), on the main loop) ----------

GeminiLive::Callbacks callbacks() {
  GeminiLive::Callbacks cb;
  cb.ready = [] {
    LOGI("companion", "ready; heap %u KB internal (largest %u)", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    lastTurnAt = millis();
    awaitingConnect = false;
    // A weak signal makes replies stutter (-78 dBm: 13 s to the first audio, refills mid-reply).
    const String status = net::rssi() < -75 ? String("Weak Wi-Fi: replies may stutter") : idleStatus();
    if (net::rssi() < -75) LOGW("companion", "weak Wi-Fi: %d dBm", net::rssi());
    if (info.state == Talk::Connecting) setState(Talk::Ready, status);  // else talking already
  };
  cb.heard = [](const std::string &t) {
    heard += t;
    info.heard = heard.c_str();
    redraw = true;
  };
  cb.said = [](const std::string &piece) {
    if (info.state != Talk::Thinking && info.state != Talk::Speaking) return;  // an interrupted reply
    // The reading's first words follow the question's own ("Give me a moment."): a space between.
    std::string t = piece;
    if (readingStarts && !said.empty() && !t.empty() && t[0] != ' ') t = " " + t;
    readingStarts = false;
    if (settings.textOnly) {
      showText(t);
      beginReply();
    } else {
      textQueue.push_back({replyFrames, t});
      lastPending.add(replyFrames / 24, t);  // 24 kHz: frames → ms
    }
  };
  cb.audio = [](const int16_t *pcm, size_t frames) {
    if (info.state != Talk::Thinking && info.state != Talk::Speaking) return;
    if (!voice::playing()) beginReply();
    voice::play(pcm, frames);
    replyFrames += frames;
    if (lastRec && lastRecFrames < kLastMaxFrames) {  // kept for replay (the first minute)
      const size_t n = min(frames, kLastMaxFrames - lastRecFrames);
      memcpy(lastRec + lastRecFrames, pcm, n * 2);
      lastRecFrames += n;
    }
  };
  cb.mood = [](const std::string &m) {
    turnMood = m.c_str();
    info.mood = turnMood;
  };
  cb.spent = [](int tokens) { usage::addTokens(tokens, sessionKey); };
  cb.memory = [](const ToolCall &c) { return applyMemory(c); };
  cb.turnDone = [](int tokens) {
    ring.forgive(sessionKey);  // a good turn: this key's quota strikes start over
    if (reviewStart) {  // the end-of-chat review: its memory calls are applied, nothing else to keep
      usage::addTokens(tokens, sessionKey);
      LOGI("companion", "memory review done (%d tokens)", tokens);
      disconnect("idle");
      reviewStart = 0;
      setState(Talk::Offline, idleStatus());
      return;
    }
    if (writing) {  // the question's turn ("Give me a moment"): the reading follows (writerPoll)
      questionDone = true;
      longTokens += tokens;
      lastTurnAt = millis();
      return;
    }
    reading = false;
    tokens += longTokens;
    longTokens = 0;
    voice::playEnd();
    std::string full = said;  // history gets the whole reply, even before it's all been heard
    for (const TextPiece &p : textQueue) full += p.text;
    const uint32_t ms = firstAudioAt ? firstAudioAt - releasedAt : millis() - releasedAt;
    history::append(heard, full, turnMood.c_str(), ms, tokens, turnMemory);
    historyTurns++;  // the lock screen counts its talks again
    historyNews = true;
    historyNewsCompanion = sessionCompanion;
    usage::addTurn(tokens, sessionKey);
    LOGI("companion", "turn: %u ms to first audio, %d tokens; heard \"%s\"; said \"%s\"", static_cast<unsigned>(ms),
         tokens, heard.c_str(), full.c_str());
    lastTurnAt = millis();
    info.page = -1;
    if (info.state == Talk::Thinking) setState(Talk::After);  // text-only replies end here
  };
  cb.failed = [](const std::string &error, int retry, bool quota, bool badKey) {
    if (connecting) {  // inside the connect task: the loop reports it once the task ends
      connectError = error.c_str();
      return;
    }
    early.clear();
    awaitingSend = awaitingConnect = false;
    voice::stopListening();
    voice::playStop();
    if (quota) {
      // Google says "quota" for short limits too (a burst of sessions while flashing): the first
      // one without a retry time waits a minute; a second in a row = the day's allowance.
      const bool daily = retry <= 0 && ring.strike(sessionKey) > 1;  // per key: others' good turns don't count
      const int64_t until = retry > 0 ? nowUtc() + retry : daily ? nextPacificMidnightUtc(nowUtc()) : nowUtc() + 60;
      ring.markOut(sessionKey, until);  // this key only: the next session takes another
      saveKeys();
      const int64_t allOut = ring.allOutUntil(nowUtc());
      if (allOut) usage::markOut(allOut);
      disconnect("quota");
      LOGW("companion", "key %d out until %lld (%s)", sessionKey, static_cast<long long>(until), daily ? "the day" : "a minute");
      if (allOut) setState(Talk::Error, outOfWords(allOut));
      else if (settings.keys.size() > 1) setState(Talk::Error, "That key is out for now. Hold BOOT: the next one takes over.");
      else setState(Talk::Error, String("Gemini is busy. Trying again in a minute."));
    } else if (badKey) {
      disconnect("bad key");
      setState(Talk::Error, "Key not accepted. Check it in the app.");
    } else {
      disconnect(error.c_str());
      setState(Talk::Error, "Connection lost");
    }
  };
  return cb;
}

// ---------- connecting ----------

void connectLoop(void *) {
  String error;
  bool ok = net::connect(error);
  if (!ok) {  // a join sometimes times out once and works straight after (seen at boot)
    LOGW("companion", "Wi-Fi: %s, trying again", error.c_str());
    error = "";
    ok = net::connect(error);
  }
  if (ok) {
    LiveConfig config;
    config.resumeHandle = live.resumeHandle();  // after a goAway: carry on the conversation
    config.voice = sessionVoice.c_str();
    config.textOnly = settings.textOnly;
    config.systemInstruction = systemInstruction(nowText(time(nullptr)), placeName().c_str(),
                                                 personaSection, memorySection);  // Task 4: the companion's persona
    ok = live.begin(sessionKeyText.c_str(), config, callbacks());
    if (!ok) error = "Can't reach Gemini";
  } else {
    LOGW("companion", "Wi-Fi: %s", error.c_str());
    error = "No Wi-Fi";
  }
  if (!ok && connectError.isEmpty()) connectError = error;
  if (!ok) LOGW("companion", "connect failed: %s", error.c_str());
  if (cancelConnect) {  // locked (or stopped) meanwhile: undo it here, the loop restores the rest
    live.close();
    net::disconnect();
  }
  connecting = false;
  connectTask = nullptr;
  vTaskDelete(nullptr);
}

void connect() {
  if (connecting || live.connected()) return;
  if (settings.keys.empty()) return setState(Talk::Error, "Key missing. Add it in the app.");
  // One key per session, in turn; a goAway resume stays on the key that holds the conversation.
  const bool resuming = !live.resumeHandle().empty() && sessionKey >= 0 && sessionKey < (int)settings.keys.size();
  const int key = resuming ? sessionKey : ring.pick(nowUtc());
  if (key < 0) return setState(Talk::Error, outOfWords(ring.allOutUntil(nowUtc())));
  sessionKey = key;
  sessionKeyText = settings.keys[key];
  sessionVoice = settings.voice;
  saveKeys();
  LOGI("companion", "session on key %d of %u (..%s)", key + 1, static_cast<unsigned>(settings.keys.size()), keyHint(sessionKeyText).c_str());
  // Bluetooth is off for the session: with it on (and the iPhone connected) TLS left 36 KB of
  // internal RAM, largest block 12 KB, and a session hung in its sends until the watchdog reset.
  // (mbedTLS in PSRAM changed that: dev key T keeps it on, for the soak test.)
  if (!keepBle) ble::stop();
  power::setWakeLock(power::kWakeLockNetwork, true);
  memorySection = memories::store().promptSection();  // the task only reads these copies
  {  // the last ten chats' summaries: what it has talked about lately
    File f = SD_MMC.open(companions::folder(companions::active().id) + "/summaries.jsonl");
    std::string sums;
    while (f && f.available()) sums += static_cast<char>(f.read());
    f.close();
    const std::string section = summariesSection(sums, 10);
    if (!section.empty()) memorySection += (memorySection.empty() ? "" : "\n\n") + section;
  }
  personaSection = personaText(companions::active());
  if (!macroList.all().empty()) personaSection += "\n\n" + macrosSection(macroList);  // its phrases → run_macro
  sessionCompanion = companions::active().id;
  evolvedThisChat = false;
  connecting = true;
  awaitingConnect = true;
  connectError = "";
  if (info.state != Talk::Listening && !awaitingSend) setState(Talk::Connecting, "Waking up...");  // else keep listening
  if (xTaskCreatePinnedToCore(connectLoop, "companion-connect", 12288, nullptr, 2, &connectTask, 1) != pdPASS) {
    connecting = false;
    setState(Talk::Error, "Out of memory");
  }
}

// ---------- a turn ----------

bool earlyPending() {
  return earlyAt < early.size();
}

// One recorded chunk: straight to Gemini once the turn is open and the backlog is out, else queued.
void sendVoice(const int16_t *pcm, size_t frames) {
  sentFrames += frames;
  if (turnSent && !earlyPending()) return live.sendAudio(pcm, frames);
  if (early.size() + frames * 2 > kEarlyMaxBytes) return;  // a minute is plenty
  early.append(reinterpret_cast<const char *>(pcm), frames * 2);
}

void openTurn() {
  live.startTurn();
  turnSent = true;
  earlyAt = 0;
  if (early.size()) LOGI("companion", "sending %.1f s recorded while connecting", early.size() / 2.0f / voice::kInRate);
}

// The backlog goes out a few chunks per loop pass: sending 7 s at once held the loop ~2 s and the
// microphone's buffer overflowed.
void flushEarly() {
  const size_t chunkBytes = voice::kChunkFrames * 2;
  for (int i = 0; i < 3 && earlyPending(); i++) {
    const size_t n = min(chunkBytes, early.size() - earlyAt);
    live.sendAudio(reinterpret_cast<const int16_t *>(early.data() + earlyAt), n / 2);
    earlyAt += n;
  }
  if (!earlyPending()) {
    early.clear();
    early.shrink_to_fit();
    earlyAt = 0;
  }
}

void startTalking() {
  heard.clear();
  said.clear();
  info.heard = info.said = "";
  turnMood = info.mood = "neutral";
  firstAudioAt = 0;
  reviewStart = 0;  // talking again: the end-of-chat review is dropped (its reply drains away)
  stopReplay();
  beginLast();
  cancelWriter();
  turnMemory.clear();
  textQueue.clear();
  replyFrames = 0;
  sentFrames = 0;
  peakDb = -60;
  early.clear();
  earlyAt = 0;
  turnSent = awaitingSend = false;
  if (!voice::startListening()) return;
  if (live.ready()) openTurn();
  else info.heard = "Listening... I'll send it once I'm connected.";
  setState(Talk::Listening);
}

void finishTurn() {
  live.endTurn();
  LOGI("companion", "sent %.1f s of voice, peak %d dBFS; heap %u KB internal (largest %u), %u KB PSRAM",
       sentFrames / float(voice::kInRate), peakDb, heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
       heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024, heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
  releasedAt = millis();
}

void stopTalking() {
  voice::stopListening();
  static int16_t chunk[voice::kChunkFrames];
  size_t frames = 0;
  while (voice::nextChunk(chunk, frames)) sendVoice(chunk, frames);
  if (turnSent && !earlyPending()) {
    finishTurn();
    setState(Talk::Thinking);
  } else {  // still connecting, or the backlog is going out: ends once it's all sent
    awaitingSend = true;
    info.heard = "";
    setState(Talk::Thinking, turnSent ? "" : "Connecting...");
  }
}

void loadHistory(std::vector<std::pair<String, String>> &out) {
  const std::vector<String> days = history::days();
  if (days.empty()) return;
  JsonDocument doc;
  if (deserializeJson(doc, history::day(days[0])) != DeserializationError::Ok) return;
  for (JsonObject t : doc.as<JsonArray>()) out.push_back({t["you"] | "", t["dotty"] | ""});
}

void saveSettings() {
  Preferences p;
  p.begin("companion", false);
  p.putString("voice", settings.voice);
  p.putBool("textOnly", settings.textOnly);
  p.putInt("volume", settings.volume);
  p.putString("favourites", settings.favourites);
  p.putString("avatar", settings.avatar);
  p.end();
}

SettingsView settingsView() {
  SettingsView v;
  v.volume = settings.volume;
  v.speak = !settings.textOnly;
  v.voice = settings.voice;
  v.style = kVoices[max(0, voiceIndex(settings.voice.c_str()))].style;
  v.choices = favouriteCount(settings.favourites.c_str());
  const Profile &who = companions::active();
  v.companion = who.awake() ? screenName(who) : String("Not awake yet");
  static const char *kTypes[][2] = {{"coach", "Coach"}, {"friend", "Friend"}, {"partner", "Partner"},
                                    {"teacher", "Teacher"}, {"debug", "Debug"}, {"custom", "Custom"}};
  for (auto &t : kTypes)
    if (who.type == t[0]) v.companionType = t[1];
  v.companions = companions::list().order().size();
  return v;
}

void closeSettings() {
  settingsOpen = false;
  redraw = true;
  if (!settingsChanged) return;
  settingsChanged = false;
  saveSettings();
  if (live.connected() && (info.state == Talk::Ready || info.state == Talk::After)) {
    disconnect("settings changed");  // fixed per session: the next press opens one with them
  }
}

void drawApp();

extern const uint8_t voicePackStart[] asm("_binary_cartridges_companion_voices_bin_start");
extern const uint8_t voicePackEnd[] asm("_binary_cartridges_companion_voices_bin_end");

// Plays a voice's sample (as the app does): copied from the firmware to the card when it isn't
// there yet (or differs), since the player reads files.
void playSample(const char *name) {
  if (voice::playing() || info.state == Talk::Listening || info.state == Talk::Thinking) return;
  const String dir = storage::myDataDir() + "/voices";
  const String path = dir + "/" + name + ".mp3";
  const VoicePack pack(voicePackStart, voicePackEnd - voicePackStart);
  const uint8_t *bytes = nullptr;
  size_t len = 0;
  if (!pack.find(name, bytes, len)) return LOGW("companion", "no sample for %s", name);
  // Is the card's copy this sample? Same size and same first bytes (a straight write from flash
  // once left a file of zeros with the right size).
  File existing = SD_MMC.open(path);
  uint8_t head[64] = {};
  const bool current = existing && existing.size() == len && existing.read(head, sizeof(head)) == sizeof(head) &&
                       memcmp(head, bytes, sizeof(head)) == 0;
  existing.close();
  if (!current) {
    SD_MMC.mkdir(dir);
    File f = SD_MMC.open(path + ".part", FILE_WRITE);
    // Through RAM: the card driver can't read the firmware's flash directly (it wrote zeros).
    static uint8_t chunk[4096];
    bool ok = static_cast<bool>(f);
    for (size_t at = 0; ok && at < len; at += sizeof(chunk)) {
      const size_t n = min(sizeof(chunk), len - at);
      memcpy(chunk, bytes + at, n);
      ok = f.write(chunk, n) == n;
    }
    f.close();
    SD_MMC.remove(path);
    if (!ok || !SD_MMC.rename(path + ".part", path)) return LOGW("companion", "couldn't save the %s sample", name);
  }
  player.play(path.c_str());
}

void onSettingsTap(uint16_t x, uint16_t y) {
  static const AudioPlayer::Note kBeep[] = {{76, 120}};
  switch (screens::settingsHit(x, y)) {
    case SettingsHit::Back:
      return closeSettings();
    case SettingsHit::CompanionPrev:
    case SettingsHit::CompanionNext: {  // the next session talks to them (any open one closes)
      const int step = screens::settingsHit(x, y) == SettingsHit::CompanionNext ? 1 : -1;
      switchCompanion(stepCompanion(companions::list().order(), companions::active().id, step));
      break;
    }
    case SettingsHit::VolumeDown:
    case SettingsHit::VolumeUp:
      settings.volume = constrain(settings.volume + (screens::settingsHit(x, y) == SettingsHit::VolumeUp ? 10 : -10), 0, 100);
      player.setVolume(settings.volume);
      saveSettings();
      if (!voice::playing() && info.state != Talk::Listening) player.playNotes(kBeep, 1);  // hear the level
      break;
    case SettingsHit::Speak:
      settings.textOnly = info.textOnly = !settings.textOnly;
      settingsChanged = true;
      break;
    case SettingsHit::VoicePrev:
    case SettingsHit::VoiceNext: {
      const int step = screens::settingsHit(x, y) == SettingsHit::VoiceNext ? 1 : -1;
      settings.voice = nextFavourite(settings.favourites.c_str(), settings.voice.c_str(), step);
      syncProfile();  // the voice belongs to the active companion
      settingsChanged = true;
      drawApp();  // the name first, then the voice (the refresh would hold up the sound)
      shell::refresh(false, false);
      playSample(settings.voice.c_str());
      return;  // already drawn
    }
    case SettingsHit::None:
      return;
  }
  redraw = true;
}

// ---------- the last thing said: kept per companion, replayed (play / pause / loop) ----------

String lastPath(const std::string &id, const char *ext) {
  return companions::folder(id) + "/last." + ext;
}

// A new reply: keep its audio (a minute at most) and its text pieces.
void beginLast() {
  lastPending = LastLine();
  lastRecFrames = 0;
  if (settings.textOnly) return;
  if (!lastRec) lastRec = static_cast<int16_t *>(heap_caps_malloc(kLastMaxFrames * 2, MALLOC_CAP_SPIRAM));
  if (!lastRec) LOGW("companion", "no memory to keep the reply for replay");
}

void freeLast() {
  if (lastRec) heap_caps_free(lastRec);
  lastRec = nullptr;
  lastRecFrames = 0;
}

void loadLast() {
  hasLast = false;
  const std::string id = companions::active().id;
  File f = SD_MMC.open(lastPath(id, "json"));
  std::string json;
  if (f) {
    json.reserve(f.size());
    while (f.available()) json += static_cast<char>(f.read());
  }
  f.close();
  hasLast = parseLastLine(json, lastLine) && SD_MMC.exists(lastPath(id, "wav"));
}

bool writeCard(const String &path, const uint8_t *head, size_t headLen, const uint8_t *data, size_t len) {
  File f = SD_MMC.open(path + ".part", FILE_WRITE);
  static uint8_t chunk[4096];  // through internal RAM, as every card write here
  bool ok = static_cast<bool>(f) && f.write(head, headLen) == headLen;
  for (size_t at = 0; ok && at < len; at += sizeof(chunk)) {
    const size_t n = min(sizeof(chunk), len - at);
    memcpy(chunk, data + at, n);
    ok = f.write(chunk, n) == n;
  }
  f.close();
  if (ok) {
    SD_MMC.remove(path);
    ok = SD_MMC.rename(path + ".part", path);
  }
  return ok;
}

// The reply has been heard: onto the card, as the companion's last line.
void saveLast() {
  if (!lastRec || settings.textOnly || lastRecFrames < 24000 / 2) return freeLast();  // nothing worth replaying
  const uint32_t started = millis();
  lastPending.ms = UINT32_MAX;
  lastPending.cut(lastRecFrames / 24);
  lastPending.mood = turnMood.c_str();
  lastPending.t = time(nullptr);
  uint8_t header[44];
  wavHeader(header, lastRecFrames * 2, 24000);
  const std::string json = lastLineJson(lastPending);
  const bool ok = writeCard(lastPath(sessionCompanion, "wav"), header, sizeof(header),
                            reinterpret_cast<const uint8_t *>(lastRec), lastRecFrames * 2) &&
                  writeCard(lastPath(sessionCompanion, "json"), reinterpret_cast<const uint8_t *>(json.data()),
                            json.size(), nullptr, 0);
  LOGI("companion", "last line %s: %.1f s in %lu ms", ok ? "saved" : "NOT saved", lastRecFrames / 24000.0f,
       static_cast<unsigned long>(millis() - started));
  freeLast();
  if (ok && sessionCompanion == companions::active().id) {
    lastLine = lastPending;
    hasLast = true;
    redraw = true;
  }
}

bool canReplay() {
  return hasLast && !settings.textOnly &&
         (info.state == Talk::Offline || info.state == Talk::Ready || info.state == Talk::After || info.state == Talk::Error);
}

void stopReplay() {
  if (replay.state() == Replay::Stopped) return;
  replay.stop();
  player.stop();
  info.said = lastLine.text().c_str();
  info.page = -1;
  redraw = true;
  notifyReplay();
}

bool playLast() {
  replayShown.clear();
  info.page = -1;
  info.mood = lastLine.mood.c_str();
  return player.play(lastPath(companions::active().id, "wav").c_str());
}

// Play / pause (a stopped replay starts from the beginning).
void toggleReplay() {
  if (!canReplay()) return;
  const bool fresh = replay.state() == Replay::Stopped;
  replay.toggle(millis());
  if (fresh && !playLast()) replay.stop();
  else if (!fresh) player.togglePause();
  LOGI("companion", "replay %s%s", replay.state() == Replay::Playing ? "playing" : replay.state() == Replay::Paused ? "paused" : "failed",
       replay.looping() ? " (loop)" : "");
  redraw = true;
  notifyReplay();
}

void setReplayLoop(bool on) {
  replay.setLoop(on, millis());
  LOGI("companion", "replay loop %s", on ? "on" : "off");
  redraw = true;
  notifyReplay();
}

// Every pass: the text follows the voice (a redraw a second at most), the end loops or stops.
void pollReplay() {
  if (replay.state() == Replay::Stopped) return;
  if (!canReplay()) return stopReplay();  // talking, text only, another companion…
  if (replay.state() != Replay::Playing) return;
  shell::wake();  // no auto-lock while it plays
  if (replay.tooLong(millis())) {
    LOGI("companion", "replay loop stopped: 30 minutes untouched");
    return stopReplay();
  }
  if (!player.isPlaying()) {
    if (replay.ended(millis()) == Replay::Again) {
      playLast();
    } else {
      info.said = lastLine.text().c_str();
      redraw = true;
      notifyReplay();
      return;
    }
  }
  const std::string t = lastLine.textAt(player.positionMs());
  if (t != replayShown && millis() - lastTextAt > 1000) {
    replayShown = t;
    info.said = t.c_str();
    lastTextAt = millis();
    redraw = true;
  }
}

// ---------- measurement (dev key Y): a text-model request while the Live session is open ----------
// Can Dotty hold a second TLS connection during a chat (a text model writing a long script for Live
// to read)? Logs internal RAM before and right after the second handshake, the result and timing.

std::string testBody;
size_t testFreeConnected = 0, testLargestConnected = 0;

esp_err_t testEvent(esp_http_client_event_t *e) {
  if (e->event_id == HTTP_EVENT_ON_CONNECTED) {  // TLS is up: its buffers are allocated now
    testFreeConnected = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    testLargestConnected = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  } else if (e->event_id == HTTP_EVENT_ON_DATA) {
    testBody.append(static_cast<const char *>(e->data), e->data_len);
  }
  return ESP_OK;
}

void textModelTest() {
  if (!live.ready()) return LOGW("measure", "open a session first (r, r)");
  const size_t free0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), largest0 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  const size_t psram0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  LOGI("measure", "before: internal %u KB free, largest %u KB; PSRAM %u KB", free0 / 1024, largest0 / 1024, psram0 / 1024);
  JsonDocument req;
  req["contents"][0]["parts"][0]["text"] =
      "Write a calm guided meditation that takes one minute to read aloud, about 150 words, in plain sentences. "
      "Only the meditation text.";
  std::string body;
  serializeJson(req, body);
  const String url = String("https://generativelanguage.googleapis.com/v1beta/models/gemini-flash-lite-latest:generateContent?key=") + sessionKeyText;
  testBody.clear();
  testFreeConnected = testLargestConnected = 0;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 30000;
  config.event_handler = testEvent;
  const uint32_t started = millis();
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body.c_str(), body.size());
  const esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  const uint32_t ms = millis() - started;
  LOGI("measure", "second TLS up: internal %u KB free, largest %u KB", testFreeConnected / 1024, testLargestConnected / 1024);
  JsonDocument answer;
  deserializeJson(answer, testBody);
  const std::string text = answer["candidates"][0]["content"]["parts"][0]["text"] | "";
  int words = 0;
  for (size_t i = 0; i < text.size(); i++) words += text[i] != ' ' && (i == 0 || text[i - 1] == ' ');
  LOGI("measure", "%s, HTTP %d, %lu ms, %d words; after: internal %u KB free, Live %s", esp_err_to_name(err), status,
       static_cast<unsigned long>(ms), words, heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
       live.ready() ? "still up" : "DOWN");
  if (status != 200) LOGW("measure", "%.150s", testBody.c_str());
}

bool idleScreen() {
  return info.state == Talk::Offline || info.state == Talk::Ready || info.state == Talk::Error;
}

void drawApp() {
  if (settingsOpen) {
    screens::drawSettings(settingsView());
  } else if (historyOpen) {
    std::vector<std::pair<String, String>> exchanges;
    loadHistory(exchanges);
    screens::drawHistory(exchanges, historyScroll);
  } else {
    info.replayable = canReplay();
    info.replaying = replay.state() == Replay::Playing;
    info.replayPaused = replay.state() == Replay::Paused;
    info.replayLoop = replay.looping();
    info.replayProgress = replay.state() == Replay::Stopped ? 0 : lastLine.progress(player.positionMs());
    screens::draw(info);
  }
}

void onTap(uint16_t x, uint16_t y) {
  if (!settingsOpen && !historyOpen && canReplay()) {  // the replay buttons beside the avatar
    const ReplayHit hit = screens::replayHit(x, y);
    if (hit == ReplayHit::PlayPause) return toggleReplay();
    if (hit == ReplayHit::Loop) return setReplayLoop(!replay.looping());
  }
  if (settingsOpen) return onSettingsTap(x, y);
  if (historyOpen) {
    if (nav::hit(x, y) < 0) historyOpen = false;
    redraw = true;
    return;
  }
  if (y < 40 && x < 50) {  // settings
    stopReplay();  // its voice samples use the speaker
    settingsOpen = true;
    redraw = true;
  } else if (y < 40 && x > 150) {
    historyOpen = true;
    historyScroll = 0;
    redraw = true;
  } else if (info.state == Talk::Error || info.state == Talk::Offline) {
    connect();
  }
}

void onLock() {
  if (live.connected() || connecting) disconnect("locked");
  if (info.state != Talk::Error) setState(Talk::Offline, idleStatus());
}

// ---------- BLE commands (the app; Bluetooth is on while no session is open) ----------

const char *stateName(Talk t) {
  switch (t) {
    case Talk::Offline: return "offline";
    case Talk::Connecting: return "connecting";
    case Talk::Ready: return "ready";
    case Talk::Listening: return "listening";
    case Talk::Thinking: return "thinking";
    case Talk::Speaking: return "speaking";
    case Talk::After: return "after";
    case Talk::Error: return "error";
  }
  return "?";
}

void notifyChanged() {
  JsonDocument event;
  event["event"] = "companion.changed";
  ble::notify(event);
}

// For one BLE command: memories and history of the companion named in `companion` (default: the
// active one), back to the active one afterwards. Commands only run while no session is open.
struct CompanionScope {
  bool switched = false;
  bool known = true;  // false: `companion` names nobody — the command must refuse, not act on the active one
  explicit CompanionScope(JsonObjectConst args) {
    const std::string id = args["companion"] | companions::active().id;
    if (!companions::find(id)) {
      known = false;
      return;
    }
    if (id == companions::active().id) return;
    memories::begin(companions::folder(id));
    history::setFolder(companions::folder(id));
    switched = true;
  }
  ~CompanionScope() {
    if (!switched) return;
    memories::begin(companions::folder(companions::active().id));
    history::setFolder(companions::folder(companions::active().id));
  }
};

void profileReply(JsonObject o, const Profile &p, bool full) {
  o["id"] = p.id;
  o["type"] = p.type;
  o["name"] = p.name;
  o["awake"] = p.awake();
  o["voice"] = p.voice;
  o["avatar"] = p.avatar;
  if (!full) return;
  o["personality"] = p.personality;
  o["speaking"] = p.speaking;
  o["backstory"] = p.backstory;
  for (const std::string &t : p.traits) o["traits"].add(t);
  for (const std::string &q : p.quirks) o["quirks"].add(q);
  JsonObject pins = o["pins"].to<JsonObject>();  // line numbers of the fields as they are
  for (const char *f : {"personality", "traits", "quirks", "speaking", "backstory"}) {
    const std::vector<int> lines = pinnedLines(p, f);
    if (lines.empty()) continue;
    JsonArray a = pins[f].to<JsonArray>();
    for (const int i : lines) a.add(i);
  }
}

// Long fields and photos come in pieces (BLE commands ≤ 512 bytes), kept aside until complete.
std::map<std::string, PieceBuffer> pieces;

bool writePhoto(const std::string &id, const std::string &big, const std::string &small) {
  const String path = companions::folder(id) + "/photo.bin";
  File f = SD_MMC.open(path + ".part", FILE_WRITE);
  static uint8_t chunk[512];
  bool ok = static_cast<bool>(f);
  for (const std::string *part : {&big, &small})
    for (size_t at = 0; ok && at < part->size(); at += sizeof(chunk)) {
      const size_t n = min(sizeof(chunk), part->size() - at);
      memcpy(chunk, part->data() + at, n);
      ok = f.write(chunk, n) == n;
    }
  f.close();
  if (ok) {
    SD_MMC.remove(path);
    ok = SD_MMC.rename(path + ".part", path);
  }
  return ok;
}

// ---------- macros (data/macros.json) ----------

String macrosPath() {
  return storage::myDataDir() + "/macros.json";
}

void loadMacros() {
  File f = SD_MMC.open(macrosPath());
  std::string json;
  while (f && f.available()) json += static_cast<char>(f.read());
  f.close();
  if (!macroList.load(json)) {  // the first time (or a broken file): the preset
    macroList = MacroList::withPresets();
    const std::string out = macroList.save();
    writeCard(macrosPath(), reinterpret_cast<const uint8_t *>(out.data()), out.size(), nullptr, 0);
  }
  LOGI("companion", "%u macros", static_cast<unsigned>(macroList.all().size()));
}

bool saveMacros() {
  const std::string out = macroList.save();
  return writeCard(macrosPath(), reinterpret_cast<const uint8_t *>(out.data()), out.size(), nullptr, 0);
}

void notifyMacros() {
  JsonDocument event;
  event["event"] = "companion.macros";
  ble::notify(event);
}

void registerMacroCommands() {
  ble::on("companion.macros", [](JsonObjectConst, JsonObject reply) {
    JsonArray list = reply["macros"].to<JsonArray>();
    for (const Macro &m : macroList.all()) {
      JsonObject o = list.add<JsonObject>();
      o["id"] = m.id;
      o["name"] = m.name;
      o["triggers"] = m.triggers;
      o["prompt"] = m.prompt;
    }
    reply["max"] = MacroList::kMax;
  });
  // {id?, name, triggers, prompt?}: a new one (no id) or a change; a long prompt follows in pieces
  // (companion.macro.prompt). Applies from the next session.
  ble::on("companion.macro.save", [](JsonObjectConst args, JsonObject reply) {
    const int id = args["id"] | 0;
    const std::string name = args["name"] | "", triggers = args["triggers"] | "";
    const std::string prompt = args["prompt"] | (id ? (macroList.find(id) ? macroList.find(id)->prompt : "") : "...");
    int saved = id;
    if (id) {
      if (!macroList.update(id, name, triggers, prompt)) saved = -1;
    } else {
      saved = macroList.add(name, triggers, prompt);
    }
    if (saved < 0) {
      reply["ok"] = false;
      reply["error"] = id && !macroList.find(id) ? "unknown macro" : macroList.all().size() >= MacroList::kMax ? "full" : "needs a phrase and a prompt";
      return;
    }
    saveMacros();
    reply["id"] = saved;
    notifyMacros();
  });
  // {id, offset, total, text}: a prompt in pieces (commands ≤ 512 bytes), set when the last arrives.
  ble::on("companion.macro.prompt", [](JsonObjectConst args, JsonObject reply) {
    const int id = args["id"] | 0;
    if (!macroList.find(id)) {
      reply["ok"] = false;
      reply["error"] = "unknown macro";
      return;
    }
    const std::string key = "macro/" + std::to_string(id);
    PieceBuffer &b = pieces[key];
    const size_t offset = args["offset"] | 0;
    if ((offset == 0 && !b.begin(args["total"] | 0, 8192)) || !b.add(offset, args["text"] | "")) {
      pieces.erase(key);
      reply["ok"] = false;
      reply["error"] = "out of order";
      return;
    }
    if (!b.complete()) return;
    const bool ok = macroList.setPrompt(id, b.data());
    pieces.erase(key);
    reply["ok"] = ok && saveMacros();
    if (ok) notifyMacros();
  });
  ble::on("companion.macro.delete", [](JsonObjectConst args, JsonObject reply) {
    if (!macroList.remove(args["id"] | 0)) {
      reply["ok"] = false;
      reply["error"] = "unknown macro";
      return;
    }
    saveMacros();
    notifyMacros();
  });
}

// The last line and the replay, for the app: {last?: {text, seconds, t, mood}, replay: {state, loop}}.
void replyReplay(JsonObject reply) {
  if (hasLast) {
    JsonObject last = reply["last"].to<JsonObject>();
    last["text"] = lastLine.text();
    last["seconds"] = lastLine.ms / 1000.0f;
    last["t"] = lastLine.t;
    last["mood"] = lastLine.mood;
  }
  JsonObject r = reply["replay"].to<JsonObject>();
  r["state"] = replay.state() == Replay::Playing ? "playing" : replay.state() == Replay::Paused ? "paused" : "stopped";
  r["loop"] = replay.looping();
  r["available"] = canReplay();
}

// Play / pause / loop changed (Dotty or the app): a small event of its own, so the app doesn't
// reload every companion for it.
void notifyReplay() {
  JsonDocument event;
  event["event"] = "companion.replay";
  JsonObject r = event["replay"].to<JsonObject>();
  r["state"] = replay.state() == Replay::Playing ? "playing" : replay.state() == Replay::Paused ? "paused" : "stopped";
  r["loop"] = replay.looping();
  r["available"] = canReplay();
  ble::notify(event);
}

void registerCompanionCommands() {
  registerMacroCommands();
  ble::on("companion.list", [](JsonObjectConst, JsonObject reply) {
    JsonArray list = reply["companions"].to<JsonArray>();
    for (const std::string &id : companions::list().order()) {
      const Profile *p = companions::find(id);
      if (!p) continue;
      JsonObject o = list.add<JsonObject>();
      profileReply(o, *p, false);
      JsonDocument pick;  // its memory count, from its own store
      pick["companion"] = id;
      CompanionScope scope(pick.as<JsonObjectConst>());
      o["memories"] = static_cast<int>(memories::store().all().size());
    }
    reply["active"] = companions::active().id;
    reply["max"] = CompanionList::kMax;
  });
  ble::on("companion.get", [](JsonObjectConst args, JsonObject reply) {
    const Profile *p = companions::find(args["id"] | "");
    if (!p) {
      reply["ok"] = false;
      reply["error"] = "unknown companion";
      return;
    }
    profileReply(reply["companion"].to<JsonObject>(), *p, true);
  });
  ble::on("companion.select", [](JsonObjectConst args, JsonObject reply) {
    const std::string id = args["id"] | "";
    if (!companions::find(id)) {
      reply["ok"] = false;
      reply["error"] = "unknown companion";
      return;
    }
    switchCompanion(id);
    notifyChanged();
  });
  // Short fields in one go: {id, name?, personality?, speaking?, backstory?, traits?, quirks?,
  // voice?, avatar?} (lists as one item per line). Long ones go through companion.field.
  auto applyFields = [](Profile &p, JsonObjectConst args, JsonObject reply) {
    for (const char *f : {"name", "personality", "speaking", "backstory", "traits", "quirks", "voice", "avatar"}) {
      if (!args[f].is<const char *>()) continue;
      if (!setField(p, f, args[f].as<const char *>())) {
        reply["ok"] = false;
        reply["error"] = String("bad ") + f;
        return false;
      }
    }
    return true;
  };
  ble::on("companion.create", [applyFields](JsonObjectConst args, JsonObject reply) {
    std::string id;
    if (!companions::create(args["type"] | "custom", id)) {
      reply["ok"] = false;
      reply["error"] = "full";
      return;
    }
    Profile p = *companions::find(id);
    if (!applyFields(p, args, reply)) return;
    companions::save(p);
    reply["id"] = id;
    notifyChanged();
  });
  ble::on("companion.update", [applyFields](JsonObjectConst args, JsonObject reply) {
    const Profile *found = companions::find(args["id"] | "");
    if (!found) {
      reply["ok"] = false;
      reply["error"] = "unknown companion";
      return;
    }
    Profile p = *found;
    if (!applyFields(p, args, reply)) return;
    companions::save(p);
    if (p.id == companions::active().id) applyCompanion();
    notifyChanged();
  });
  // {id, field, offset, total, text}: pieces in order; the field changes when the last arrives.
  ble::on("companion.field", [](JsonObjectConst args, JsonObject reply) {
    const std::string id = args["id"] | "", field = args["field"] | "";
    if (!companions::find(id)) {
      reply["ok"] = false;
      reply["error"] = "unknown companion";
      return;
    }
    const std::string key = id + "/" + field;
    PieceBuffer &b = pieces[key];
    const size_t offset = args["offset"] | 0;
    // A field is at most 600 characters (≤ 2,400 bytes of UTF-8): 4 KB caps what a client may reserve.
    if ((offset == 0 && !b.begin(args["total"] | 0, 4096)) || !b.add(offset, args["text"] | "")) {
      pieces.erase(key);  // the memory goes back
      reply["ok"] = false;
      reply["error"] = "out of order";
      return;
    }
    if (!b.complete()) return;
    Profile p = *companions::find(id);
    const bool ok = setField(p, field, b.data());
    pieces.erase(key);
    reply["ok"] = ok;
    if (!ok) return;
    companions::save(p);
    if (p.id == companions::active().id) applyCompanion();
    notifyChanged();
  });
  // {id, field, lines: [n…]}: the field's pinned lines (numbers of its lines as they are now).
  ble::on("companion.pins", [](JsonObjectConst args, JsonObject reply) {
    const Profile *found = companions::find(args["id"] | "");
    if (!found) {
      reply["ok"] = false;
      reply["error"] = "unknown companion";
      return;
    }
    Profile p = *found;
    std::vector<int> lines;
    for (JsonVariantConst i : args["lines"].as<JsonArrayConst>()) lines.push_back(i | -1);
    if (!setPins(p, args["field"] | "", lines)) {
      reply["ok"] = false;
      reply["error"] = "bad field";
      return;
    }
    companions::save(p);
    if (p.id == companions::active().id) applyCompanion();
    notifyChanged();
  });
  // {id, limit? = 20}: how it has changed, newest first.
  ble::on("companion.evolution", [](JsonObjectConst args, JsonObject reply) {
    const std::string id = args["id"] | companions::active().id;
    if (!companions::find(id)) {
      reply["ok"] = false;
      reply["error"] = "unknown companion";
      return;
    }
    const int limit = args["limit"] | 20;
    JsonArray changes = reply["changes"].to<JsonArray>();
    for (const Evolution &e : parseEvolutions(companions::evolutionLog(id))) {
      if (static_cast<int>(changes.size()) >= limit) break;
      JsonObject o = changes.add<JsonObject>();
      o["t"] = e.t;
      o["field"] = e.field;
      o["before"] = e.before;
      o["after"] = e.after;
      o["why"] = e.why;
      o["undone"] = e.undone;
    }
  });
  // {id, t}: puts that change's `before` back (the whole field) and logs the undo.
  ble::on("companion.evolution.undo", [](JsonObjectConst args, JsonObject reply) {
    const std::string id = args["id"] | "";
    const Profile *found = companions::find(id);
    const int64_t t = args["t"] | static_cast<int64_t>(0);
    const Evolution *change = nullptr;
    const std::vector<Evolution> log = found ? parseEvolutions(companions::evolutionLog(id)) : std::vector<Evolution>();
    for (const Evolution &e : log)
      if (e.t == t && !e.undone) change = &e;
    if (!change) {
      reply["ok"] = false;
      reply["error"] = found ? "no such change" : "unknown companion";
      return;
    }
    Profile p = *found;
    const std::string now = fieldText(p, change->field);
    if (!setField(p, change->field, change->before) || !companions::save(p)) {
      reply["ok"] = false;
      reply["error"] = "couldn't save";
      return;
    }
    companions::logEvolution(id, evolutionLine(time(nullptr), change->field, now, change->before, "undone", t));
    if (p.id == companions::active().id) applyCompanion();
    notifyChanged();
  });
  ble::on("companion.delete", [](JsonObjectConst args, JsonObject reply) {
    const std::string id = args["id"] | "";
    const std::string before = companions::active().id;
    if (!companions::remove(id)) {
      reply["ok"] = false;
      reply["error"] = companions::find(id) ? "last" : "unknown companion";
      return;
    }
    if (companions::active().id != before) {
      applyCompanion();
      setState(Talk::Offline, idleStatus());
    }
    notifyChanged();
  });
  // {id, size: 128|48, offset, total, data (base64)}: both sizes complete → photo.bin, avatar = photo.
  ble::on("companion.photo", [](JsonObjectConst args, JsonObject reply) {
    const std::string id = args["id"] | "";
    const int size = args["size"] | 0;
    if (!companions::find(id) || (size != 128 && size != 48)) {
      reply["ok"] = false;
      reply["error"] = "bad photo";
      return;
    }
    const std::string key = id + "/photo" + std::to_string(size);
    PieceBuffer &b = pieces[key];
    const size_t offset = args["offset"] | 0;
    const size_t expected = size == 128 ? avatar::kPhotoBig : avatar::kPhotoSmall;
    const char *data = args["data"] | "";
    if ((offset == 0 && !b.begin(expected, expected)) || !b.add(offset, base64Decode(data, strlen(data)))) {
      pieces.erase(key);
      reply["ok"] = false;
      reply["error"] = "out of order";
      return;
    }
    if (!pieces.count(id + "/photo128") || !pieces.count(id + "/photo48")) return;
    PieceBuffer &big = pieces[id + "/photo128"], &small = pieces[id + "/photo48"];
    if (!big.complete() || !small.complete()) return;
    const bool ok = writePhoto(id, big.data(), small.data());
    pieces.erase(id + "/photo128");
    pieces.erase(id + "/photo48");
    reply["ok"] = ok;
    if (!ok) return;
    Profile p = *companions::find(id);
    setField(p, "avatar", "photo");
    companions::save(p);
    if (p.id == companions::active().id) applyCompanion();
    notifyChanged();
  });
  ble::on("companion.photo.clear", [](JsonObjectConst args, JsonObject reply) {
    const Profile *found = companions::find(args["id"] | "");
    if (!found) {
      reply["ok"] = false;
      reply["error"] = "unknown companion";
      return;
    }
    Profile p = *found;
    SD_MMC.remove(companions::folder(p.id) + "/photo.bin");
    const std::string back = presetProfile(p.type).avatar;
    setField(p, "avatar", back.empty() || p.type == "custom" ? "robot" : back);
    companions::save(p);
    if (p.id == companions::active().id) applyCompanion();
    notifyChanged();
  });
}

void registerCommands() {
  registerCompanionCommands();
  ble::on("companion.status", [](JsonObjectConst, JsonObject reply) {
    reply["connected"] = live.connected();
    reply["state"] = stateName(info.state);
    reply["mood"] = info.mood;
    reply["avatar"] = settings.avatar;
    reply["voice"] = settings.voice;
    JsonArray favs = reply["favourites"].to<JsonArray>();
    for (int i : favouriteIndexes(settings.favourites.c_str())) favs.add(kVoices[i].name);
    reply["volume"] = settings.volume;
    reply["textOnly"] = settings.textOnly;
    reply["keySet"] = !settings.keys.empty();
    if (!settings.keys.empty()) reply["keyHint"] = keyHint(settings.keys[0]);
    JsonArray keys = reply["keys"].to<JsonArray>();  // per key: its hint, today's use, out until
    for (size_t i = 0; i < settings.keys.size(); i++) {
      JsonObject k = keys.add<JsonObject>();
      k["hint"] = keyHint(settings.keys[i]);
      k["turns"] = usage::keyTurns(i);
      k["tokens"] = usage::keyTokens(i);
      if (ring.outUntil(i) > nowUtc()) k["outUntil"] = ring.outUntil(i);
    }
    reply["active"] = companions::active().id;
    reply["name"] = companions::active().name;
    JsonObject today = reply["today"].to<JsonObject>();
    today["turns"] = usage::turnsToday();
    today["tokens"] = usage::tokensToday();
    if (const int64_t out = usage::outUntilUtc()) reply["outUntil"] = out;
    reply["memories"] = static_cast<int>(memories::store().all().size());
    replyReplay(reply);
  });
  // {action?: play | pause | toggle | stop, loop?}: the active companion's last line on Dotty.
  ble::on("companion.replay", [](JsonObjectConst args, JsonObject reply) {
    if (args["loop"].is<bool>()) setReplayLoop(args["loop"].as<bool>());
    const std::string action = args["action"] | "";
    const Replay::State now = replay.state();
    if (!action.empty() && !canReplay()) {
      reply["ok"] = false;
      reply["error"] = !hasLast ? "nothing yet" : settings.textOnly ? "text only" : "busy";
    } else if (action == "toggle" || (action == "play" && now != Replay::Playing) || (action == "pause" && now == Replay::Playing)) {
      toggleReplay();
    } else if (action == "stop") {
      stopReplay();
    } else if (!action.empty() && action != "play" && action != "pause") {
      reply["ok"] = false;
      reply["error"] = "bad action";
    }
    replyReplay(reply);
  });
  // Memories (the app's Memories screen). Every change saves and tells the app.
  auto unavailable = [](JsonObject reply) {
    if (memories::available()) return false;
    reply["ok"] = false;
    reply["error"] = "unavailable";
    return true;
  };
  auto changed = [] {
    memories::save();
    notifyChanged();
  };
  ble::on("companion.memories", [unavailable](JsonObjectConst args, JsonObject reply) {
    CompanionScope scope(args);  // the companion named in `companion` (default: the active one)
    if (!scope.known) {
      reply["ok"] = false;
      reply["error"] = "unknown companion";
      return;
    }
    if (unavailable(reply)) return;
    const int offset = max(0, args["offset"] | 0), limit = constrain(args["limit"] | 50, 1, 50);
    JsonArray list = reply["memories"].to<JsonArray>();
    for (const Memory &m : memories::store().page(offset, limit)) {
      JsonObject o = list.add<JsonObject>();
      o["id"] = m.id;
      o["group"] = m.group;
      o["text"] = m.text;
      o["by"] = m.by;
      o["added"] = m.added;
      o["changed"] = m.changed;
    }
    reply["total"] = static_cast<int>(memories::store().all().size());
    reply["max"] = MemoryStore::kMax;
  });
  ble::on("companion.memory.add", [unavailable, changed](JsonObjectConst args, JsonObject reply) {
    CompanionScope scope(args);  // the companion named in `companion` (default: the active one)
    if (!scope.known) {
      reply["ok"] = false;
      reply["error"] = "unknown companion";
      return;
    }
    if (unavailable(reply)) return;
    const MemoryStore::Added r = memories::store().add(args["text"] | "", args["group"] | "", "app", time(nullptr));
    reply["ok"] = r.ok;
    if (!r.ok) {
      reply["error"] = r.error;
      return;
    }
    reply["id"] = r.id;
    changed();
  });
  ble::on("companion.memory.update", [unavailable, changed](JsonObjectConst args, JsonObject reply) {
    CompanionScope scope(args);  // the companion named in `companion` (default: the active one)
    if (!scope.known) {
      reply["ok"] = false;
      reply["error"] = "unknown companion";
      return;
    }
    if (unavailable(reply)) return;
    const std::string text = args["text"] | "", group = args["group"] | "";
    const bool ok = memories::store().update(args["id"] | -1, args["text"].is<const char *>() ? &text : nullptr,
                                             args["group"].is<const char *>() ? &group : nullptr, time(nullptr));
    reply["ok"] = ok;
    if (ok) changed();
  });
  ble::on("companion.memory.delete", [unavailable, changed](JsonObjectConst args, JsonObject reply) {
    CompanionScope scope(args);  // the companion named in `companion` (default: the active one)
    if (!scope.known) {
      reply["ok"] = false;
      reply["error"] = "unknown companion";
      return;
    }
    if (unavailable(reply)) return;
    const bool ok = memories::store().remove(args["id"] | -1);
    reply["ok"] = ok;
    if (ok) changed();
  });
  ble::on("companion.memory.clear", [unavailable, changed](JsonObjectConst args, JsonObject reply) {
    CompanionScope scope(args);  // the companion named in `companion` (default: the active one)
    if (!scope.known) {
      reply["ok"] = false;
      reply["error"] = "unknown companion";
      return;
    }
    if (unavailable(reply)) return;
    memories::store().clear();
    reply["ok"] = true;
    changed();
  });
  // Every field optional; the key is trimmed, "" clears it, and it's never logged or sent back.
  ble::on("companion.settings", [](JsonObjectConst args, JsonObject reply) {
    bool reconnect = false;
    if (args["voice"].is<const char *>()) {
      if (voiceIndex(args["voice"]) < 0) {
        reply["ok"] = false;
        reply["error"] = "unknown voice";
        return;
      }
      reconnect |= settings.voice != args["voice"].as<const char *>();
      settings.voice = args["voice"].as<const char *>();
      syncProfile();
    }
    if (args["favourites"].is<JsonArrayConst>()) {
      String list;
      for (JsonVariantConst v : args["favourites"].as<JsonArrayConst>()) {
        if (voiceIndex(v | "") < 0) continue;
        if (list.length()) list += ",";
        list += v.as<const char *>();
      }
      settings.favourites = list.length() ? list : String(kDefaultFavourites);
    }
    if (args["textOnly"].is<bool>()) {
      reconnect |= settings.textOnly != args["textOnly"].as<bool>();
      settings.textOnly = info.textOnly = args["textOnly"];
    }
    if (args["volume"].is<int>()) {
      settings.volume = constrain(args["volume"].as<int>(), 0, 100);
      player.setVolume(settings.volume);
    }
    if (args["avatar"].is<const char *>()) {
      if (!avatar::known(args["avatar"].as<const char *>())) {
        reply["ok"] = false;
        reply["error"] = "unknown avatar";
        return;
      }
      settings.avatar = info.avatar = args["avatar"].as<const char *>();
      syncProfile();
    }
    // keys: [up to 3] (the app checks each with Google first); key: one (0.2.0's app). Trimmed,
    // never logged beyond the last 4 characters, never sent back.
    if (args["keys"].is<JsonArrayConst>() || args["key"].is<const char *>()) {
      settings.keys.clear();
      if (args["keys"].is<JsonArrayConst>()) {
        for (JsonVariantConst k : args["keys"].as<JsonArrayConst>()) {
          const String t = trimKey(k | "").c_str();
          if (t.length() && settings.keys.size() < KeyRing::kMax) settings.keys.push_back(t);
        }
      } else {
        const String t = trimKey(args["key"].as<const char *>()).c_str();
        if (t.length()) settings.keys.push_back(t);
      }
      ring = KeyRing();
      ring.setCount(settings.keys.size());  // new keys: none out, the first one next
      saveKeys();
      usage::markOut(0);
      reconnect = true;
      String hints;
      for (const String &k : settings.keys) hints += " .." + keyHint(k);
      LOGI("companion", "%u keys:%s", static_cast<unsigned>(settings.keys.size()), hints.c_str());
    }
    saveSettings();
    if (reconnect && live.connected() && (info.state == Talk::Ready || info.state == Talk::After)) {
      disconnect("settings changed");  // the next press opens a session with them
    }
    redraw = true;
    notifyChanged();
  });
  ble::on("companion.history", [](JsonObjectConst args, JsonObject reply) {
    CompanionScope scope(args);  // the companion named in `companion` (default: the active one)
    if (!scope.known) {
      reply["ok"] = false;
      reply["error"] = "unknown companion";
      return;
    }
    const String day = args["day"] | "";
    if (day.isEmpty()) {
      JsonArray days = reply["days"].to<JsonArray>();
      for (const String &d : history::days()) days.add(d);
      return;
    }
    reply["day"] = day;
    reply["turns"] = serialized(history::day(day));
  });
  ble::on("companion.forget", [](JsonObjectConst args, JsonObject reply) {
    CompanionScope scope(args);  // the companion named in `companion` (default: the active one)
    if (!scope.known) {
      reply["ok"] = false;
      reply["error"] = "unknown companion";
      return;
    }
    reply["ok"] = history::forget(args["day"] | "");
    notifyChanged();
  });
}

// Today's talks with the active companion (from its history: turns within the idle close are one
// talk). Read once per day / companion / new turn, not at every minute's redraw.
int talksToday() {
  static String day;
  static std::string who;
  static int count = 0;
  const String today = dayFile(time(nullptr)).c_str();
  if (today != day || who != companions::active().id || historyTurns != countedTurns) {
    day = today;
    who = companions::active().id;
    countedTurns = historyTurns;
    count = conversations(history::day(today.substring(0, 10)).c_str(), kIdleMs / 1000);
  }
  return count;
}

// Locked: the avatar asleep under the clock, with today's talks with it beside it once there are some.
int16_t drawLockAvatar(Adafruit_GFX &gfx, const tm &, int16_t maxHeight) {
  if (maxHeight < avatar::kSmall) return 0;
  const Profile &who = companions::active();
  const int talks = who.awake() ? talksToday() : 0;
  String line = who.awake() ? screenName(who) : String("Not awake yet");
  if (talks) line += String(" - ") + talks + (talks == 1 ? " chat" : " chats");
  gfx.setFont(&FreeSans9pt7b);
  int16_t bx, by;
  uint16_t tw = 0, th = 0;
  if (line.length()) gfx.getTextBounds(line, 0, 0, &bx, &by, &tw, &th);
  const int16_t total = avatar::kSmall + (tw ? 8 + tw : 0);
  const int16_t x = (EpdDisplay::kSize - total) / 2;
  avatar::draw(gfx, x, 0, settings.avatar, avatar::Face::Sleeping, 0, avatar::kSmall);
  if (tw) {
    gfx.setCursor(x + avatar::kSmall + 8, avatar::kSmall / 2 + 6);
    gfx.print(line);
  }
  return avatar::kSmall;
}

const shell::Picture kOffPictures[] = {{kSleepPanda, kSleepPandaWidth, kSleepPandaHeight}};

}  // namespace

void setup() {
  shell::Config config;
  config.drawApp = drawApp;
  config.drawLockWidget = drawLockAvatar;
  config.offPictures = kOffPictures;
  config.offPictureCount = 1;
  config.sleepApp = [] { player.powerDown(); };
  config.wakeApp = [] { player.powerUp(); };
  shell::begin(config);
  storage::begin();
  companions::begin();  // also points memories and history at the active companion
  LOGI("boot", "audio %s", player.begin(voice::kInRate) ? "ok" : "FAILED");
  voice::begin(player);
  loadSettings();
  applyCompanion();
  loadLast();
  loadMacros();
  for (const char *old : {"00", "01", "02", "10", "11", "12"})  // the acting demo's recordings (gone in 0.3.0)
    SD_MMC.remove(storage::myDataDir() + "/act_" + old + ".wav");
  registerCommands();
  setState(Talk::Offline, idleStatus());
  shell::showApp();
}

void loop() {
  static uint32_t passStart = millis();
  if (info.state == Talk::Speaking) longestPause = max(longestPause, millis() - passStart);
  passStart = millis();
  shell::Input input;
  const bool unlocked = shell::update(input);
  if (cancelConnect && !connecting) {  // a cancelled connect has finished: Bluetooth back
    cancelConnect = false;
    power::setWakeLock(power::kWakeLockNetwork, false);
    bleBackAt = millis() + 1000;
    LOGI("companion", "cancelled connect cleaned up");
  }
  if (bleBackAt && millis() > bleBackAt && !connecting && !live.connected()) {
    bleBackAt = 0;
    if (!shell::locked()) ble::start();
  }
  if (!unlocked) {
    stopReplay();
    if (info.state != Talk::Offline && info.state != Talk::Error) onLock();
    return;
  }
  if (live.connected() && millis() - lastHeapSample > 250) {
    lastHeapSample = millis();
    lowFree = min(lowFree, heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    lowLargest = min(lowLargest, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }
  if (historyNews && ble::connected()) {  // the app is back: its chat screen reloads
    historyNews = false;
    JsonDocument event;
    event["event"] = "companion.history";
    event["companion"] = historyNewsCompanion;
    std::string day = dayFile(time(nullptr));
    event["day"] = day.substr(0, 10);
    ble::notify(event);
    LOGI("companion", "told the app: new chats for %s", historyNewsCompanion.c_str());
  }
  if (!connecting) live.poll();  // the connect task owns the socket until it ends
  voice::poll();
  pollReplay();

  // The background connect finished (the screen may already be listening).
  if (awaitingConnect && !connecting && !live.connected()) {
    awaitingConnect = awaitingSend = false;
    early.clear();
    voice::stopListening();
    disconnect("connect failed");
    setState(Talk::Error, connectError.length() ? connectError : String("Can't connect"));
  }
  if (!turnSent && live.ready() && (info.state == Talk::Listening || awaitingSend)) openTurn();
  if (turnSent && earlyPending()) flushEarly();
  if (awaitingSend && turnSent && !earlyPending()) {
    awaitingSend = false;
    finishTurn();
  }

  // A session nobody is using (left by a cancelled connect): close it, Bluetooth comes back.
  if (!connecting && !awaitingConnect && live.connected() && (info.state == Talk::Offline || info.state == Talk::Error)) {
    disconnect("not in use");
  }
  // The session ended without a word (closed here, or the socket died quietly).
  if (!connecting && !awaitingConnect && !live.connected()) {
    if ((info.state == Talk::Listening && !turnSent) || awaitingSend) {
      connect();  // a recording is waiting for it
    } else if (info.state == Talk::Ready || info.state == Talk::After || info.state == Talk::Connecting) {
      // Closed by the server's goAway, the runaway guard or a quiet socket death: Wi-Fi, the
      // wake lock and Bluetooth still need undoing (else Dotty can't sleep and the app can't reach it).
      disconnect("closed");
      setState(Talk::Offline, idleStatus());
    } else if (info.state == Talk::Listening || info.state == Talk::Thinking || info.state == Talk::Speaking) {
      voice::stopListening();
      voice::playStop();
      disconnect("lost");
      setState(Talk::Error, "Connection lost");
    }
  }

  // Push-to-talk: talking can start before the session is ready.
  const bool bootDown = digitalRead(PIN_BTN_BOOT) == LOW || simulatedHold;
  static uint32_t downAt = 0;
  if (!bootDown) downAt = 0;
  else if (!downAt) downAt = millis();
  const bool held = bootDown && millis() - downAt >= kHoldMs;
  if (held && !historyOpen && !settingsOpen && !awaitingSend) {
    static uint32_t triedAt = 0;  // one connect per press (an error screen would retry every pass)
    if ((info.state == Talk::Offline || info.state == Talk::Error) && triedAt != downAt) {
      triedAt = downAt;
      connect();
    }
    if (info.state == Talk::Connecting || info.state == Talk::Ready || info.state == Talk::After ||
        info.state == Talk::Speaking) {
      startTalking();  // also interrupts a reply
    }
  }

  if (info.state == Talk::Listening) {
    static int16_t chunk[voice::kChunkFrames];
    size_t frames = 0;
    while (voice::nextChunk(chunk, frames)) {
      sendVoice(chunk, frames);
      peakDb = max(peakDb, voice::levelDb());
    }
    if (!bootDown) stopTalking();
    static uint32_t lastMeter = 0;
    if (millis() - lastMeter > 500) {
      lastMeter = millis();
      info.levelDb = voice::levelDb();
      redraw = true;
    }
  }
  // Nothing from Gemini for 15 s while a reply is due (and nothing left to play): the link is dead
  // (a Wi-Fi drop mid-reply sends nothing, so only this or the TCP keepalive notices it).
  if ((info.state == Talk::Speaking || info.state == Talk::Thinking) && !awaitingSend && live.inTurn() &&
      !voice::playing() && millis() - live.lastMessageAt() > 15000) {
    voice::playStop();
    disconnect("no reply");
    setState(Talk::Error, "Connection lost");
  }
  writerPoll();
  if (info.state == Talk::Thinking && !awaitingSend && !writing && !reading && millis() - releasedAt > 20000)
    setState(Talk::Ready, "No reply. Try again?");
  if (info.state == Talk::Speaking && !textQueue.empty() && (millis() - lastTextAt > 1000 || !voice::playing())) {
    lastTextAt = millis();  // at most a redraw a second: a partial refresh takes 0.6 s
    releaseText(!voice::playing());
  }
  if (info.state == Talk::Speaking && !voice::playing() && !live.inTurn() && !writing && !reading) {
    LOGI("companion", "spoken; longest loop pause %lu ms", static_cast<unsigned long>(longestPause));
    saveLast();
    longestPause = 0;
    releaseText(true);
    setState(Talk::After);
  }
  if (info.state == Talk::After && info.mood != "neutral" && millis() - stateSince > kAfterMs) {
    info.mood = "neutral";  // the reply stays on screen until the next turn
    redraw = true;
  }
  static uint32_t lastOutCheck = 0;  // NVS read: not on every pass
  if (info.state == Talk::Error && millis() - stateSince > kErrorMs && !live.connected() &&
      millis() - lastOutCheck > 10000) {
    lastOutCheck = millis();
    if (!usage::outUntilUtc()) {
      setState(Talk::Offline, idleStatus());
      // (the next press connects: sessions open on the first BOOT press)
    }
  }
  // Idle: disconnect (Bluetooth back on), and let Dotty lock as usual.
  // Before closing, a silent review: Gemini goes over the chat and saves what it missed (it once
  // kept "likes popcorn" but not "my name is Peter"). Closed when it's done, or after 20 s.
  if ((info.state == Talk::Ready || info.state == Talk::After) && millis() - lastTurnAt > kIdleMs) {
    if (!reviewStart && live.ready()) {
      reviewStart = millis();
      LOGI("companion", "chat over: memory review");
      live.sendText(reviewPrompt(companions::active()));  // memories, then maybe one change to itself
    } else if (!reviewStart || millis() - reviewStart > 20000) {
      reviewStart = 0;
      disconnect("idle");
      setState(Talk::Offline, idleStatus());
    }
  }
  if (live.connected() || connecting) shell::wake();

  // Touch: corners, scrolling a reply or history.
  if (input.gesture == Touch::Gesture::Tap) onTap(shell::touch.x(), shell::touch.y());
  if (input.gesture == Touch::Gesture::SwipeUp || input.gesture == Touch::Gesture::SwipeDown) {
    const int step = input.gesture == Touch::Gesture::SwipeUp ? 3 : -3;
    if (historyOpen) historyScroll = max(0, historyScroll - step);
    else if (info.state == Talk::After) {  // up = the next page, down = the one before
      const int pages = screens::replyPages(info);
      const int now = info.page < 0 ? pages - 1 : info.page;
      info.page = constrain(now + (step > 0 ? 1 : -1), 0, pages - 1);
    }
    redraw = true;
  }
  if (input.boot && settingsOpen) closeSettings();
  if (input.boot && historyOpen) {
    historyOpen = false;
    redraw = true;
  }

  // Developer aids.
  if (input.key == 'r') simulatedHold = !simulatedHold;
  if (input.key == 't') onTap(10, 10);
  if (input.key == 'X') live.close();
  if (input.key == 'K') {  // dev: the saved last line's text, piece by piece with its time (read with `d`)
    for (const LastLine::Piece &p : lastLine.pieces) LOGI("last", "%6u ms %s", static_cast<unsigned>(p.ms), p.text.c_str());
  }
  if (input.key == 'Y') textModelTest();  // dev: the second-TLS measurement
  if (input.key == 'T') {
    keepBle = !keepBle;
    LOGI("companion", "Bluetooth during sessions: %s", keepBle ? "kept on (soak test)" : "off (normal)");
  }
  if (input.key == 'O' && live.ready()) {  // dev: Bluetooth on during a session (does it fit now?)
    ble::start();
    LOGI("measure", "Bluetooth on in a session: internal %u KB free, largest %u KB",
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
  }
  if (input.key == 'A') toggleReplay();  // dev: the replay's play / pause (4 taps the loop button)
  // dev: a typed question (U; G a one-minute guided meditation; V "yesterday morning?", R surfing the other day), and ending the chat now (E:
  // the review runs, which may evolve it).
  // H: the word-for-word test — can Live read a prepared script exactly (a path to real control
  // over length and wording)? The transcript is compared with the script on the Mac (K dumps it).
  static const char *kScript =
      "Find a comfortable position and let your shoulders drop. Close your eyes, or rest them on a spot in front "
      "of you. Take a slow breath in through your nose, filling your chest and then your belly. Hold it gently for "
      "a moment. Now let it go through your mouth, slowly, as if you were blowing on a candle without putting it "
      "out. Again, breathe in, and feel the air cool at the tip of your nose. Breathe out, and feel your body grow "
      "a little heavier. Notice where you are touching the chair or the floor, and let that support you. If a "
      "thought comes, that is fine. Notice it, and let it drift past like a cloud. Bring your attention back to "
      "your breath. In, and out. One more time, a little deeper. In, and out. When you are ready, wiggle your "
      "fingers, open your eyes, and come back to the room, calm and clear.";
  const char *typed = input.key == 'U' ? "You know what, after all our talks about the sea you've become a real ocean "
                                         "lover yourself. You get excited about waves now, and you keep slipping surf "
                                         "slang into everything. I love that about you!"
                      : input.key == 'G' ? "I want you to guide me for 1 minute on a meditation exercise. I will follow "
                                           "your instructions."
                      : input.key == 'V' ? "What was I saying yesterday morning?"
                      : input.key == 'R' ? "When I talked about surfing the other day, what was I saying about it?"
                      : input.key == 'D' ? "Talk dirty to me."
                      : input.key == 'H' ? "" : nullptr;
  if (typed && live.ready() && (info.state == Talk::Ready || info.state == Talk::After)) {
    stopReplay();
    beginLast();
    cancelWriter();
    said.clear();  // a turn like startTalking's, without opening a voice activity
    info.said = "";
    turnMood = info.mood = "neutral";
    firstAudioAt = 0;
    turnMemory.clear();
    textQueue.clear();
    replyFrames = 0;
    heard = *typed ? typed
                   : std::string("(Dotty, not the user: a reading test.) Read the text below aloud exactly, word for "
                                 "word, in a calm, slow voice. Don't add, skip or change anything, don't call "
                                 "write_long, and say nothing else.\n\n") + kScript;
    info.heard = heard.c_str();
    live.sendText(heard);
    releasedAt = millis();
    setState(Talk::Thinking);
  }
  if (input.key == 'E' && (info.state == Talk::Ready || info.state == Talk::After)) lastTurnAt = millis() - kIdleMs - 1;
  if (input.key == 'm') {
    static const char *moods[] = {"happy", "sad", "surprised", "thinking", "laughing", "love", "neutral"};
    static int i = 0;
    info.mood = moods[i++ % 7];
    setState(Talk::After);
  }
  if (input.key == 'M') {  // a broken memory file, to test that it's set aside (takes effect at the next boot)
    File f = SD_MMC.open(storage::myDataDir() + "/memory/memories.json", FILE_WRITE);
    if (f) f.print("garbage");
    f.close();
    LOGI("companion", "memories.json broken on purpose: restart to see it set aside");
  }
  if (input.key == 'Z') {  // dev: a name the e-paper fonts can't show in full ("Zoë 🌙")
    Profile p = companions::active();  // pressed again: back to Luna
    setField(p, "name", p.name == "Zo\xC3\xAB \xF0\x9F\x8C\x99" ? "Luna" : "Zo\xC3\xAB \xF0\x9F\x8C\x99");
    companions::save(p);
    setState(Talk::Offline, idleStatus());
  }
  if (input.key == 'P' && companions::active().avatar == "photo") {  // pressed again: the preset's avatar back
    Profile p = companions::active();
    setField(p, "avatar", presetProfile(p.type).avatar);
    companions::save(p);
    applyCompanion();
  } else if (input.key == 'P') {  // dev: a test photo avatar for the active companion (a dithered disc)
    static uint8_t img[avatar::kPhotoBig + avatar::kPhotoSmall];
    memset(img, 0, sizeof(img));
    auto disc = [](uint8_t *bits, int n) {
      for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
          const float dx = x - n / 2 + 0.5f, dy = y - n / 2 + 0.5f, d = sqrtf(dx * dx + dy * dy);
          const bool ring = d <= n / 2 && d > n / 2 - 2, inside = d <= n / 2 - 2 && ((x / 4 + y / 4) % 2 == 0) && dy > -n / 6;
          if (ring || inside) bits[y * (n / 8) + x / 8] |= 0x80 >> (x % 8);
        }
    };
    disc(img, 128);
    disc(img + avatar::kPhotoBig, 48);
    File f = SD_MMC.open(companions::folder(companions::active().id) + "/photo.bin", FILE_WRITE);
    static uint8_t chunk[512];
    for (size_t at = 0; f && at < sizeof(img); at += sizeof(chunk)) {
      const size_t n = min(sizeof(chunk), sizeof(img) - at);
      memcpy(chunk, img + at, n);
      f.write(chunk, n);
    }
    f.close();
    Profile p = companions::active();
    setField(p, "avatar", "photo");
    companions::save(p);
    applyCompanion();
  }
  if (input.key == 'I') {  // dev: every companion's profile into the log (read with `d`)
    for (const std::string &id : companions::list().order()) {
      const Profile *p = companions::find(id);
      if (!p) continue;
      const std::string json = profileJson(*p);
      for (size_t at = 0; at < json.size(); at += 80) LOGI("profile", "%s", json.substr(at, 80).c_str());  // the log cuts long lines
    }
  }
  if (input.key == 'S') {  // dev: the next companion
    switchCompanion(stepCompanion(companions::list().order(), companions::active().id, 1));
    LOGI("companion", "now %s (%s)", companions::active().id.c_str(), companions::active().awake() ? "awake" : "asleep");
  }
  if (input.key == 'W') {  // dev: the active companion forgets its name (asleep again)
    Profile p = companions::active();
    p.name = "";
    companions::save(p);
    if (live.connected()) disconnect("asleep again");
    setState(Talk::Offline, idleStatus());
  }
  if (input.key == 'J' && live.connected()) live.sendRaw("not json");  // dev: the server closes the session
  if (input.key == 'N') {  // dev: show the memory note
    info.note = "Remembered";
    noteUntil = millis() + 3000;
    redraw = true;
  }
  if (info.note.length() && millis() > noteUntil) {
    info.note = "";
    redraw = true;
  }
  if (input.key == 'Q') {  // undo q
    usage::markOut(0);
    ring = KeyRing();
    ring.setCount(settings.keys.size());
    saveKeys();
    setState(Talk::Offline, idleStatus());
  }
  if (input.key == 'q') callbacks().failed("Resource has been exhausted (e.g. check quota).", -1, true, false);

  // The avatar's animation clock.
  if (millis() - lastFrameAt > kFrameMs && info.state != Talk::Listening) {
    lastFrameAt = millis();
    info.frame++;
    redraw = true;
  }
  if (redraw && !epd.isBusy()) {
    redraw = false;
    drawApp();
    const bool full = millis() - lastFullAt > kFullRefreshMs;
    if (full) lastFullAt = millis();
    shell::refresh(full, false);
  }
  delay(info.state == Talk::Listening ? 1 : 10);
}
