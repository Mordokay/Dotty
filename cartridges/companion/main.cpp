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
#include "adpcm_recorder.h"
#include "job_slot.h"
#include "macro_store.h"
#include "recall.h"
#include "relay_link.h"
#include "relay_path.h"
#include "script_writer.h"
#include "piece_buffer.h"
#include "ui.h"
#include "memories.h"
#include "moment_lines.h"
#include "moments.h"
#include "line_job.h"
#include "phone_wake.h"
#include "voice_note.h"
#include "note_deck.h"
#include "rec_messages.h"
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

DOTTY_CARTRIDGE("companion", "Companion", "0.6.0");

namespace {

using shell::epd;

constexpr uint32_t kHoldMs = 120;                   // BOOT held this long = listen (as in Tape)
constexpr uint32_t kIdleMs = 3 * 60 * 1000;         // no turn for this long: disconnect
// Through the iPhone a chat wraps up (review, close) after 20 s of quiet: iOS cuts the phone's idle
// connection 35-90 s after a reply. The next press resumes the conversation (within 10 minutes).
constexpr uint32_t kRelayIdleMs = 20 * 1000;
constexpr uint32_t kResumeKeepMs = 10 * 60 * 1000;
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
// Two minutes (a 300-word reading), kept as ADPCM (~1.4 MB) and written to the card as a WAV by a
// task once heard (a 2-minute file took seconds: the loop shouldn't stop for it).
constexpr size_t kLastMaxFrames = 120 * 24000;
AdpcmRecorder lastRec;
bool recording = false;  // this reply is being kept
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
enum class Job { Write, Recall, Line, Plan };
Job jobKind = Job::Write;
std::string jobCallId;           // recall's function call, answered with what was found
bool writing = false;            // a job asked, its follow-up not sent yet
bool reading = false;            // the reading turn is under way
bool readingStarts = false;      // its first text piece is next
std::vector<std::string> readingRest;  // a long reading's chunks still to send (script_writer.h readingChunks)
bool readingChunkDone = false;   // the last chunk has arrived: the next goes out once most of it has played
bool questionDone = false;       // the question's own turn (often "Give me a moment") has ended
bool written = false;            // the job's result has been taken (writtenResult is ready)
Written writtenResult;
std::string writerBody;          // the next job's request, built on the loop (the task gets a copy)
bool jobPending = false;         // asked while the last task (cancelled) was still running: starts after
JobSlot<Written> jobs;           // the task and its hand-over (job_slot.h)
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
// Nobody spoke (the user, 2026-10-24: an empty press came back as "¿Qué?" — Gemini's guess at near-silence):
// the turn only opens once ~0.3 s of sound has been heard (the voice before it waits in `early`); a press
// without it isn't sent and the screen says "I didn't hear anything". One loud 100 ms (the button's click,
// -17 dBFS once) isn't speech.
// -35 let a room's background through (-29 dBFS for seconds); a voice near Dotty peaks well above -28.
constexpr int kVoiceDb = -28, kVoiceChunks = 3;
int loudChunks = 0;

// Which way the chat goes (relay_path.h): through the iPhone, or Wi-Fi raced against it. The Wi-Fi
// task claims the session (wifiBegan) under a short lock before its WebSocket opens; a phone that wins
// before that cancels the task and the relay starts at once; after it, Wi-Fi keeps the chat (the grace
// window may still hand it to the phone).
RelayPath path;
relay::Transport relayTransport;
portMUX_TYPE raceLock = portMUX_INITIALIZER_UNLOCKED;
volatile bool wifiBegan = false;
bool phoneWasReady = false;
uint32_t relayOpenAt = 0;        // a relay chat's setup was sent (the phone has 10 s to answer)
uint32_t relayDistrustUntil = 0; // the phone didn't answer a setup: Wi-Fi for chats until then
std::string resumeRetryHandle;  // a hand-over's resume, retried while Google still holds the old session
int resumeRetries = 0;
uint32_t resumeRetryAt = 0;
// A moment just played by itself: a BOOT hold until then opens a chat that knows what was said.
uint32_t momentReplyUntil = 0;
std::string firedContext;   // firedNote(…) for that chat
bool momentOnScreen = false;  // a moment's text is up with no chat behind it (not a closed session)
// Recording a moment's line (line_job.h): the reading's audio goes to lineRec, not the speaker.
LineJob lineJob;
bool capturingLine = false;
bool lineDiscard = false;  // a stopped reading's turn is still under way: its audio, words and end are dropped
std::map<std::string, uint32_t> lineBackoff;  // "<companion>/<key>" → no new try before then (millis)
AdpcmRecorder lineRec;
LastLine linePending;      // its text pieces, timed
size_t lineFrames = 0;     // 24 kHz frames received
uint32_t lineCooldownUntil = 0, lineReadingAt = 0, lastLineScan = 0;
void abortLine(const char *why);
// The phone wake (phone_wake.h): a session opened only to record lines (no chat), and the daily plan.
PhoneWake phoneWake;
bool bgSession = false;     // the open (or opening) session records lines: no chat, the recording instruction
bool planRunning = false;   // the day's plan is with the text model
bool forcePlan = false;     // dev z: plan now, whatever the time
int bgFailures = 0;         // lines that failed in this wake: it gives up after two
int64_t lastWake = -1, plannedDay = -1;  // NVS companion/lastWake, plannedDay (read on first use)
uint32_t bgOpenedAt = 0;
uint32_t noteTestUntil = 0;  // dev n: a test voice note is recording until then
// Voice notes (voice_note.h): the deck, a recording to play once the reply ends, the reaction after Done.
NoteDeck deck;
bool deckOpen = false;
int16_t noteChunk[voice::kChunkFrames];  // one 100 ms buffer for every voice-note path (internal RAM is tight)
int16_t *noteSendBuf = nullptr;          // rec.fetch's frame buffer (PSRAM, made on first use)
std::string deckPendingTitle;  // start_recording: the deck opens once the reply has ended
bool deckPending = false;
uint32_t deckPendingAt = 0;  // a deck that hasn't opened within 2 minutes (the reply never ended) is dropped
std::string pendingPlayCompanion, pendingPlayName, pendingPlayTitle;
bool notePlaying = false;
struct NoteReaction {
  bool waiting = false, gotText = false;
  std::string companion, name, title, transcript;
  uint32_t ms = 0, until = 0;
} reaction;
std::string pendingTypedText, pendingTypedLabel;  // a typed turn waiting for a session
RecTextAssembler recText;  // companion.recording.text / .piece from the phone
// A whole recording the phone asked for (rec.fetch): sent a few frames a loop pass.
struct NoteSend {
  bool active = false;
  File file;
  std::string companion, name;
} noteSend;
std::map<std::string, int> syncTries;  // recordings a wake asked the phone to sync, by name (no endless wakes)
uint32_t lastSyncActivity = 0;
uint32_t pendingTypedSince = 0;
bool lineScanStale = false;  // the wake's cached "lines left?" must be read again (the plan added some)
const char *wakeVia = "nothing";  // how the last wake reached the internet (the log, for the battery measurements)
void dropPending();
bool jobsDue();
bool syncWanted();
bool recordingNeedsSync(const RecordingMeta &m);
void pollNoteSend();
void loadWakeTimes();
void pollPhoneWake();
void backgroundPoll();

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

// Where the user is, as the phone said (system data: core.place); "" when no phone has said.
String placeName() {
  String name;
  double lat, lon;
  return shell::place(name, lat, lon) ? name : String();
}

void cancelWriter();
GeminiLive::ToolAnswer applyMomentTool(const ToolCall &c);
void startWriter(const std::string &what, int seconds);
void startRecall(const ToolCall &c);
void startMacro(const Macro &m);
void loadMacros();

uint32_t resumeKeptAt = 0;  // when a conversation was kept to resume (kResumeKeepMs)

void disconnect(const char *why) {
  reviewStart = 0;
  abortLine(why);
  const bool wasBackground = bgSession;
  bgSession = false;
  ble::advertiseSlowly(false);
  if (wasBackground) live.forgetResume();  // a recording session is never resumed by a chat
  else if (!live.resumeHandle().empty()) resumeKeptAt = millis();
  path.end();
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
    LOGI("companion", "session low: internal %u KB free, largest %u KB (%s)", lowFree / 1024, lowLargest / 1024,
         live.overRelay() ? "through the iPhone" : ble::running() ? "Wi-Fi, Bluetooth on" : "Wi-Fi");
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
  dropPending();
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

// What a task works from: its own copies (the loop may start building the next job meanwhile).
struct WriterRequest {
  uint32_t job;
  Job kind;
  std::string body;
  String key;
};

void writerTask(void *arg) {
  WriterRequest *r = static_cast<WriterRequest *>(arg);
  const String url = String("https://generativelanguage.googleapis.com/v1beta/models/") + kWriterModel +
                     ":generateContent?key=" + r->key;
  std::string body;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 60000;  // the text model has taken 15-25 s on slow days (relay_link.h)
  config.event_handler = [](esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA) static_cast<std::string *>(e->user_data)->append(static_cast<const char *>(e->data), e->data_len);
    return ESP_OK;
  };
  config.user_data = &body;
  const uint32_t started = millis();
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, r->body.c_str(), r->body.size());
  const esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  Written w = err == ESP_OK ? parseWritten(status, body) : Written();
  if (err != ESP_OK) w.error = esp_err_to_name(err);
  const bool recall = r->kind == Job::Recall;
  LOGI(recall ? "recall" : "writer", "%s in %lu ms: %u characters, %d tokens%s%s",
       w.ok ? (recall ? w.text.substr(0, 120).c_str() : "written") : "failed",
       static_cast<unsigned long>(millis() - started), static_cast<unsigned>(w.text.size()), w.tokens,
       w.ok ? "" : ": ", w.error.c_str());
  const uint32_t job = r->job;
  delete r;
  jobs.finish(job, w);  // the loop takes it if nobody gave up on it meanwhile
  vTaskDelete(nullptr);
}

// The job in writerBody / jobKind, now; or once the last (cancelled) task has finished: never two
// TLS requests on top of Live.
// In a chat through the iPhone, Dotty has no Wi-Fi: the text-model call goes through the phone.
struct RelayFetch {
  bool active = false;
  uint32_t job = 0, id = 0, startedAt = 0;
} relayFetch;

void launchJob() {
  if (jobs.busy()) {
    jobPending = true;
    LOGI("writer", "waiting for the last request to end");
    return;
  }
  jobPending = false;
  // Through the phone: in a relay chat, or with no session at all (a phone wake's plan or line).
  if ((live.overRelay() && live.connected()) || (!live.connected() && relay::phoneReady())) {
    relayFetch = {true, jobs.start(), relay::fetch(kWriterModel, writerBody), millis()};
    LOGI(jobKind == Job::Recall ? "recall" : "writer", "through the iPhone (%u bytes)", static_cast<unsigned>(writerBody.size()));
    return;
  }
  WriterRequest *r = new WriterRequest{jobs.start(), jobKind, writerBody, sessionKeyText};
  const uint32_t job = r->job;
  if (xTaskCreatePinnedToCore(writerTask, "writer", 12288, r, 1, nullptr, 0) != pdPASS) {
    delete r;
    Written failed;
    failed.error = "no task";
    jobs.finish(job, failed);
  }
}

void startJob() {
  abortLine("the chat needs the text model");  // the chat's own long answer comes first
  writing = true;
  written = false;
  questionDone = false;
  launchJob();
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
  const std::vector<std::string> days = recallDays(c.from, c.to, now);
  const std::vector<std::string> newestFirst(days.rbegin(), days.rend());
  // Built a day at a time, never every exchange at once (recall.h).
  std::string block = recallBlock(newestFirst, [](const std::string &day) { return history::raw(day.c_str()); }, name, c.group);
  // Its voice notes from those days, word for word (the topic's ones when there's a topic).
  std::string notes;
  std::string topic = c.group;
  for (char &ch : topic) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
  for (const RecordingMeta &m : note::list(who.id)) {
    const std::string day = dayFile(m.created).substr(0, 10);
    if (std::find(days.begin(), days.end(), day) == days.end()) continue;
    std::string text = companions::readText(note::path(who.id, m.name, "txt"));
    if (text.empty()) continue;
    std::string lowerText = text + " " + m.title;
    for (char &ch : lowerText) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    if (!topic.empty() && lowerText.find(topic) == std::string::npos) continue;
    const std::string line = transcriptBlock(m, text);
    if (notes.size() + line.size() > 30000) break;
    notes += (notes.empty() ? "" : "\n") + line;
  }
  if (!notes.empty()) block += "\n\nVoice notes the user recorded:\n" + notes;
  writerBody = recallRequest(name, c.text, now, block);
  jobKind = Job::Recall;
  jobCallId = c.id;
  LOGI("recall", "\"%.80s\": %s..%s (%u days), topic \"%s\": %u bytes of chats, internal %u KB free", c.text.c_str(),
       days.front().c_str(), days.back().c_str(), static_cast<unsigned>(days.size()), c.group.c_str(),
       static_cast<unsigned>(block.size()), heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
  startJob();
}

void cancelWriter() {
  jobs.cancel();  // a running task's result is dropped
  jobPending = false;
  writing = reading = questionDone = written = false;
  readingRest.clear();
  readingChunkDone = false;
  longTokens = 0;
  if (info.writing || info.recalling) redraw = true;
  info.writing = info.recalling = false;
}

// Both done (the text, and the question's own turn): Live reads it, or answers itself if it failed.
void writerPoll() {
  if (relayFetch.active) {  // collected even when nobody waits for it any more: the slot frees
    int status = 0;
    std::string body;
    if (relay::takeFetched(relayFetch.id, status, body)) {
      Written w = status ? parseWritten(status, body) : Written();
      if (!status) w.error = body;
      LOGI("writer", "through the iPhone: %s in %lu ms (%u characters)", w.ok ? "written" : w.error.c_str(),
           static_cast<unsigned long>(millis() - relayFetch.startedAt), static_cast<unsigned>(w.text.size()));
      relayFetch.active = false;
      jobs.finish(relayFetch.job, w);
    }
  }
  // A long reading's next chunk, once most of the last has played (< 10 s left ahead of the speaker).
  if (reading && readingChunkDone && !readingRest.empty() && live.ready()) {
    const size_t played = voice::playedFrames();
    const uint32_t aheadMs = replyFrames > played ? (replyFrames - played) / 24 : 0;
    if (aheadMs < 10000) {
      readingChunkDone = false;
      readingStarts = true;  // a space between the chunks' text
      LOGI("writer", "next chunk (%u left, %lu ms ahead)", static_cast<unsigned>(readingRest.size() - 1), static_cast<unsigned long>(aheadMs));
      live.sendText(continueReadingNote(readingRest.front()), true);
      readingRest.erase(readingRest.begin());
      lastTurnAt = millis();
    }
  }
  if (!writing) return;
  if (jobPending && !jobs.busy()) launchJob();
  if (!written && jobs.take(writtenResult)) written = true;
  if (!written) return;
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
  else if (!writtenResult.ok) live.sendText(writerFailedNote(), true);
  else {  // read in chunks of ~100 words (a 300-word text is two minutes; Dotty buffers one)
    std::vector<std::string> chunks = readingChunks(writtenResult.text, 100);
    if (chunks.empty()) chunks.push_back(writtenResult.text);
    readingRest.assign(chunks.begin() + 1, chunks.end());
    readingChunkDone = false;
    LOGI("writer", "reading %u chunks", static_cast<unsigned>(chunks.size()));
    live.sendText(readAloudNote(chunks.front()), true);
  }
  if (info.state != Talk::Speaking) setState(Talk::Thinking);
  lastTurnAt = millis();
  redraw = true;
}

// ---------- Gemini callbacks (run inside live.poll(), on the main loop) ----------

GeminiLive::Callbacks callbacks() {
  GeminiLive::Callbacks cb;
  cb.ready = [] {
    ble::advertiseSlowly(false);
    LOGI("companion", "ready; heap %u KB internal (largest %u)", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    lastTurnAt = millis();
    awaitingConnect = false;
    if (!live.overRelay()) path.wifiReady(millis());
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
    if (lineDiscard) return;
    if (capturingLine) {  // a moment's line being recorded: its words, timed to its audio
      linePending.add(lineFrames / 24, piece);
      return;
    }
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
    if (lineDiscard) return;
    if (capturingLine) {  // to the card, not the speaker: the moment stays a surprise
      lineRec.add(pcm, frames);
      lineFrames += frames;
      return;
    }
    if (info.state != Talk::Thinking && info.state != Talk::Speaking) return;
    if (!voice::playing()) beginReply();
    voice::play(pcm, frames);
    replyFrames += frames;
    if (recording) lastRec.add(pcm, frames);  // kept for replay (the first two minutes)
  };
  cb.mood = [](const std::string &m) {
    turnMood = m.c_str();
    info.mood = turnMood;
  };
  cb.spent = [](int tokens) { usage::addTokens(tokens, sessionKey); };
  cb.memory = [](const ToolCall &c) { return applyMemory(c); };
  cb.tool = [](const ToolCall &c) { return applyMomentTool(c); };
  cb.turnDone = [](int tokens) {
    ring.forgive(sessionKey);  // a good turn: this key's quota strikes start over
    if (lineDiscard) {  // the end of a stopped reading: nothing to keep
      usage::addTokens(tokens, sessionKey);
      lineDiscard = false;
      return;
    }
    if (capturingLine) {  // a chunk of a moment's line has been read
      usage::addTokens(tokens, sessionKey);
      lineJob.chunkDone();
      lastTurnAt = millis();
      return;
    }
    if (reviewStart) {  // the end-of-chat review: its memory calls are applied, nothing else to keep
      usage::addTokens(tokens, sessionKey);
      LOGI("companion", "memory review done (%d tokens)", tokens);
      if (live.overRelay()) live.keepResume();  // the next press carries on this conversation
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
    if (reading && !readingRest.empty()) {  // a chunk has arrived: the next goes out in writerPoll
      longTokens += tokens;
      readingChunkDone = true;
      lastTurnAt = millis();
      return;
    }
    reading = false;
    tokens += longTokens;
    longTokens = 0;
    path.replyEnded(millis());
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
    // Taking a Wi-Fi chat over: Google hadn't let go of the session yet ("already connected to an
    // existing client") — try the same resume again shortly (up to 3 times).
    if (live.overRelay() && error.find("already connected") != std::string::npos && !resumeRetryHandle.empty() &&
        resumeRetries < 3) {
      resumeRetries++;
      resumeRetryAt = millis() + 1000;
      LOGI("companion", "Google still holds the Wi-Fi session: resuming again in 1 s (%d)", resumeRetries);
      return;
    }
    if (live.overRelay() && reviewStart && !quota && !badKey && ble::connected()) {
      // The end-of-chat review over a connection the iPhone already dropped: the chat is over
      // anyway — close quietly and let the next press resume the conversation.
      LOGI("companion", "the iPhone's connection ended before the review: the next press resumes");
      live.keepResume();
      disconnect("closed after the chat");
      setState(Talk::Offline, idleStatus());
      return;
    }
    if (live.overRelay()) path.phoneLost();
    if (connecting && !live.overRelay()) {  // inside the connect task: the loop reports it once the task ends
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
      setState(Talk::Error, error == "Lost the phone" ? "Lost the phone" : "Connection lost");
    }
  };
  return cb;
}

// ---------- connecting ----------

LiveConfig sessionConfig() {
  LiveConfig config;
  if (bgSession) {  // recording lines: the companion's voice and character, nothing else
    config.voice = sessionVoice.c_str();
    config.systemInstruction = recordingInstruction(personaSection);
    return config;
  }
  config.resumeHandle = live.resumeHandle();  // after a goAway: carry on the conversation
  config.voice = sessionVoice.c_str();
  config.textOnly = settings.textOnly;
  config.systemInstruction = systemInstruction(nowText(time(nullptr)), placeName().c_str(), personaSection, memorySection);
  return config;
}

// The chat through the iPhone (resume: the Wi-Fi chat's handle when the phone takes it over).
void beginRelay(const std::string &resume = "") {
  LiveConfig config = sessionConfig();
  if (!resume.empty()) config.resumeHandle = resume;
  awaitingConnect = true;  // cleared when Gemini is ready
  relayOpenAt = millis();
  if (!live.begin(sessionKeyText.c_str(), config, callbacks(), relayTransport)) {
    awaitingConnect = false;
    path.phoneLost();
    return setState(Talk::Error, "Lost the phone");
  }
  LOGI("companion", "chat through the iPhone%s", resume.empty() ? "" : " (taking over the Wi-Fi chat)");
  if (info.state != Talk::Listening && !awaitingSend) setState(Talk::Connecting, "Waking up...");
}

// The phone arrived in a Wi-Fi chat's grace window: the chat moves to it, the conversation resumed.
void handOver() {
  const std::string handle = live.latestHandle();
  resumeRetryHandle = handle;
  resumeRetries = 0;
  live.close();
  net::disconnect();
  power::setWakeLock(power::kWakeLockNetwork, false);
  beginRelay(handle);
}

void connectLoop(void *) {
  String error;
  bool ok = net::connect(error);
  if (!ok && !cancelConnect) {  // a join sometimes times out once and works straight after (seen at boot)
    LOGW("companion", "Wi-Fi: %s, trying again", error.c_str());
    error = "";
    ok = net::connect(error);
  }
  bool claimed = false;  // the session is Wi-Fi's (the phone didn't win first)
  if (ok) {
    taskENTER_CRITICAL(&raceLock);
    if (!cancelConnect) claimed = wifiBegan = true;
    taskEXIT_CRITICAL(&raceLock);
  }
  if (ok && claimed) {
    ok = live.begin(sessionKeyText.c_str(), sessionConfig(), callbacks());
    if (!ok) error = "Can't reach Gemini";
  } else if (ok) {
    error = "the phone took the chat";
  } else {
    LOGW("companion", "Wi-Fi: %s", error.c_str());
    error = "No Wi-Fi";
  }
  if (!ok && connectError.isEmpty()) connectError = error;
  if (!ok)
    LOGW("companion", "connect failed: %s (internal %u KB free, largest %u KB)", error.c_str(),
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
  if (cancelConnect) {  // locked, stopped or the phone won meanwhile: undo what this task opened
    if (claimed) live.close();
    net::disconnect();
  }
  connecting = false;
  connectTask = nullptr;
  vTaskDelete(nullptr);
}

void connect() {
  if (connecting || live.connected()) return;
  if (!live.resumeHandle().empty() && millis() - resumeKeptAt > kResumeKeepMs) live.forgetResume();  // too old
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
  // The phone (connected, said hello, holds this key) takes the chat; else Wi-Fi is raced against it
  // with Bluetooth left on (mbedTLS in PSRAM made that fit; Bluetooth goes off once Wi-Fi has won and
  // the grace window is over).
  const bool distrusted = relayDistrustUntil && static_cast<int32_t>(relayDistrustUntil - millis()) > 0;
  const bool phone = relay::phoneReady() && relay::phoneHasKey(keyHint(sessionKeyText)) && !distrusted;
  path.press(millis(), phone);
  phoneWasReady = phone;
  power::setWakeLock(power::kWakeLockNetwork, true);
  memorySection = memories::store().promptSection();  // the task only reads these copies
  {  // the last ten chats' summaries: what it has talked about lately
    File f = SD_MMC.open(companions::folder(companions::active().id) + "/summaries.jsonl");
    std::string sums;
    while (f && f.available()) sums += static_cast<char>(f.read());
    f.close();
    const std::string section = summariesSection(sums, 10);
    if (!section.empty()) memorySection += (memorySection.empty() ? "" : "\n\n") + section;
    const std::string notes = recordingsSection(note::list(companions::active().id), 10);  // its voice notes
    if (!notes.empty()) memorySection += (memorySection.empty() ? "" : "\n\n") + notes;
  }
  personaSection = personaText(companions::active());
  if (momentReplyUntil && static_cast<int32_t>(momentReplyUntil - millis()) > 0) {  // it just said something by itself
    personaSection += "\n\n" + firedContext;
    momentReplyUntil = 0;
  }
  if (const MomentList *set = moments::of(companions::active().id)) {  // what it has been asked to say later
    const std::string section = momentsSection(*set, time(nullptr));
    if (!section.empty()) personaSection += "\n\n" + section;
  }
  if (!macroList.all().empty()) personaSection += "\n\n" + macrosSection(macroList);  // its phrases → run_macro
  sessionCompanion = companions::active().id;
  evolvedThisChat = false;
  connectError = "";
  if (phone) return beginRelay();
  LOGI("companion", "no phone yet: racing Wi-Fi against it");
  ble::advertiseSlowly(true);  // the Wi-Fi scan gets most of the radio; the phone still finds Dotty
  wifiBegan = false;
  cancelConnect = false;
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
  momentOnScreen = false;
  abortLine("talking");
  lineDiscard = false;  // the new question's start drains whatever is left of it
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
  loudChunks = 0;
  early.clear();
  earlyAt = 0;
  turnSent = awaitingSend = false;
  if (!voice::startListening()) return;
  if (!live.ready()) info.heard = "Listening... I'll send it once I'm connected.";  // the turn opens on speech
  setState(Talk::Listening);
}

void finishTurn() {
  live.endTurn();
  LOGI("companion", "sent %.1f s of voice, peak %d dBFS, %d loud chunks; heap %u KB internal (largest %u), %u KB PSRAM",
       sentFrames / float(voice::kInRate), peakDb, loudChunks, heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
       heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024, heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
  releasedAt = millis();
}

void stopTalking() {
  if (loudChunks < kVoiceChunks && !turnSent) {  // nothing said: nothing sent
    voice::stopListening();
    early.clear();
    earlyAt = 0;
    LOGI("companion", "press with no voice (%d loud chunks, peak %d dBFS): not sent", loudChunks, peakDb);
    setState(live.connected() || connecting ? Talk::Ready : Talk::Offline, "I didn't hear anything");
    return;
  }
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

// ---------- moments: reminders, alarms and timers set by voice (moments.h) ----------

void notifyMoments() {
  JsonDocument event;
  event["event"] = "companion.moments";
  ble::notify(event);
}

void notifyRecordings() {
  JsonDocument event;
  event["event"] = "companion.recordings";
  ble::notify(event);
}

void notifyTimer() {
  JsonDocument event;
  event["event"] = "companion.timer";
  ble::notify(event);
}

std::string jsonText(const JsonDocument &doc) {
  std::string out;
  serializeJson(doc, out);
  return out;
}

GeminiLive::ToolAnswer toolError(const char *error) {
  JsonDocument doc;
  doc["error"] = error;
  return {jsonText(doc), true};  // spoken: the model puts it right with the user
}

// "mon", "Monday", "wed" → the day's bit (moment_store.h: Monday = bit 0).
uint8_t dayBits(const std::vector<std::string> &days) {
  static const char *kNames[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
  uint8_t bits = 0;
  for (std::string d : days) {
    for (char &ch : d) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    for (int i = 0; i < 7; i++)
      if (d.compare(0, 3, kNames[i]) == 0) bits |= 1 << i;
  }
  return bits;
}

void showNote(const String &note) {
  info.note = note;
  noteUntil = millis() + 3000;
  redraw = true;
}

// set_moment / cancel_moment / list_moments / start_timer / stop_timer from the model.
GeminiLive::ToolAnswer applyMomentTool(const ToolCall &c) {
  const int64_t now = time(nullptr);
  const bool ours = sessionCompanion == companions::active().id;  // switched mid-chat: nothing lands
  TimerState &timer = moments::timer();
  if (c.name == "start_timer" || c.name == "stop_timer") {
    if (!ours) return toolError("unavailable");
    JsonDocument doc;
    if (c.name == "stop_timer") {
      if (!timer.running) return toolError("no timer is running");
      if (moments::available()) moments::dropRecording(timer.companion, "timer");
      timer.cancel();
      moments::saveTimer();
      notifyTimer();
      LOGI("moments", "timer stopped");
      return {"ok", false};
    }
    if (timer.running) {
      doc["error"] = "a timer is running";
      doc["label"] = timer.label;
      doc["left"] = timer.secondsLeft(now);
      return {jsonText(doc), true};
    }
    if (!timer.start(c.seconds, c.label, c.tickEvery, sessionCompanion, now))
      return toolError("a timer runs from 1 second to 24 hours");
    timer.what = c.text;  // "sing happy birthday": what its end says (the user, 2026-10-24)
    timer.style = validStyle(c.style) ? c.style : "";
    moments::saveTimer();
    notifyTimer();
    showNote("Timer set");
    LOGI("moments", "timer: %d s \"%s\"%s%s%s", c.seconds, c.label.c_str(), c.tickEvery ? " with ticks" : "",
         c.text.empty() ? "" : ", then: ", c.text.c_str());
    doc["endsAt"] = localTimeText(timer.endsAt);
    return {jsonText(doc), false};
  }
  if (c.name == "start_recording" || c.name == "play_recording" || c.name == "delete_recording") {
    if (!ours || !storage::available()) return toolError("unavailable");
    if (c.name == "start_recording") {
      const uint64_t freeMb = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
      if (freeMb < 50) return toolError("no room on the card");
      deckPendingTitle = c.label.empty() ? "Voice note" : c.label;
      deckPending = true;
      deckPendingAt = millis();
      LOGI("note", "start_recording \"%s\": the deck opens after this reply", deckPendingTitle.c_str());
      return {"ok", false};
    }
    const std::vector<RecordingMeta> all = note::list(sessionCompanion);
    const int i = matchRecording(all, c.text, now);
    if (i < 0) return toolError(all.empty() ? "they have no recordings yet" : "no recording matches that");
    JsonDocument doc;
    if (c.name == "play_recording") {
      pendingPlayCompanion = sessionCompanion;
      pendingPlayName = all[i].name;
      pendingPlayTitle = all[i].title;
      doc["playing"] = all[i].title;
    } else {
      note::remove(sessionCompanion, all[i].name);
      LOGI("note", "deleted %s (%s)", all[i].name.c_str(), all[i].title.c_str());
      doc["deleted"] = all[i].title;
      notifyRecordings();
    }
    return {jsonText(doc), false};
  }
  MomentList *list = ours ? moments::of(sessionCompanion) : nullptr;
  if (!list) return toolError("unavailable");
  if (c.name == "list_moments") return {listMomentsText(*list, now), true};
  if (c.name == "cancel_moment") {
    const Moment *m = list->find(c.memoryId);
    if (!m || m->kind == MomentKind::Spontaneous) return toolError("no such moment");
    LOGI("moments", "cancelled %d: %s", c.memoryId, m->what.c_str());
    list->remove(c.memoryId);
    moments::dropRecording(sessionCompanion, std::to_string(c.memoryId));
    moments::save(sessionCompanion);
    notifyMoments();
    return {"ok", false};
  }
  // set_moment
  Moment m;
  if (!kindFromName(c.kind.empty() ? "reminder" : c.kind, m.kind) || m.kind == MomentKind::Spontaneous)
    return toolError("kind must be reminder or alarm");
  if (!parseLocalTime(c.at, m.at)) return toolError("at must be YYYY-MM-DDTHH:MM");
  m.at -= m.at % 60;  // on the minute
  if (m.at <= now) return toolError("that time has passed");
  if (!repeatFromName(c.repeat, m.repeat)) m.repeat = Repeat::Once;
  m.days = dayBits(c.days);
  if (m.repeat == Repeat::Days && !m.days) return toolError("which days?");
  if (!firstOccurrence(m)) return toolError("which days?");  // "weekdays" asked on a Saturday: Monday
  m.what = c.text;
  m.style = validStyle(c.style) ? c.style : "spoken";
  m.source = "chat";
  m.created = now;
  if (static_cast<int>(list->all().size()) >= MomentList::kMax) return toolError("full: 50 moments are set");
  const int id = list->add(m);
  if (id < 0) return toolError("say what it's about");
  moments::save(sessionCompanion);
  notifyMoments();
  showNote(m.kind == MomentKind::Alarm ? "Saving your alarm..." : "Saving your reminder...");
  LOGI("moments", "set_moment: id %d at %s (%s, %s): %s", id, localTimeText(m.at).c_str(), kindName(m.kind),
       m.style.c_str(), m.what.c_str());
  JsonDocument doc;
  doc["id"] = id;
  doc["at"] = localTimeText(m.at);
  return {jsonText(doc), false};
}

// ---------- moments firing (moment_schedule.h): beeps, the line, alarms ringing ----------
// A due group (a companion's moments at one minute) unlocks Dotty and plays: the attention beeps, a
// pause, its recording with the text paced by it (or each member's own recording, or just the text);
// alarms ring every minute until BOOT (a tap snoozes). Several groups at once play one after the
// other, the beeps only before the first.

struct Clip {
  String wav;       // "" = no recording: the text alone
  LastLine timing;  // its text pieces, timed to the audio
  std::string text;
};

struct Firing {
  bool active = false;
  FireGroup group;
  std::vector<Clip> clips;
  size_t clip = 0;
  enum class Phase { Beeps, Gap, Clip, ClipGap, Ended } phase = Phase::Beeps;
  uint32_t phaseAt = 0;
  bool beeps = true;  // the attention beeps before it (only the first of several at once)
  Delivery delivery = Delivery::Voice;
  std::string shown;  // the text so far (earlier clips + the current one's heard part)
  std::string line;   // all of it (for the history and the chat that may follow)
  std::string kind;   // reminder, alarm, spontaneous
  uint32_t hold = 0;  // the gap after a clip shown as text only (time to read it)
  bool preview = false;  // played from the app: nothing moves on or is deleted
  uint32_t startedMs = 0;  // a BOOT press already down before this doesn't answer it
} firing;
std::vector<FireGroup> firingQueue;
bool previewFiring = false;  // companion.moment.play: plays it without moving it on or deleting anything
Ringing ringing;
String missedAlarmLine;    // "Missed alarm 07:30" on the lock screen until the next unlock
uint32_t lastMomentPoll = 0;

void stopReplay();

const char *momentIcon(const std::string &kind) {
  return kind == "alarm" ? "alarm" : kind == "spontaneous" ? "speech" : kind == "timer" ? "timer" : "bell";
}

// What a group will play: its shared recording, else each member's own (or its text).
std::vector<Clip> clipsFor(const FireGroup &g, MomentList &list) {
  std::vector<const Moment *> members;
  for (const int id : g.ids)
    if (const Moment *m = list.find(id)) members.push_back(m);
  auto clipOf = [&](const std::string &key, const std::string &sig, const std::string &fallback) {
    Clip c;
    c.text = fallback;
    if (!moments::recorded(g.companion, key, sig)) return c;
    LastLine l;
    if (parseLastLine(companions::readText(moments::recordingPath(g.companion, key, "json")), l)) {
      c.wav = moments::recordingPath(g.companion, key, "wav");
      c.timing = l;
      if (!l.text().empty()) c.text = l.text();
    }
    return c;
  };
  std::vector<Clip> out;
  if (members.size() > 1) {
    std::string joined;
    for (const Moment *m : members) joined += (joined.empty() ? "" : " ") + (m->line.empty() ? m->what + "." : m->line);
    const Clip together = clipOf("group-" + std::to_string(g.minute), groupSignature(members), joined);
    if (!together.wav.isEmpty()) return {together};
  }
  for (const Moment *m : members)
    out.push_back(clipOf(std::to_string(m->id), groupSignature({m}), m->line.empty() ? m->what + "." : m->line));
  return out;
}

bool momentBusy() {
  return info.state == Talk::Listening || info.state == Talk::Thinking || info.state == Talk::Speaking || awaitingSend ||
         writing || reading || deckOpen;  // nothing beeps into a voice note
}

void showFiring() {
  momentOnScreen = true;
  lastTurnAt = millis();  // the text stays up like a reply's (the idle check would clear it at once)
  info.momentIcon = momentIcon(firing.kind);
  info.said = firing.shown.c_str();
  info.page = -1;
  if (info.state != Talk::After) setState(Talk::After);
  redraw = true;
}

// The 5 rising attention notes (~2 s): "pay attention, I'm going to speak".
void playAttention() {
  static const AudioPlayer::Note kAttention[] = {{72, 300}, {0, 100}, {76, 300}, {0, 100}, {79, 300},
                                                 {0, 100}, {84, 300}, {0, 100}, {88, 400}};
  player.playNotes(kAttention, sizeof(kAttention) / sizeof(kAttention[0]));
}

void endFiring();

// From the beeps through every clip; called again for each ring of an alarm.
void playFiring(bool withBeeps) {
  firing.clip = 0;
  firing.shown.clear();
  const bool voice = firing.delivery == Delivery::Voice;
  if (firing.group.alarm) player.setVolume(moments::rules().alarmVolume);
  if (withBeeps && firing.delivery != Delivery::TextOnly) {
    if (firing.kind == "timer") {  // time's up: a fanfare
      static const AudioPlayer::Note kFanfare[] = {{67, 120}, {72, 120}, {76, 120}, {79, 120}, {84, 400}};
      player.playNotes(kFanfare, sizeof(kFanfare) / sizeof(kFanfare[0]));
    } else {
      playAttention();
    }
    firing.phase = Firing::Phase::Beeps;
  } else {
    firing.phase = Firing::Phase::Gap;
  }
  firing.phaseAt = millis();
  if (!voice) firing.shown = firing.line;  // beeps (or nothing) and the text, no voice
  showFiring();
}

void startFiring(const FireGroup &g, bool beeps) {
  MomentList *list = moments::of(g.companion);
  if (!list) return;
  shell::wake();  // the screen lights up like an alarm clock
  stopReplay();
  firing = Firing();
  firing.active = true;
  firing.startedMs = millis();
  firing.group = g;
  firing.beeps = beeps;
  firing.clips = clipsFor(g, *list);
  const int64_t now = time(nullptr);
  firing.delivery = delivery(moments::rules(), g.alarm, now);
  std::string kind = "reminder";
  for (const int id : g.ids)
    if (const Moment *m = list->find(id)) {
      if (m->kind == MomentKind::Alarm) kind = "alarm";
      else if (m->kind == MomentKind::Spontaneous && kind == "reminder") kind = "spontaneous";
    }
  firing.kind = kind;
  for (const Clip &c : firing.clips) firing.line += (firing.line.empty() ? "" : " ") + c.text;
  LOGI("moments", "firing %s's %s at %s: %u moment(s), %u clip(s)%s", g.companion.c_str(), kind.c_str(),
       localTimeText(g.minute * 60).c_str(), static_cast<unsigned>(g.ids.size()), static_cast<unsigned>(firing.clips.size()),
       firing.delivery == Delivery::Voice ? "" : firing.delivery == Delivery::BeepsAndText ? " (quiet: beeps and text)" : " (quiet: text only)");
  firing.preview = previewFiring;
  previewFiring = false;
  // Moved on now (a repeating one to its next time), so a pause or a restart never fires it twice.
  if (!firing.preview) {
    for (const int id : g.ids)
      if (Moment *m = list->find(id)) advance(*m, now, false);
    moments::save(g.companion);
    notifyMoments();
  }
  if (g.alarm && !firing.preview) ringing.start(millis(), true);
  playFiring(beeps);
}

// Every pass while a moment plays.
void pollFiring() {
  const uint32_t now = millis();
  shell::wake();
  switch (firing.phase) {
    case Firing::Phase::Beeps:
      if (!player.isPlaying()) {
        firing.phase = Firing::Phase::Gap;
        firing.phaseAt = now;
      }
      break;
    case Firing::Phase::Gap:
    case Firing::Phase::ClipGap: {
      const uint32_t gap = firing.phase == Firing::Phase::Gap ? 500 : firing.hold ? firing.hold : 700;
      if (now - firing.phaseAt < gap) break;
      firing.hold = 0;
      if (firing.delivery != Delivery::Voice || firing.clip >= firing.clips.size()) {
        firing.phase = Firing::Phase::Ended;
        firing.phaseAt = now;
        break;
      }
      const Clip &c = firing.clips[firing.clip];
      if (c.wav.isEmpty() || !player.play(c.wav.c_str())) {  // no recording: its text, a moment to read it
        firing.shown += (firing.shown.empty() ? "" : " ") + c.text;
        showFiring();
        firing.clip++;
        firing.phase = Firing::Phase::ClipGap;
        firing.phaseAt = now;
        firing.hold = 3000;  // a moment on screen to read it
      } else {
        firing.phase = Firing::Phase::Clip;
        firing.phaseAt = now;
      }
      break;
    }
    case Firing::Phase::Clip: {
      const Clip &c = firing.clips[firing.clip];
      const std::string heard = c.timing.textAt(player.positionMs());
      std::string before;
      for (size_t i = 0; i < firing.clip; i++) before += (before.empty() ? "" : " ") + firing.clips[i].text;
      const std::string shown = before + (before.empty() || heard.empty() ? "" : " ") + heard;
      if (shown != firing.shown && now - lastTextAt > 1000) {
        firing.shown = shown;
        lastTextAt = now;
        showFiring();
      }
      if (!player.isPlaying()) {
        firing.hold = 0;
        firing.shown = before + (before.empty() ? "" : " ") + c.text;
        showFiring();
        firing.clip++;
        firing.phase = Firing::Phase::ClipGap;
        firing.phaseAt = now;
      }
      break;
    }
    case Firing::Phase::Ended:
      if (!ringing.active()) return endFiring();
      switch (ringing.poll(now)) {  // an alarm: again every minute until BOOT
        case Ringing::Action::Play:
          if (now - firing.phaseAt > 1000) playFiring(true);
          break;
        case Ringing::Action::GiveUp: {
          char at[8];
          const time_t minute = static_cast<time_t>(firing.group.minute * 60);
          tm t;
          gmtime_r(&minute, &t);
          snprintf(at, sizeof(at), "%02d:%02d", t.tm_hour, t.tm_min);
          missedAlarmLine = String("Missed alarm ") + at;
          LOGI("moments", "alarm unanswered for 10 minutes: %s", missedAlarmLine.c_str());
          return endFiring();
        }
        default:
          break;
      }
      break;
  }
}

String lastPath(const std::string &id, const char *ext);
void loadLast();

// A copy of a repeating moment's recording (it plays again tomorrow), on a task: a minute is ~3 MB.
struct LastCopy {
  String from, to;
};

void lastCopyTask(void *arg) {
  LastCopy *c = static_cast<LastCopy *>(arg);
  File in = SD_MMC.open(c->from);
  File out = SD_MMC.open(c->to + ".part", FILE_WRITE);
  static uint8_t buf[4096];
  bool ok = in && out;
  for (int n; ok && (n = in.read(buf, sizeof(buf))) > 0;) ok = out.write(buf, n) == static_cast<size_t>(n);
  in.close();
  out.close();
  if (ok) {
    SD_MMC.remove(c->to);
    ok = SD_MMC.rename(c->to + ".part", c->to);
  }
  LOGI("moments", "replay copy %s", ok ? "saved" : "NOT saved");
  delete c;
  vTaskDelete(nullptr);
}

// The moment's words become its companion's last line, so ▶ replays them like a reply (the user, 2026-10-24).
void keepAsLastLine() {
  const Clip *kept = nullptr;
  for (const Clip &c : firing.clips)
    if (!c.wav.isEmpty()) kept = &c;  // several recordings: the last one
  if (!kept || firing.delivery != Delivery::Voice) return;
  const std::string who = firing.group.companion;
  LastLine line = kept->timing;
  line.mood = "neutral";
  line.t = time(nullptr);
  const std::string json = lastLineJson(line);
  if (!companions::writeText(lastPath(who, "json"), json)) return;
  // A one-off's recording is deleted next anyway: moved, at once. A repeating one's is copied.
  bool repeating = false;  // the kept file is a repeating moment's own recording
  if (MomentList *list = moments::of(who))
    for (const int id : firing.group.ids)
      if (const Moment *m = list->find(id))
        repeating = repeating || (m->repeat != Repeat::Once && kept->wav == moments::recordingPath(who, std::to_string(id), "wav"));
  if (!repeating) {
    SD_MMC.remove(lastPath(who, "wav"));
    if (!SD_MMC.rename(kept->wav, lastPath(who, "wav"))) return;
  } else {
    LastCopy *c = new LastCopy{kept->wav, lastPath(who, "wav")};
    if (xTaskCreatePinnedToCore(lastCopyTask, "last-copy", 6144, c, 1, nullptr, 0) != pdPASS) delete c;
  }
  if (who == companions::active().id) {
    lastLine = line;
    hasLast = true;
  }
}

void endFiring() {
  player.stop();
  player.setVolume(settings.volume);
  ringing.stop();
  if (firing.preview) {  // the app's "Play on Dotty": as it was
    LOGI("moments", "preview done");
    firing.active = false;
    momentOnScreen = false;
    info.momentIcon = "";
    setState(Talk::Offline, idleStatus());
    return;
  }
  keepAsLastLine();
  if (MomentList *list = moments::of(firing.group.companion)) {
    for (const int id : firing.group.ids) {  // one-offs are done: they go with their recordings (the history keeps them)
      const Moment *m = list->find(id);
      if (m && m->state == MomentState::Upcoming) continue;
      moments::dropRecording(firing.group.companion, std::to_string(id));
      list->remove(id);
    }
    moments::save(firing.group.companion);
  }
  moments::dropRecording(firing.group.companion, "group-" + std::to_string(firing.group.minute));
  if (firing.kind == "timer") {  // the timer is over
    moments::dropRecording(firing.group.companion, "timer");
    moments::timer().cancel();
    moments::saveTimer();
    notifyTimer();
  }
  if (firing.group.companion == companions::active().id) {  // history: the active companion's folder
    history::append("", firing.line, "neutral", 0, 0, "moment: " + firing.kind);
    historyTurns++;
    historyNews = true;
    historyNewsCompanion = firing.group.companion;
  }
  if (firing.kind != "alarm") {
    momentReplyUntil = millis() + 180000;
    firedContext = firedNote(firing.line, firing.kind);
  }
  LOGI("moments", "%s done", firing.kind.c_str());
  firing.active = false;
  lastTurnAt = millis();
  info.momentIcon = "";
  info.said = firing.line.c_str();
  redraw = true;
  if (!firingQueue.empty()) {  // the next companion's, without beeps
    const FireGroup next = firingQueue.front();
    firingQueue.erase(firingQueue.begin());
    startFiring(next, false);
  }
}

// BOOT on a playing moment: an alarm is answered ("I'm up"), anything else just stops.
void answerFiring() {
  LOGI("moments", "%s answered", firing.kind.c_str());
  firingQueue.clear();
  const bool alarm = firing.kind == "alarm";
  endFiring();
  if (alarm) showNote("Good morning!");
}

// A tap on a playing moment: snooze an alarm (5 minutes, 3 times; the 4th = BOOT); else stop it.
void tapFiring() {
  if (firing.kind != "alarm") return answerFiring();
  player.stop();
  if (!ringing.snooze(millis())) return answerFiring();
  firing.phase = Firing::Phase::Ended;
  firing.phaseAt = millis();
  const time_t until = time(nullptr) + Ringing::kSnoozeMs / 1000;
  tm t;
  gmtime_r(&until, &t);
  char note[24];
  snprintf(note, sizeof(note), "Snoozed until %02d:%02d", t.tm_hour, t.tm_min);
  showNote(note);
  LOGI("moments", "%s (%d of %d)", note, ringing.snoozes(), Ringing::kMaxSnoozes);
}

// ---------- the timer (timer_beeps.h): its sounds as it counts down, its end ----------

int lastTimerLeft = -1;      // the second last heard (each new one may play a cue)
int lastTimerStep = -1;      // the screen shows the time left in steps of 10 s (1 s in the last minute)
bool timerVolume = false;    // a countdown beep set its own volume: back to the companion's after
bool timerConfirm = false;   // "Cancel the timer?" is up

bool timerOpen = false;       // the timer screen is open (the chip's tap, or its last minute)
bool timerAutoOpened = false;  // its last minute opened it already (back keeps it closed)

bool timerShowing() {
  return timerOpen && moments::timer().running && !firing.active && !settingsOpen && !historyOpen && !deckOpen;
}

// Time's up: a fanfare, the companion's recorded line (or the words), ringing like an alarm until BOOT or a tap.
void startTimerEnd() {
  TimerState &t = moments::timer();
  shell::wake();
  stopReplay();
  timerConfirm = false;
  firing = Firing();
  firing.active = true;
  firing.startedMs = millis();
  firing.kind = "timer";
  firing.group.companion = t.companion;
  firing.group.alarm = true;  // the alarm volume
  firing.delivery = Delivery::Voice;
  Clip c;
  c.text = t.title() + ": time's up!";
  const std::string sig = t.signature();
  LastLine l;
  if (moments::recorded(t.companion, "timer", sig) &&
      parseLastLine(companions::readText(moments::recordingPath(t.companion, "timer", "json")), l)) {
    c.wav = moments::recordingPath(t.companion, "timer", "wav");
    c.timing = l;
    if (!l.text().empty()) c.text = l.text();
  }
  firing.clips.push_back(c);
  firing.line = c.text;
  LOGI("moments", "timer \"%s\" is up%s", t.label.c_str(), c.wav.isEmpty() ? " (no recording)" : "");
  ringing.start(millis(), false);  // BOOT or a tap stops it; no snooze
  playFiring(true);
}

void pollTimer() {
  TimerState &t = moments::timer();
  if (timerVolume && !player.isPlaying()) {
    player.setVolume(settings.volume);
    timerVolume = false;
  }
  if (!t.running) timerOpen = timerAutoOpened = false;  // the next timer starts closed
  if (!t.running || t.paused || firing.active) return;
  const int64_t now = time(nullptr);
  if (now - t.endsAt >= 3600) {  // it ended over an hour ago (Dotty was off): forgotten quietly
    LOGI("moments", "timer \"%s\" ended long ago: dropped", t.label.c_str());
    moments::dropRecording(t.companion, "timer");
    t.cancel();
    moments::saveTimer();
    return;
  }
  const int left = t.secondsLeft(now);
  if (left == 0 && lastTimerLeft == 0) {  // its end waited for a reply or a press: now?
    if (timerCue(0, t.tickEvery, momentBusy()).sound == TimerSound::End) {
      lastTimerLeft = -1;
      return startTimerEnd();
    }
    return;
  }
  if (left != lastTimerLeft) {
    const bool newSecond = lastTimerLeft < 0 || left < lastTimerLeft;
    lastTimerLeft = left;
    if (newSecond) {
      const TimerCue cue = timerCue(left, t.tickEvery, momentBusy());
      if (cue.sound == TimerSound::End) {
        lastTimerLeft = -1;
        return startTimerEnd();
      }
      if (cue.sound != TimerSound::None && !shell::locked()) {
        const AudioPlayer::Note n = {cue.beep.midi, cue.beep.ms};
        player.setVolume(cue.beep.volume);
        player.playNotes(&n, 1);
        timerVolume = true;
      }
    }
    const int step = left / timerRefreshSeconds(left);
    if (step != lastTimerStep && timerShowing()) {
      lastTimerStep = step;
      redraw = true;
    }
  }
  if (left <= 60) {  // the last minute: Dotty unlocks and shows the countdown (once: back closes it)
    shell::wake();
    if (!timerAutoOpened) {
      timerAutoOpened = timerOpen = true;
      redraw = true;
    }
  }
}

void timerTap(uint16_t x, uint16_t y) {
  TimerState &t = moments::timer();
  if (screens::timerHit(x, y) == TimerHit::Back) {
    timerOpen = timerConfirm = false;
    redraw = true;
    return;
  }
  if (timerConfirm) {
    const TimerHit hit = screens::timerHit(x, y);
    if (hit == TimerHit::None) return;
    timerConfirm = false;
    if (hit == TimerHit::Yes) {
      LOGI("moments", "timer cancelled on Dotty");
      moments::dropRecording(t.companion, "timer");
      t.cancel();
      moments::saveTimer();
      notifyTimer();
    }
  } else if (t.paused) {
    t.resume(time(nullptr));
    moments::saveTimer();
    notifyTimer();
  } else {
    t.pause(time(nullptr));
    moments::saveTimer();
    notifyTimer();
  }
  redraw = true;
}

// Every pass (locked too): missed ones, due ones. Once a second is plenty.
void pollMoments() {
  if (firing.active) return pollFiring();
  if (!moments::available() || millis() - lastMomentPoll < 1000) return;
  lastMomentPoll = millis();
  const int64_t now = time(nullptr);
  const std::vector<CompanionMoments> all = moments::all();
  Due d = due(all, now, momentBusy());
  for (const auto &miss : d.missed) {
    MomentList *list = moments::of(miss.first);
    Moment *m = list ? list->find(miss.second) : nullptr;
    if (!m) continue;
    LOGI("moments", "missed %s's %s %d (%s): %s", miss.first.c_str(), kindName(m->kind), m->id, localTimeText(m->at).c_str(),
         m->what.c_str());
    if (m->kind == MomentKind::Alarm) missedAlarmLine = String("Missed alarm ") + localTimeText(m->at).substr(11).c_str();
    if (miss.first == companions::active().id) history::append("", "(missed) " + m->what, "neutral", 0, 0, std::string("moment: ") + kindName(m->kind));
    advance(*m, now, true);
    if (m->state != MomentState::Upcoming) moments::dropRecording(miss.first, std::to_string(m->id));
    moments::save(miss.first);
    notifyMoments();
  }
  if (d.fire.empty()) return;
  firingQueue.assign(d.fire.begin() + 1, d.fire.end());
  startFiring(d.fire.front(), true);
}

// Locked: when to wake next (the shell sleeps until the next minute anyway).
void armLockedWake() {
  int64_t next = nextDue(moments::all());
  const TimerState &t = moments::timer();
  if (t.running && !t.paused && (!next || t.endsAt - 60 < next)) next = t.endsAt - 60;  // the last minute
  if (jobsDue()) {  // the next phone wake
    loadWakeTimes();
    const int64_t wake = phoneWake.nextWakeAt(lastWake, moments::rules().wakeEvery, true);
    if (!next || wake < next) next = wake;
  }
  shell::wakeLockedAt(static_cast<time_t>(next));
}

// ---------- the last thing said: kept per companion, replayed (play / pause / loop) ----------

String lastPath(const std::string &id, const char *ext) {
  return companions::folder(id) + "/last." + ext;
}

// A new reply: keep its audio (two minutes at most) and its text pieces.
void beginLast() {
  lastPending = LastLine();
  lastRec.reset(kLastMaxFrames);
  recording = !settings.textOnly;
}

void freeLast() {
  lastRec.reset(0);
  recording = false;
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

// A heard reply on its way to the card (a task writes it: two minutes are ~6 MB of WAV).
struct LastSave {
  AdpcmRecorder audio;
  LastLine line;
  std::string signature;  // a moment's line: what it was recorded for (moments::recorded)
  std::string companion;
  String wav, json;
  uint32_t startedAt = 0;
  volatile bool done = false;
  bool ok = false;
};
LastSave *lastSave = nullptr;

void lastSaveTask(void *arg) {
  LastSave *job = static_cast<LastSave *>(arg);
  uint8_t header[44];
  wavHeader(header, job->audio.samples() * 2, 24000);
  File f = SD_MMC.open(job->wav + ".part", FILE_WRITE);
  bool ok = static_cast<bool>(f) && f.write(header, sizeof(header)) == sizeof(header);
  job->audio.decode([&](const int16_t *pcm, size_t n) {
    if (ok) ok = f.write(reinterpret_cast<const uint8_t *>(pcm), n * 2) == n * 2;
  });
  f.close();
  if (ok) {
    SD_MMC.remove(job->wav);
    ok = SD_MMC.rename(job->wav + ".part", job->wav);
  }
  std::string json = lastLineJson(job->line);
  if (!job->signature.empty()) {  // a moment's recording: what it was made for
    JsonDocument doc;
    deserializeJson(doc, json);
    doc["signature"] = job->signature;
    json.clear();
    serializeJson(doc, json);
  }
  ok = ok && writeCard(job->json, reinterpret_cast<const uint8_t *>(json.data()), json.size(), nullptr, 0);
  job->ok = ok;
  job->done = true;  // the loop takes it from here (lastSavePoll)
  vTaskDelete(nullptr);
}

// The reply has been heard: onto the card, as the companion's last line.
void saveLast() {
  if (!recording || settings.textOnly || lastRec.samples() < 24000 / 2 || lastSave) return freeLast();  // nothing to keep (or one still saving)
  lastPending.ms = UINT32_MAX;
  lastPending.cut(lastRec.samples() / 24);
  lastPending.mood = turnMood.c_str();
  lastPending.t = time(nullptr);
  LastSave *job = new LastSave();
  job->audio = std::move(lastRec);
  job->line = lastPending;
  job->companion = sessionCompanion;
  job->wav = lastPath(sessionCompanion, "wav");
  job->json = lastPath(sessionCompanion, "json");
  job->startedAt = millis();
  lastRec = AdpcmRecorder();
  recording = false;
  lastSave = job;
  if (xTaskCreatePinnedToCore(lastSaveTask, "last-save", 8192, job, 1, nullptr, 0) != pdPASS) {
    LOGW("companion", "last line NOT saved: no task");
    delete job;
    lastSave = nullptr;
  }
}

void lastSavePoll() {
  if (!lastSave || !lastSave->done) return;
  LOGI("companion", "last line %s: %.1f s in %lu ms", lastSave->ok ? "saved" : "NOT saved", lastSave->audio.samples() / 24000.0f,
       static_cast<unsigned long>(millis() - lastSave->startedAt));
  if (lastSave->ok && lastSave->companion == companions::active().id) {
    lastLine = lastSave->line;
    hasLast = true;
    redraw = true;
  }
  delete lastSave;
  lastSave = nullptr;
}

bool canReplay() {
  return hasLast && !settings.textOnly && !firing.active &&
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

// ---------- recording moments' lines (line_job.h): written, read silently, saved ----------
// The earliest-due moment (or group, or the timer) without its recording; in a chat, only the chat's
// companion's (its session reads it). A recording is made for a signature (what, style): an edit
// records again.

LastSave *lineSave = nullptr;
const char *lineNote = "Saving your reminder...";

bool nextLineWork(LineWork &out, const std::string &onlyCompanion) {
  const int64_t now = time(nullptr);
  bool found = false;
  auto consider = [&](const LineWork &w) {
    const auto back = lineBackoff.find(w.companion + "/" + w.key);
    if (back != lineBackoff.end() && static_cast<int32_t>(back->second - millis()) > 0) return;  // failed lately
    if (!found || w.at < out.at) {
      out = w;
      found = true;
    }
  };
  TimerState &timer = moments::timer();
  if (timer.running && !timer.paused && !timer.ready && timer.secondsLeft(now) > 20 &&
      (onlyCompanion.empty() || timer.companion == onlyCompanion)) {
    LineWork w;
    w.companion = timer.companion;
    w.key = "timer";
    const std::string up = timer.label.empty() ? "their timer ran out" : "their " + timer.label + " timer ran out";
    w.style = timer.style.empty() ? "excited" : timer.style;
    // What they asked for at its end ("sing happy birthday"), else just that it's up — without it the
    // writer leant on memories and every timer was "the eggs".
    // Nothing asked for: the companion makes the ending its own (the user, 2026-10-24) — creative, funny,
    // in character, still plainly "time's up".
    w.topics.push_back({"timer", timer.what.empty() ? up + "; nothing in particular was asked for, so make it your own: "
                                                         "creative and funny, true to your character and quirks, a "
                                                         "sentence or two that still clearly says time's up"
                                                    : up + "; as asked when it was set: " + timer.what,
                        w.style});
    w.at = timer.endsAt;
    w.words = timer.what.empty() ? 60 : 150;
    w.signature = timer.signature();
    consider(w);
  }
  for (const CompanionMoments &c : moments::all()) {
    if (!onlyCompanion.empty() && c.companion != onlyCompanion) continue;
    MomentList &list = *c.list;
    bool changed = false;
    std::vector<int> grouped;
    for (const std::vector<int> &ids : sharedMinutes(list)) {
      std::vector<const Moment *> members;
      LineWork w;
      w.companion = c.companion;
      for (const int id : ids) {
        const Moment *m = list.find(id);
        members.push_back(m);
        w.topics.push_back({kindName(m->kind), m->what, m->style});
        if (m->repeat == Repeat::Once) grouped.push_back(id);
      }
      w.key = "group-" + std::to_string(members[0]->at / 60);
      w.ids = ids;
      w.at = members[0]->at;
      w.words = groupWords(ids.size());
      w.style = members[0]->style;
      w.signature = groupSignature(members);
      const bool ready = moments::recorded(c.companion, w.key, w.signature);
      for (const int id : ids) {  // a one-off in a group is ready when the group is
        Moment *m = list.find(id);
        if (m->repeat == Repeat::Once && m->ready != ready) {
          m->ready = ready;
          changed = true;
        }
      }
      if (!ready) consider(w);
    }
    for (const Moment &m : list.all()) {
      if (m.state != MomentState::Upcoming) continue;
      if (std::find(grouped.begin(), grouped.end(), m.id) != grouped.end()) continue;
      LineWork w;
      w.companion = c.companion;
      w.key = std::to_string(m.id);
      w.ids = {m.id};
      w.topics.push_back({kindName(m.kind), m.what, m.style});
      w.at = m.at;
      w.words = 150;
      w.style = m.style;
      w.signature = groupSignature({&m});
      const bool ready = moments::recorded(c.companion, w.key, w.signature);
      if (m.ready != ready) {
        list.find(m.id)->ready = ready;
        changed = true;
      }
      if (!ready) consider(w);
    }
    if (changed) moments::save(c.companion);
  }
  return found;
}

void abortLine(const char *why) {
  if (!lineJob.busy()) return;
  LOGI("line", "stopped (%s)", why);
  if (lineJob.step() == LineJob::Step::Writing) jobs.cancel();
  lineJob.abort();
  capturingLine = false;
  lineRec.reset(0);
  lineCooldownUntil = millis() + 60000;
}

void lineSaveTask(void *arg) {
  lastSaveTask(arg);  // the replay's: WAV from the ADPCM, then the timing file (with its signature)
}

// The reading has arrived: onto the card (a task: a minute of WAV is ~3 MB).
void saveLine() {
  capturingLine = false;
  if (lineSave || lineRec.samples() < 24000 / 2) {
    LOGW("line", "nothing to keep (%u frames)", static_cast<unsigned>(lineRec.samples()));
    lineRec.reset(0);
    return lineJob.saved(false);
  }
  linePending.ms = UINT32_MAX;
  linePending.cut(lineRec.samples() / 24);
  linePending.t = time(nullptr);
  LastSave *job = new LastSave();
  job->audio = std::move(lineRec);
  job->line = linePending;
  job->signature = lineJob.work().signature;
  job->companion = lineJob.work().companion;
  job->wav = moments::recordingPath(lineJob.work().companion, lineJob.work().key, "wav");
  job->json = moments::recordingPath(lineJob.work().companion, lineJob.work().key, "json");
  job->startedAt = millis();
  lineRec = AdpcmRecorder();
  lineSave = job;
  if (xTaskCreatePinnedToCore(lineSaveTask, "line-save", 8192, job, 1, nullptr, 0) != pdPASS) {
    delete job;
    lineSave = nullptr;
    lineJob.saved(false);
  }
}

void lineSaved() {
  if (!lineSave || !lineSave->done) return;
  const bool ok = lineSave->ok;
  LOGI("line", "%s %s: %.1f s in %lu ms", ok ? "recorded" : "NOT recorded", lineJob.work().key.c_str(),
       lineSave->audio.samples() / 24000.0f, static_cast<unsigned long>(millis() - lineSave->startedAt));
  delete lineSave;
  lineSave = nullptr;
  lineJob.saved(ok);
  if (!ok) return;
  const LineWork &w = lineJob.work();
  if (w.key == "timer") {
    moments::timer().line = lineJob.text();
    moments::timer().ready = true;
    moments::saveTimer();
  } else if (MomentList *list = moments::of(w.companion)) {
    for (const int id : w.ids)
      if (Moment *m = list->find(id)) {
        if (w.ids.size() == 1) m->line = lineJob.text();
        m->ready = true;
      }
    moments::save(w.companion);
  }
  notifyMoments();
}

// In a chat: once the reply has ended, the chat's companion's lines are written and read on its session.
void pollLines() {
  lineSaved();
  if (lineJob.step() == LineJob::Step::Done || lineJob.step() == LineJob::Step::Failed) {
    if (lineJob.step() == LineJob::Step::Failed) {
      lineCooldownUntil = millis() + 60000;
      lineBackoff[lineJob.work().companion + "/" + lineJob.work().key] = millis() + 30 * 60 * 1000;  // not every wake
      if (bgSession) bgFailures++;
    }
    lineJob.reset();
    capturingLine = false;
  }
  // Room for a second TLS connection (the writer): in a Wi-Fi chat only once Bluetooth is off (Wi-Fi +
  // Bluetooth + Live left 7 KB once and Wi-Fi couldn't start again), and never in a tight heap.
  const bool roomForWriter = (live.overRelay() || !ble::running()) &&
                             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= 24 * 1024 &&
                             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= 40 * 1024;
  const bool idleChat = (bgSession ? live.ready() && !live.inTurn()
                                   : live.ready() && !connecting && (info.state == Talk::After || info.state == Talk::Ready) &&
                                         !writing && !reading && !reviewStart && !live.inTurn() && !voice::playing()) &&
                        !firing.active;
  if (lineJob.busy()) {
    lastTurnAt = millis();  // the chat stays open (its review waits) until the line is done
    info.note = lineNote;
    noteUntil = millis() + 3000;
  }
  switch (lineJob.step()) {
    case LineJob::Step::Idle: {
      if (!idleChat || !roomForWriter || jobs.busy() || planRunning || static_cast<int32_t>(millis() - lineCooldownUntil) < 0)
        return;
      if (millis() - lastLineScan < 3000) return;
      lastLineScan = millis();
      LineWork w;
      if (!nextLineWork(w, sessionCompanion)) return;
      const Profile *who = companions::find(w.companion);
      if (!who) return;
      lineNote = w.key == "timer" ? "Saving your timer..." : "Saving your reminder...";
      for (const Topic &t : w.topics)
        if (t.kind == "alarm") lineNote = "Saving your alarm...";
      lineJob.start(w);
      if (w.ids.size() == 1 && w.key != "timer") {  // the companion's own lines come written (the plan)
        const MomentList *list = moments::of(w.companion);
        const Moment *m = list ? list->find(w.ids[0]) : nullptr;
        if (m && m->kind == MomentKind::Spontaneous && !m->line.empty()) {
          Written given;
          given.ok = true;
          given.text = m->line;
          lineJob.written(given);
          LOGI("line", "%s's %s: written by the plan", w.companion.c_str(), w.key.c_str());
          return;
        }
      }
      writerBody = lineRequest(*who, w.companion == sessionCompanion ? memorySection : std::string(), w.topics, w.at, w.words);
      jobKind = Job::Line;
      LOGI("line", "writing %s's %s (%u topic(s), %d words at most)", w.companion.c_str(), w.key.c_str(),
           static_cast<unsigned>(w.topics.size()), w.words);
      launchJob();
      redraw = true;
      return;
    }
    case LineJob::Step::Writing: {
      Written w;
      if (jobs.take(w)) {
        if (w.tokens) usage::addTokens(w.tokens, sessionKey);
        LOGI("line", "%s (%u characters)", w.ok ? "written" : w.error.c_str(), static_cast<unsigned>(w.text.size()));
        lineJob.written(w);
      }
      return;
    }
    case LineJob::Step::WaitSession:
      if (idleChat && sessionCompanion == lineJob.work().companion) {
        lineJob.sessionReady();
        lineRec.reset(kLastMaxFrames);
        linePending = LastLine();
        lineFrames = 0;
        lineReadingAt = millis();
      }
      return;
    case LineJob::Step::Reading: {
      if (millis() - lineReadingAt > readingCapMs(lineJob.work().words)) {
        lineDiscard = live.inTurn();  // its turn may still end: swallowed, not taken for a chat's reply
        return abortLine("the reading took too long");
      }
      std::string note;
      if (lineJob.takeSend(note)) {
        capturingLine = true;
        live.sendText(note, lineJob.text().size() && lineFrames > 0);
      }
      return;
    }
    case LineJob::Step::Saving:
      if (capturingLine) saveLine();
      return;
    default:
      return;
  }
}

// ---------- the phone wake (phone_wake.h) and the daily plan ----------
// While locked, every wakeEvery minutes when a job is due (a line to record, the day's plan), Bluetooth
// comes on for the phone; the jobs run through it, or over Wi-Fi when it doesn't come. Unlocked, they
// run whenever the phone is there and Dotty is idle (an app-made moment records at once).

void loadWakeTimes() {
  if (lastWake >= 0) return;
  Preferences p;
  p.begin("companion", true);
  lastWake = p.getLong64("lastWake", 0);
  plannedDay = p.getLong64("plannedDay", 0);
  p.end();
}

void saveWakeTimes() {
  Preferences p;
  p.begin("companion", false);
  p.putLong64("lastWake", lastWake);
  p.putLong64("plannedDay", plannedDay);
  p.end();
}

bool planIsDue() {
  loadWakeTimes();
  if (!moments::available() || !companions::active().awake()) return false;
  return forcePlan || planDue(time(nullptr), plannedDay, moments::rules());
}

bool canRecord() {
  return !settings.keys.empty() && !usage::outUntilUtc();
}

// Is there anything for a wake to do? Read from the card at most every 30 s.
bool jobsDue() {
  static uint32_t checkedAt = 0;
  static bool due = false;
  if (checkedAt && millis() - checkedAt < 30000) return due;
  checkedAt = millis();
  LineWork w;
  due = (canRecord() && (planIsDue() || nextLineWork(w, ""))) || syncWanted();
  return due;
}

// A session only for recording: that companion's voice and character, no chat.
bool startBackgroundSession(const std::string &companion, bool viaPhone) {
  const Profile *p = companions::find(companion);
  const int key = ring.pick(nowUtc());
  if (!p || key < 0) return false;
  sessionKey = key;
  sessionKeyText = settings.keys[key];
  sessionVoice = p->voice.c_str();
  sessionCompanion = companion;
  personaSection = personaText(*p);
  memorySection = companion == companions::active().id ? memories::store().promptSection() : std::string();
  live.forgetResume();  // its own session: a chat's kept conversation isn't carried into it (nor back)
  bgSession = true;
  awaitingConnect = true;
  bgOpenedAt = millis();
  power::setWakeLock(power::kWakeLockNetwork, true);
  LOGI("wake", "recording session for %s %s", companion.c_str(), viaPhone ? "through the iPhone" : "over Wi-Fi");
  if (viaPhone) {
    relayOpenAt = millis();
    if (live.begin(sessionKeyText.c_str(), sessionConfig(), callbacks(), relayTransport)) return true;
    disconnect("the phone didn't take it");
    return false;
  }
  wifiBegan = false;
  cancelConnect = false;
  connecting = true;
  connectError = "";
  if (xTaskCreatePinnedToCore(connectLoop, "companion-connect", 12288, nullptr, 2, &connectTask, 1) != pdPASS) {
    connecting = false;
    disconnect("no task");
    return false;
  }
  return true;
}

// The day's spontaneous lines: written by the text model (through the phone or Wi-Fi), checked, added.
void pollPlan() {
  if (planRunning) {
    Written w;  // parsePlan reads the raw answer: the job hands it over as the text
    if (!jobs.take(w)) return;
    planRunning = false;
    forcePlan = false;
    const int64_t now = time(nullptr), day = now - now % 86400;
    std::vector<int64_t> taken;
    for (const CompanionMoments &c : moments::all())
      for (const Moment &m : c.list->all())
        if (m.state == MomentState::Upcoming && m.at - m.at % 86400 == day) taken.push_back(m.at);
    JsonDocument body;  // parsePlan wants generateContent's answer
    body["candidates"][0]["content"]["parts"][0]["text"] = w.text;
    std::string raw;
    serializeJson(body, raw);
    const std::vector<PlannedLine> plan = parsePlan(w.ok ? 200 : 500, raw, day, moments::rules(), taken, moments::rules().perDay);
    MomentList *list = moments::of(companions::active().id);
    int added = 0;
    for (const PlannedLine &l : plan) {
      if (!list || l.at <= now) continue;
      Moment m;
      m.kind = MomentKind::Spontaneous;
      m.at = l.at;
      m.what = l.what;
      m.line = l.line;
      m.style = l.style;
      m.source = "companion";
      m.created = now;
      if (list->add(m) > 0) added++;
    }
    if (added) moments::save(companions::active().id);
    plannedDay = day;  // once a day, even when nothing came back
    saveWakeTimes();
    lineScanStale = true;  // the plan's new lines are work for this same wake
    LOGI("wake", "today's plan: %d line(s) (%s)", added, w.ok ? "planned" : w.error.c_str());
    notifyMoments();
    return;
  }
  if (!planIsDue() || jobs.busy() || lineJob.busy()) return;
  const bool through = relay::phoneReady() && (!live.connected() || live.overRelay());
  if (!through && !(bgSession && live.ready())) return;  // over Wi-Fi it needs the session's connection
  const Profile &who = companions::active();
  std::string sums = companions::readText(companions::folder(who.id) + "/summaries.jsonl");
  const int64_t now = time(nullptr), day = now - now % 86400;
  std::vector<int64_t> taken;
  for (const CompanionMoments &c : moments::all())
    for (const Moment &m : c.list->all())
      if (m.state == MomentState::Upcoming && m.at - m.at % 86400 == day) taken.push_back(m.at);
  writerBody = planRequest(who, memories::store().promptSection(), summariesSection(sums, 10), day, moments::rules(),
                           moments::rules().perDay, taken);
  jobKind = Job::Plan;
  planRunning = true;
  LOGI("wake", "planning today's lines (%d)", moments::rules().perDay);
  launchJob();
}

void endPhoneWake(const char *how) {
  if (bgSession || live.connected() || connecting) disconnect("phone wake done");
  if (!connecting) {  // whatever happened, nothing of the wake may keep Dotty awake
    net::disconnect();
    power::setWakeLock(power::kWakeLockNetwork, false);
  }
  shell::endBluetoothWindow();
  LOGI("wake", "phone wake: done in %lu s via %s", static_cast<unsigned long>((millis() - bgOpenedAt) / 1000), how);
  phoneWake.end();
  bgSession = false;
  planRunning = false;
}

void pollPhoneWake() {
  const int64_t now = time(nullptr);
  const bool chatting = !bgSession && (live.connected() || connecting || info.state == Talk::Listening);
  if (phoneWake.stage() == PhoneWake::Stage::Idle) {
    if (chatting || firing.active || !moments::available() || !jobsDue()) return;
    loadWakeTimes();
    if (shell::locked()) {
      if (now < phoneWake.nextWakeAt(lastWake, moments::rules().wakeEvery, true)) return;
      shell::bluetoothWindow(40);
    } else if (!relay::phoneReady() || (info.state != Talk::Offline && info.state != Talk::Error) || now - lastWake < 120) {
      return;  // unlocked: only with the phone there and nothing else going on (and not over and over)
    }
    wakeVia = "nothing";
    phoneWake.start(millis());
    bgFailures = 0;
    for (const std::string &id : companions::list().order())  // each recording asked about ≤ 3 wakes a boot
      for (const RecordingMeta &m : note::list(id))
        if (recordingNeedsSync(m)) syncTries[m.name]++;
    bgOpenedAt = millis();
    lastWake = now;
    saveWakeTimes();
    LOGI("wake", "phone wake: %s%s", planIsDue() ? "plan " : "", "record");
    return;
  }
  if (chatting) {  // the user wants to talk: the wake gives way
    phoneWake.end();
    shell::endBluetoothWindow();
    return;
  }
  const bool phone = relay::phoneReady();
  // What's left, read from the card every few seconds (not every pass: each moment's recording is checked).
  static LineWork work;
  static bool lineLeft = false;
  static uint32_t scannedAt = 0;
  static LineJob::Step scannedStep = LineJob::Step::Idle;
  if (!scannedAt || lineScanStale || millis() - scannedAt > 3000 || lineJob.step() != scannedStep) {
    lineScanStale = false;
    scannedAt = millis();
    scannedStep = lineJob.step();
    lineLeft = nextLineWork(work, "");
  }
  // Recordings the phone lacks: it's told once it's there (companion.recordings), then fetches and
  // transcribes them; done when nothing is left or it has been quiet for 45 s.
  static bool syncAsked = false;
  if (phoneWake.stage() != PhoneWake::Stage::Phone) syncAsked = false;
  bool syncLeft = false;
  if (phone && phoneWake.stage() == PhoneWake::Stage::Phone && syncWanted()) {
    if (!syncAsked) {
      syncAsked = true;
      lastSyncActivity = millis();
      notifyRecordings();
      LOGI("wake", "asking the phone to sync recordings");
    }
    syncLeft = noteSend.active || millis() - lastSyncActivity < 45000;
  }
  const bool done = PhoneWake::jobsDone(planIsDue(), planRunning, lineJob.busy(), lineLeft, bgFailures) && !syncLeft;
  const PhoneWake::Stage stage = phoneWake.poll(millis(), phone, true, done);
  if (stage == PhoneWake::Stage::Done) return endPhoneWake(wakeVia);
  if (stage == PhoneWake::Stage::Phone && shell::locked()) shell::bluetoothWindow(30);  // kept open while it works
  if (stage == PhoneWake::Stage::Wifi && shell::bluetoothWindowOpen()) shell::endBluetoothWindow();
  // A session that didn't open, or a phone that didn't answer its setup in 10 s.
  if (bgSession && awaitingConnect && !connecting && !live.connected()) {
    LOGW("wake", "the recording session didn't open: %s", connectError.c_str());
    disconnect("recording session didn't open");  // Wi-Fi off, the Network wake lock released
    bgFailures++;
  }
  // An open recording session that died quietly (the iPhone suspended, a 1006 while idle).
  if (bgSession && !awaitingConnect && !connecting && !live.connected()) {
    LOGW("wake", "the recording session closed");
    disconnect("recording session closed");
    bgFailures++;
  }
  if (bgSession && awaitingConnect && live.overRelay() && !live.ready() && millis() - relayOpenAt > 10000) {
    disconnect("the phone didn't answer");
    bgFailures++;
  }
  if (bgSession && live.ready()) awaitingConnect = false;
  if (stage == PhoneWake::Stage::Phone) wakeVia = "phone";
  if (stage == PhoneWake::Stage::Wifi) wakeVia = "wifi";
  pollPlan();
  if (planRunning || lineJob.busy() || bgFailures >= 2) return;
  if (stage != PhoneWake::Stage::Phone && stage != PhoneWake::Stage::Wifi) return;
  // Over Wi-Fi even the plan needs a session (its connection); through the phone it goes first, alone.
  const bool planOverWifi = planIsDue() && stage == PhoneWake::Stage::Wifi;
  if (planIsDue() && !planOverWifi) return;
  const std::string want = planOverWifi ? companions::active().id : lineLeft ? work.companion : std::string();
  if (want.empty()) return;
  if (bgSession && live.ready() && want != sessionCompanion) disconnect("next companion");  // its own voice
  if (!bgSession && !connecting) startBackgroundSession(want, stage == PhoneWake::Stage::Phone);
}

// Locked: what the loop does while a wake runs (the chat's own polling only runs unlocked).
void backgroundPoll() {
  if (!connecting || live.overRelay()) live.poll();
  if (!(live.overRelay() && live.connected())) relay::poll();
  if (cancelConnect && !connecting) {
    cancelConnect = false;
    power::setWakeLock(power::kWakeLockNetwork, false);
  }
  writerPoll();  // collects a fetch through the phone
  pollLines();
  pollNoteSend();
  pollPhoneWake();
}

// ---------- voice notes: the deck, playing one, the reaction (voice_note.h, note_deck.h) ----------

String clockText(uint32_t ms) {
  char buf[12];
  const uint32_t s = ms / 1000;
  snprintf(buf, sizeof(buf), "%lu:%02lu", static_cast<unsigned long>(s / 60), static_cast<unsigned long>(s % 60));
  return buf;
}

DeckView deckView() {
  DeckView v;
  v.title = note::current().title.c_str();
  v.clock = clockText(deck.elapsedMs(millis()));
  v.recording = deck.state() == NoteDeck::State::Recording;
  v.paused = deck.state() == NoteDeck::State::Paused;
  v.confirm = deck.state() == NoteDeck::State::ConfirmDiscard;
  v.empty = deck.parts() == 0 && !v.recording;
  v.levelDb = voice::levelDb();
  return v;
}

void openDeck(const std::string &title) {
  if (noteSend.active) {  // a fetch on the same channel: stopped, the phone asks again later
    noteSend.file.close();
    noteSend.active = false;
    ble::notify(recEndEvent(noteSend.name, false));
    LOGI("note", "sending %s stopped: a new recording", noteSend.name.c_str());
  }
  if (!note::begin(sessionCompanion.empty() ? companions::active().id : sessionCompanion, title)) {
    LOGW("note", "the deck didn't open (no card?)");
    return;
  }
  deck.open(millis());
  deckOpen = true;
  redraw = true;
}

// A typed turn (the user doesn't see the text; `label` is what the history and the screen show).
void typedTurn(const std::string &text, const std::string &label) {
  stopReplay();
  beginLast();
  cancelWriter();
  said.clear();
  info.said = "";
  turnMood = info.mood = "neutral";
  firstAudioAt = 0;
  turnMemory.clear();
  textQueue.clear();
  replyFrames = 0;
  heard = label;
  info.heard = heard.c_str();
  live.sendText(text);
  releasedAt = millis();
  setState(Talk::Thinking);
}

// Sent once a session is ready (one is opened for it when there's none).
void queueTypedTurn(const std::string &text, const std::string &label) {
  pendingTypedText = text;
  pendingTypedLabel = label;
  pendingTypedSince = millis();
}

void pollTypedTurn() {
  if (pendingTypedText.empty()) return;
  if (live.ready() && (info.state == Talk::Ready || info.state == Talk::After || info.state == Talk::Offline)) {
    typedTurn(pendingTypedText, pendingTypedLabel);
    pendingTypedText.clear();
  } else if (!live.connected() && !connecting && !awaitingConnect && info.state != Talk::Error) {
    connect();
  } else if (millis() - pendingTypedSince > 30000) {
    LOGW("note", "no session for the reaction: dropped");
    pendingTypedText.clear();
  }
}

void finishDeck(bool keep) {
  size_t frames = 0;
  while (voice::nextChunk(noteChunk, frames)) note::add(noteChunk, frames);
  voice::stopListening();
  const bool streamed = note::streaming();
  const RecordingMeta m = note::current();
  const std::string name = note::finish(keep);
  deckOpen = false;
  lastTurnAt = millis();
  setState(live.connected() ? Talk::Ready : Talk::Offline, idleStatus());
  if (name.empty()) return;
  if (!streamed) notifyRecordings();  // the phone streamed it: telling it now made it fetch the file again
  reaction = NoteReaction();
  reaction.waiting = true;
  reaction.companion = sessionCompanion.empty() ? companions::active().id : sessionCompanion;
  reaction.name = name;
  reaction.title = m.title;
  reaction.ms = note::current().ms;
  reaction.until = millis() + (streamed ? 10000 : 0);  // the phone's draft is due within seconds
}

void pollReaction() {
  if (!reaction.waiting || (!reaction.gotText && static_cast<int32_t>(millis() - reaction.until) < 0)) return;
  reaction.waiting = false;
  if (reaction.companion != companions::active().id) return;  // switched meanwhile
  const std::string what = "'" + reaction.title + "' (" + clockText(reaction.ms).c_str() + ")";
  const std::string text =
      reaction.gotText
          ? "(Dotty, not the user:) They just recorded a voice note with you, " + what + ". What they said: " +
                reaction.transcript +
                "\n\nSay briefly that it's saved and react to what they said, in character. Save lasting facts about "
                "them with remember."
          : "(Dotty, not the user:) They just recorded a voice note with you, " + what +
                ". It's saved, but you can't hear it yet: say briefly that it's kept and that you'll listen to it "
                "properly once their phone is near.";
  LOGI("note", "reaction to %s (%s)", reaction.name.c_str(), reaction.gotText ? "with its text" : "no text yet");
  queueTypedTurn(text, "(recording) " + reaction.title + " - " + clockText(reaction.ms).c_str());
}

// held: BOOT down ≥ kHoldMs (a part starts on a real hold); up: released for 40 ms (no bounce ends it).
void pollDeck(bool held, bool up) {
  shell::wake();  // the deck keeps Dotty unlocked
  lastTurnAt = millis();  // and the chat open (no review meanwhile)
  const bool recording = deck.state() == NoteDeck::State::Recording;
  NoteDeck::Event e = deck.hold(recording ? !up : held, millis());
  if (e == NoteDeck::Event::PartStart) {
    voice::startListening();
    note::partStart();
    redraw = true;
  } else if (e == NoteDeck::Event::PartEnd || e == NoteDeck::Event::PartDropped) {
    size_t frames = 0;
    while (voice::nextChunk(noteChunk, frames)) note::add(noteChunk, frames);
    voice::stopListening();
    note::partEnd();
    if (e == NoteDeck::Event::PartDropped) note::undo();  // a bounce, not a part: off the card and the phone
    redraw = true;
  }
  if (deck.state() == NoteDeck::State::Recording) {
    size_t frames = 0;
    while (voice::nextChunk(noteChunk, frames)) note::add(noteChunk, frames);
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw > 1000) {
      lastDraw = millis();
      redraw = true;
    }
  }
  e = deck.tick(millis());
  if (e == NoteDeck::Event::Warn) {
    static const AudioPlayer::Note kWarn[] = {{84, 150}, {0, 100}, {84, 150}};
    player.playNotes(kWarn, 3);
  }
  if (e == NoteDeck::Event::Done) finishDeck(true);
  if (e == NoteDeck::Event::CloseEmpty || e == NoteDeck::Event::Discard) finishDeck(false);
}

void deckTap(uint16_t x, uint16_t y) {
  const DeckHit hit = screens::deckHit(x, y, deckView());
  NoteDeck::Event e = NoteDeck::Event::None;
  switch (hit) {
    case DeckHit::Back: e = deck.cancel(); break;
    case DeckHit::Undo:
      if (deck.undo() == NoteDeck::Event::Undo) LOGI("note", "undo: %lu ms off", static_cast<unsigned long>(note::undo()));
      break;
    case DeckHit::Done: e = deck.done(); break;
    case DeckHit::Discard: e = deck.discard(false); break;
    case DeckHit::Yes: e = deck.discard(true); break;
    case DeckHit::No: deck.keep(); break;
    default: break;
  }
  if (e == NoteDeck::Event::Done) finishDeck(true);
  if (e == NoteDeck::Event::Discard || e == NoteDeck::Event::CloseEmpty) finishDeck(false);
  redraw = true;
}

// play_recording: after the reply that announced it.
void pollNotePlay() {
  if (notePlaying && !player.isPlaying()) {
    notePlaying = false;
    info.momentIcon = "";
    redraw = true;
  }
  if (pendingPlayName.empty() || info.state == Talk::Thinking || info.state == Talk::Speaking || voice::playing() ||
      live.inTurn())
    return;
  const String wav = note::path(pendingPlayCompanion, pendingPlayName, "wav");
  if (player.play(wav.c_str())) {
    notePlaying = true;
    info.momentIcon = "speech";
    info.said = (std::string("Playing: ") + pendingPlayTitle).c_str();
    info.page = -1;
    if (info.state != Talk::After) setState(Talk::After);
    lastTurnAt = millis();
    LOGI("note", "playing %s", pendingPlayName.c_str());
  }
  pendingPlayName.clear();
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
  if (deckOpen) {
    screens::drawDeck(deckView());
  } else if (settingsOpen) {
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
    const TimerState &t = moments::timer();
    const bool timer = timerShowing();
    info.timerLabel = timer ? t.title().c_str() : "";
    info.timerClock = timer ? t.clock(time(nullptr)).c_str() : "";
    info.timerChip = t.running && !timer ? t.clock(time(nullptr)).c_str() : "";
    info.timerPaused = t.paused;
    info.timerConfirm = timerConfirm;
    screens::draw(info);
  }
}

void onTap(uint16_t x, uint16_t y) {
  if (deckOpen) return deckTap(x, y);
  if (notePlaying) {  // a tap stops a voice note playing
    player.stop();
    return;
  }
  if (firing.active) return tapFiring();  // snooze an alarm, stop anything else
  if (timerShowing()) return timerTap(x, y);
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
  } else if (y < 28 && x > 130 && moments::timer().running) {  // the timer chip
    timerOpen = true;
    redraw = true;
  } else if ((y < 40 || (moments::timer().running && y < 60)) && x > 150) {
    historyOpen = true;
    historyScroll = 0;
    redraw = true;
  } else if (info.state == Talk::Error || info.state == Talk::Offline) {
    connect();
  }
}

// What a chat left to do later doesn't outlive it (a deck days later, a note playing at the next unlock).
void dropPending() {
  deckPending = false;
  pendingPlayName.clear();
  reaction.waiting = false;
  pendingTypedText.clear();
}

void onLock() {
  dropPending();
  // Locking turns Bluetooth off at once: a relay chat is over even though nothing reported it yet.
  if (live.overRelay() && live.connected() == false && !live.latestHandle().empty()) live.keepResume();
  if (live.connected() || connecting || live.overRelay()) disconnect("locked");
  momentOnScreen = false;
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
// active one), back to the active one afterwards. Over Wi-Fi commands only run while no session is
// open (Bluetooth is off); in a chat through the iPhone they run during it too (the loop's own task).
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

// Keys one at a time (usage_rules.h addKey / removeKey): the app used to send its iPhone's whole
// Keychain list, which wiped keys added from elsewhere. The out-until times follow their keys.
void registerKeyCommands() {
  ble::on("companion.key.add", [](JsonObjectConst args, JsonObject reply) {
    std::vector<std::string> keys;
    for (const String &k : settings.keys) keys.push_back(k.c_str());
    const std::string why = addKey(keys, args["key"] | "", KeyRing::kMax);
    if (!why.empty()) {
      reply["ok"] = false;
      reply["error"] = why;
      return;
    }
    settings.keys.push_back(keys.back().c_str());
    ring.setCount(settings.keys.size());  // the new one: not out
    saveKeys();
    usage::markOut(0);
    LOGI("companion", "key ..%s added (%u)", keyHint(settings.keys.back()).c_str(), static_cast<unsigned>(settings.keys.size()));
    notifyChanged();
  });
  ble::on("companion.key.remove", [](JsonObjectConst args, JsonObject reply) {
    const std::string hint = args["hint"] | "";
    int index = -1;
    for (size_t i = 0; i < settings.keys.size() && index < 0; i++)
      if (keyHint(settings.keys[i]) == hint.c_str()) index = i;
    std::vector<std::string> keys;
    for (const String &k : settings.keys) keys.push_back(k.c_str());
    if (index < 0 || !removeKey(keys, hint)) {
      reply["ok"] = false;
      reply["error"] = "unknown key";
      return;
    }
    std::vector<int64_t> outs;
    for (int i = 0; i < ring.count(); i++)
      if (i != index) outs.push_back(ring.outUntil(i));
    settings.keys.erase(settings.keys.begin() + index);
    ring.setCount(settings.keys.size());
    ring.restore(0, outs);
    saveKeys();
    LOGI("companion", "key ..%s removed (%u left)", hint.c_str(), static_cast<unsigned>(settings.keys.size()));
    notifyChanged();
  });
}

// ---------- moments over Bluetooth (the app's Moments screen) ----------

void dayNames(JsonArray out, uint8_t bits) {
  static const char *kNames[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
  for (int i = 0; i < 7; i++)
    if (bits & (1 << i)) out.add(kNames[i]);
}

void timerReply(JsonObject o) {
  const TimerState &t = moments::timer();
  const int64_t now = time(nullptr);
  o["label"] = t.label;
  o["endsAt"] = t.endsAt;
  o["paused"] = t.paused;
  o["left"] = t.secondsLeft(now);
  o["tickEvery"] = t.tickEvery;
  o["what"] = t.what;
  o["companion"] = t.companion;
  o["ready"] = t.ready;
}

void rulesReply(JsonObject o) {
  JsonDocument doc;
  deserializeJson(doc, moments::rules().save());
  for (JsonPairConst kv : doc.as<JsonObjectConst>()) o[kv.key()] = kv.value();
}

void registerMomentCommands() {
  // {companion?}: every companion's when none is named. Times are local epoch seconds.
  ble::on("companion.moments", [](JsonObjectConst args, JsonObject reply) {
    reply["available"] = moments::available();
    reply["max"] = MomentList::kMax;
    const std::string only = args["companion"] | "";
    JsonArray list = reply["moments"].to<JsonArray>();
    for (const CompanionMoments &c : moments::all()) {
      if (!only.empty() && c.companion != only) continue;
      for (const Moment &m : c.list->all()) {
        JsonObject o = list.add<JsonObject>();
        o["id"] = m.id;
        o["companion"] = c.companion;
        o["kind"] = kindName(m.kind);
        o["at"] = m.at;
        o["repeat"] = repeatName(m.repeat);
        if (m.repeat == Repeat::Days) dayNames(o["days"].to<JsonArray>(), m.days);
        o["what"] = m.what;
        o["style"] = m.style;
        o["source"] = m.source;
        o["ready"] = m.ready;
        o["state"] = m.state == MomentState::Done ? "done" : m.state == MomentState::Missed ? "missed" : "upcoming";
      }
    }
    if (moments::timer().running) timerReply(reply["timer"].to<JsonObject>());
  });
  // {id?, companion, kind, at, repeat?, days?, what, style?}: a new one (no id) or a change → {id}
  ble::on("companion.moment.save", [](JsonObjectConst args, JsonObject reply) {
    const std::string who = args["companion"] | companions::active().id;
    MomentList *list = moments::of(who);
    auto fail = [&](const char *error) {
      reply["ok"] = false;
      reply["error"] = error;
    };
    if (!list) return fail(moments::available() ? "unknown companion" : "unavailable");
    const int id = args["id"] | 0;
    Moment m;
    if (id) {
      const Moment *old = list->find(id);
      if (!old) return fail("no such moment");
      m = *old;
    } else {
      m.source = "app";
      m.created = time(nullptr);
    }
    if (!kindFromName(args["kind"] | kindName(m.kind), m.kind) || m.kind == MomentKind::Spontaneous) return fail("bad kind");
    const int64_t at = args["at"] | m.at;
    if (at != m.at || !id) {
      if (at <= time(nullptr)) return fail("past");
      m.at = at - at % 60;
    }
    if (!repeatFromName(args["repeat"] | repeatName(m.repeat), m.repeat)) return fail("bad repeat");
    if (args["days"].is<JsonArrayConst>()) {
      std::vector<std::string> days;
      for (JsonVariantConst d : args["days"].as<JsonArrayConst>()) days.push_back(d | "");
      m.days = dayBits(days);
    }
    if (m.repeat == Repeat::Days && !m.days) return fail("no days");
    if (!firstOccurrence(m)) return fail("no days");  // the first time on an allowed day
    m.what = args["what"] | m.what.c_str();
    m.style = args["style"] | m.style.c_str();
    if (!validStyle(m.style)) return fail("bad style");
    if (m.state != MomentState::Upcoming) m.state = MomentState::Upcoming;
    const int saved = id ? (list->update(m) ? id : -1) : list->add(m);
    if (saved < 0) return fail(static_cast<int>(list->all().size()) >= MomentList::kMax ? "full" : "bad moment");
    moments::save(who);
    LOGI("moments", "app %s %s's moment %d at %s: %s", id ? "changed" : "added", who.c_str(), saved,
         localTimeText(m.at).c_str(), m.what.c_str());
    reply["id"] = saved;
    notifyMoments();
  });
  ble::on("companion.moment.delete", [](JsonObjectConst args, JsonObject reply) {
    const std::string who = args["companion"] | companions::active().id;
    MomentList *list = moments::of(who);
    const int id = args["id"] | 0;
    if (!list || !list->remove(id)) {
      reply["ok"] = false;
      reply["error"] = "no such moment";
      return;
    }
    moments::dropRecording(who, std::to_string(id));
    moments::save(who);
    notifyMoments();
  });
  // Plays it now on Dotty (a preview: it stays as it is).
  ble::on("companion.moment.play", [](JsonObjectConst args, JsonObject reply) {
    const std::string who = args["companion"] | companions::active().id;
    MomentList *list = moments::of(who);
    const int id = args["id"] | 0;
    const Moment *m = list ? list->find(id) : nullptr;
    if (!m || firing.active || momentBusy()) {
      reply["ok"] = false;
      reply["error"] = !m ? "no such moment" : "busy";
      return;
    }
    FireGroup g;
    g.companion = who;
    g.minute = m->at / 60;
    g.ids = {id};
    previewFiring = true;
    startFiring(g, true);
  });
  // No fields: the rules as they are; with fields: changed (values clamped).
  ble::on("companion.moment.rules", [](JsonObjectConst args, JsonObject reply) {
    bool changed = false;
    for (JsonPairConst kv : args) changed = changed || strcmp(kv.key().c_str(), "cmd") != 0;
    if (changed) {
      std::string json;
      serializeJson(args, json);
      moments::rules().load(json);
      if (!moments::saveRules()) {
        reply["ok"] = false;
        reply["error"] = "unavailable";
        return;
      }
      notifyMoments();
    }
    rulesReply(reply["rules"].to<JsonObject>());
  });
  // {action: start|pause|resume|cancel, seconds?, label?, tickEvery?} → {timer}
  ble::on("companion.timer", [](JsonObjectConst args, JsonObject reply) {
    TimerState &t = moments::timer();
    const std::string action = args["action"] | "";
    const int64_t now = time(nullptr);
    if (action == "start") {
      if (t.running) {
        reply["ok"] = false;
        reply["error"] = "running";
        return;
      }
      if (!t.start(args["seconds"] | 0, args["label"] | "", args["tickEvery"] | 0, companions::active().id, now)) {
        reply["ok"] = false;
        reply["error"] = "bad seconds";
        return;
      }
      t.what = args["what"] | "";
      t.style = args["style"] | "";
      if (!t.style.empty() && !validStyle(t.style)) t.style.clear();
    } else if (action == "pause") {
      t.pause(now);
    } else if (action == "resume") {
      t.resume(now);
    } else if (action == "cancel") {
      if (t.running) moments::dropRecording(t.companion, "timer");
      t.cancel();
    } else if (!action.empty()) {
      reply["ok"] = false;
      reply["error"] = "bad action";
      return;
    }
    if (!action.empty()) {
      moments::saveTimer();
      notifyTimer();
      redraw = true;
    }
    if (t.running) timerReply(reply["timer"].to<JsonObject>());
  });
}

// The phone's transcript (Apple's draft, Gemini's final) and summary for a recording.
void applyRecordingText(const RecText &t) {
  std::vector<RecordingMeta> all = note::list(t.companion);
  auto it = std::find_if(all.begin(), all.end(), [&](const RecordingMeta &m) { return m.name == t.id; });
  if (it == all.end()) return LOGW("note", "text for an unknown recording %s", t.id.c_str());
  RecordingMeta m = *it;
  companions::writeText(note::path(t.companion, m.name, "txt"), t.transcript);
  m.summary = t.summary;
  m.points = t.points;
  if (!t.title.empty()) m.title = t.title;
  m.isDraft = t.isDraft;
  m.textAt = time(nullptr);
  m.synced = true;
  note::saveMeta(t.companion, m);
  lastSyncActivity = millis();
  LOGI("note", "%s: %s transcript, %u characters; \"%s\"", m.name.c_str(), t.isDraft ? "draft" : "final",
       static_cast<unsigned>(t.transcript.size()), m.title.c_str());
  // The testing phase (the user, 2026-10-24): both texts in the log, to compare Apple's with Gemini's.
  auto logText = [](const char *what, const std::string &text) {
    for (size_t at = 0; at < text.size(); at += 90) LOGI("transcript", "%s: %s", what, text.substr(at, 90).c_str());
  };
  if (!t.draft.empty()) logText("Apple", t.draft);
  logText(t.isDraft ? "draft" : "final", t.transcript);
  if (reaction.waiting && reaction.name == m.name) {
    reaction.gotText = true;
    reaction.transcript = t.transcript;
  }
  notifyRecordings();
}

bool recordingNeedsSync(const RecordingMeta &m) {
  return !m.synced || m.textAt == 0;
}

bool syncWanted() {  // read from the card at most every 5 s
  static uint32_t checkedAt = 0;
  static bool wanted = false;
  if (checkedAt && millis() - checkedAt < 5000) return wanted;
  checkedAt = millis();
  wanted = false;
  for (const std::string &id : companions::list().order())
    for (const RecordingMeta &m : note::list(id))
      if (recordingNeedsSync(m) && syncTries[m.name] < 3) wanted = true;
  return wanted;
}

// rec.fetch: the whole recording, as when it was recorded (rec.begin, 0x02 frames, rec.end).
void pollNoteSend() {
  if (!noteSend.active) return;
  if (!noteSendBuf) noteSendBuf = static_cast<int16_t *>(heap_caps_malloc(2048 * 2, MALLOC_CAP_SPIRAM));
  if (!noteSendBuf) return;
  const size_t per = std::min<size_t>(relay::recordingFrameSamples(), 2048);
  lastSyncActivity = millis();
  phoneWake.extend(millis());                // progress: the wake goes on (30 minutes take ~10)
  if (!shell::locked()) shell::wake();       // and no auto-lock (it would turn Bluetooth off)
  for (int i = 0; i < 16; i++) {             // as fast as the link takes them (~3x realtime)
    const size_t at = noteSend.file.position();
    const size_t got = noteSend.file.read(reinterpret_cast<uint8_t *>(noteSendBuf), per * 2) / 2;
    const bool end = got == 0;
    if (!end && relay::sendRecordingFrame(noteSendBuf, got)) continue;
    if (!end && ble::connected()) {          // the link is busy: the same frame next pass
      noteSend.file.seek(at);
      return;
    }
    noteSend.file.close();
    noteSend.active = false;
    ble::notify(recEndEvent(noteSend.name, end));
    LOGI("note", "%s %s to the phone", noteSend.name.c_str(), end ? "sent" : "NOT sent (the phone went)");
    return;
  }
}

void registerRecordingCommands() {
  ble::on("companion.recordings", [](JsonObjectConst args, JsonObject reply) {
    const std::string who = args["companion"] | companions::active().id;
    const int offset = args["offset"] | 0, limit = std::min(args["limit"] | 50, 50);
    const std::vector<RecordingMeta> all = note::list(who);
    reply["total"] = all.size();
    JsonArray list = reply["recordings"].to<JsonArray>();
    for (int i = offset; i < static_cast<int>(all.size()) && i < offset + limit; i++) {
      const RecordingMeta &m = all[i];
      JsonObject o = list.add<JsonObject>();
      o["name"] = m.name;
      o["title"] = m.title;
      o["created"] = m.created;
      o["ms"] = m.ms;
      o["synced"] = m.synced;
      o["text"] = m.textAt > 0;
      o["isDraft"] = m.isDraft;
      o["summary"] = m.summary;
    }
    reply["companion"] = who;
  });
  ble::on("companion.recording.text", [](JsonObjectConst args, JsonObject reply) {
    RecText t;
    if (recText.command(args, t)) applyRecordingText(t);
    else if (args["done"] | false) {
      reply["ok"] = false;
      reply["error"] = "incomplete";  // the phone sends it all again
    }
  });
  ble::on("companion.recording.piece", [](JsonObjectConst args, JsonObject reply) {
    if (!recText.piece(args)) {
      reply["ok"] = false;
      reply["error"] = "bad piece";
    }
  });
  ble::on("companion.recording.rename", [](JsonObjectConst args, JsonObject reply) {
    const std::string who = args["companion"] | "", name = args["name"] | "", title = args["title"] | "";
    for (RecordingMeta m : note::list(who))
      if (m.name == name && !title.empty()) {
        m.title = title.substr(0, 80);
        note::saveMeta(who, m);
        notifyRecordings();
        return;
      }
    reply["ok"] = false;
    reply["error"] = "no such recording";
  });
  ble::on("companion.recording.delete", [](JsonObjectConst args, JsonObject reply) {
    if (!note::remove(args["companion"] | "", args["name"] | "")) {
      reply["ok"] = false;
      reply["error"] = "no such recording";
      return;
    }
    notifyRecordings();
  });
  ble::on("companion.recording.play", [](JsonObjectConst args, JsonObject reply) {
    const std::string who = args["companion"] | "", name = args["name"] | "";
    for (const RecordingMeta &m : note::list(who))
      if (m.name == name) {
        pendingPlayCompanion = who;
        pendingPlayName = name;
        pendingPlayTitle = m.title;
        return;
      }
    reply["ok"] = false;
    reply["error"] = "no such recording";
  });
  // The phone wants a whole recording (it missed it, joined late, or a frame was lost).
  ble::on("rec.fetch", [](JsonObjectConst args, JsonObject reply) {
    const std::string who = args["companion"] | "", name = args["id"] | "";
    const uint32_t from = args["from"] | 0u;  // samples the phone already has (an interrupted fetch)
    if (who.empty() || name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos) {
      reply["ok"] = false;
      reply["error"] = "bad name";
      return;
    }
    File f = SD_MMC.open(note::path(who, name, "wav"));
    if (!f || noteSend.active || note::active()) {
      reply["ok"] = false;
      reply["error"] = !f ? "no such recording" : "busy";
      return;
    }
    f.seek(44 + static_cast<size_t>(from) * 2 < f.size() ? 44 + static_cast<size_t>(from) * 2 : 44);
    std::string title = name;
    for (const RecordingMeta &m : note::list(who))
      if (m.name == name) title = m.title;
    relay::resetRecording();
    ble::notify(recBeginEvent(name, who, title, f.position() > 44 ? (f.position() - 44) / 2 : 0));
    noteSend.active = true;
    noteSend.file = f;
    noteSend.companion = who;
    noteSend.name = name;
    lastSyncActivity = millis();
    LOGI("note", "sending %s to the phone", name.c_str());
  });
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
  registerMomentCommands();
  registerRecordingCommands();
  registerKeyCommands();
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
    moments::companionRemoved(id);
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
  config.beforeLock = [] {  // a recording ends (Done) while Bluetooth is still on: the phone hears its end
    if (deckOpen) finishDeck(deck.lockOrPower(millis()) == NoteDeck::Event::Done);
  };
  config.beforePowerOff = [] {  // PWR held or the battery empty: the part being recorded is kept
    if (deckOpen) finishDeck(deck.lockOrPower(millis()) == NoteDeck::Event::Done);
    else if (note::active()) note::finish(true);
  };
  config.nowPlaying = [] {  // the lock screen's bottom line: a missed alarm, else a running timer
    if (missedAlarmLine.length()) return missedAlarmLine;
    const TimerState &t = moments::timer();
    return t.running ? String(t.lockLine(time(nullptr)).c_str()) : String();
  };
  config.onLockedWake = [] { return false; };          // lets wakeLockedAt end a sleep (the loop does the work)
  shell::begin(config);
  storage::begin();
  companions::begin();  // also points memories and history at the active companion
  moments::begin();     // reminders, alarms, the companions' own lines, the timer
  note::repairAll();    // voice notes a power cut left with a stale header
  LOGI("boot", "audio %s", player.begin(voice::kInRate) ? "ok" : "FAILED");
  voice::begin(player);
  loadSettings();
  applyCompanion();
  loadLast();
  loadMacros();
  for (const char *old : {"00", "01", "02", "10", "11", "12"})  // the acting demo's recordings (gone in 0.3.0)
    SD_MMC.remove(storage::myDataDir() + "/act_" + old + ".wav");
  registerCommands();
  relay::begin();  // the phone's relay.hello, its data (a chat through the iPhone)
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
  static bool wasLocked = false;
  if (!unlocked && deckOpen) {  // locking (or the auto-lock) = Done
    if (deck.lockOrPower(millis()) == NoteDeck::Event::Done) finishDeck(true);
    else finishDeck(false);
  }
  if (!unlocked) {
    stopReplay();
    if (info.state != Talk::Offline && info.state != Talk::Error && !firing.active) onLock();
    wasLocked = true;
    pollMoments();  // one due unlocks Dotty (shell::wake) and plays from the next pass
    pollTimer();
    if (phoneWake.stage() != PhoneWake::Stage::Idle || bgSession || connecting || lineJob.busy()) backgroundPoll();
    else pollPhoneWake();  // is one due?
    armLockedWake();
    return;
  }
  if (wasLocked) {  // unlocked: the lock screen's "Missed alarm" has been seen
    wasLocked = false;
    missedAlarmLine = "";
  }
  pollMoments();
  pollTimer();
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
  // The connect task owns the WebSocket until it ends; a relay session is the loop's from the start.
  if (!connecting || live.overRelay()) live.poll();
  // The phone's data when no relay session reads it (a session's own poll does, in order).
  if (!(live.overRelay() && live.connected())) relay::poll();
  voice::poll();
  pollReplay();
  lastSavePoll();

  // The background connect finished (the screen may already be listening).
  if (awaitingConnect && !connecting && !live.connected() && !resumeRetryAt && !bgSession) {
    awaitingConnect = awaitingSend = false;
    early.clear();
    voice::stopListening();
    disconnect("connect failed");
    setState(Talk::Error, connectError.length() ? connectError : String("Can't connect"));
  }
  if (!turnSent && live.ready() && (info.state == Talk::Listening || awaitingSend) && loudChunks >= kVoiceChunks) openTurn();
  if (turnSent && earlyPending()) flushEarly();
  if (awaitingSend && turnSent && !earlyPending()) {
    awaitingSend = false;
    finishTurn();
  }

  // A session nobody is using (left by a cancelled connect): close it, Bluetooth comes back.
  if (!connecting && !awaitingConnect && live.connected() && !bgSession && (info.state == Talk::Offline || info.state == Talk::Error)) {
    disconnect("not in use");
  }
  // The session ended without a word (closed here, or the socket died quietly).
  if (!connecting && !awaitingConnect && !live.connected()) {
    if ((info.state == Talk::Listening && !turnSent) || awaitingSend) {
      connect();  // a recording is waiting for it
    } else if (momentOnScreen) {
      // A moment's words stay up for its reply window (3 minutes), then the usual screen.
      if (!firing.active && !(momentReplyUntil && static_cast<int32_t>(momentReplyUntil - millis()) > 0) &&
          millis() - lastTurnAt > kIdleMs) {
        momentOnScreen = false;
        setState(Talk::Offline, idleStatus());
      }
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

  // The phone took a chat's setup but never answered (a lost piece, an app without its relay…):
  // after 10 s the chat goes over Wi-Fi instead, the recording kept, and the phone sits out a minute.
  if (awaitingConnect && live.overRelay() && live.connected() && !live.ready() && millis() - relayOpenAt > 10000 && !bgSession) {
    LOGW("companion", "the phone didn't answer the chat's setup: Wi-Fi instead");
    relayDistrustUntil = millis() + 60000;
    live.close();
    awaitingConnect = false;
    path.end();
    connect();
  }
  if (resumeRetryAt && millis() > resumeRetryAt) {  // a hand-over's resume, again
    resumeRetryAt = 0;
    if (relay::phoneReady()) beginRelay(resumeRetryHandle);
  }
  if (live.ready() && live.overRelay()) resumeRetryHandle.clear();  // resumed: done

  // The race and the grace window (relay_path.h).
  // A phone counts only if it holds this chat's key (else it can't relay it).
  const bool phoneNow = relay::phoneReady() && relay::phoneHasKey(keyHint(sessionKeyText));
  if (phoneNow && !phoneWasReady) {
    path.phoneReady(millis());
    if (path.takeDropWifi()) {  // the phone won: the Wi-Fi task stops short of Gemini
      bool tooLate;
      taskENTER_CRITICAL(&raceLock);
      tooLate = wifiBegan;
      if (!tooLate) cancelConnect = true;
      taskEXIT_CRITICAL(&raceLock);
      if (tooLate) {  // Wi-Fi is about to be ready: it keeps the chat (the window may hand it over)
        path.end();
        path.press(millis(), false);
        LOGI("companion", "the phone came back late: Wi-Fi keeps this chat");
      } else {
        LOGI("companion", "the phone won the race");
        ble::advertiseSlowly(false);
        net::abortConnect();  // the Wi-Fi join stops: its radio and memory are the relay's again
        beginRelay();
      }
    }
  }
  phoneWasReady = phoneNow;
  // The phone came back during a Wi-Fi chat: it takes the chat between turns (Wi-Fi and Bluetooth
  // together are the tight case — a WebSocket stalled 9 s and dropped while the phone reconnected).
  if (live.ready() && !live.overRelay() && phoneNow && (info.state == Talk::Ready || info.state == Talk::After) &&
      !writing && !reading && !reviewStart && path.takeHandover()) {
    handOver();
  }
  if (live.connected() && !live.overRelay() && ble::running() && !path.keepBluetooth(millis())) {
    LOGI("companion", "Wi-Fi chat: Bluetooth off for the rest of it");
    ble::stop();
  }

  // Push-to-talk: talking can start before the session is ready.
  const bool bootDown = digitalRead(PIN_BTN_BOOT) == LOW || simulatedHold;
  static uint32_t downAt = 0;
  if (!bootDown) downAt = 0;
  else if (!downAt) downAt = millis();
  const bool held = bootDown && millis() - downAt >= kHoldMs;
  static uint32_t bootUpSince = 0;  // the release, debounced for the deck
  if (bootDown) bootUpSince = 0;
  else if (!bootUpSince) bootUpSince = millis();
  if (deckOpen) pollDeck(held, bootUpSince && millis() - bootUpSince >= 40);
  if (notePlaying && held) player.stop();
  static uint32_t answeredPress = 0;  // the press that answered a moment doesn't also start a chat
  if (held && firing.active && static_cast<int32_t>(downAt - firing.startedMs) >= 0) {
    const bool ringing = firing.kind == "alarm" || firing.kind == "timer";  // BOOT only stops those
    answerFiring();
    if (ringing) answeredPress = downAt;
  }
  if (held && (bgSession || phoneWake.stage() != PhoneWake::Stage::Idle)) {  // talking beats recording lines
    LOGI("wake", "BOOT: the recording session gives way");
    phoneWake.end();
    shell::endBluetoothWindow();
    if (bgSession) disconnect("a chat");
  }
  if (held && !deckOpen && !historyOpen && !settingsOpen && !awaitingSend && answeredPress != downAt) {
    static uint32_t triedAt = 0;  // one connect per press (an error screen would retry every pass)
    if ((info.state == Talk::Offline || info.state == Talk::Error) && triedAt != downAt) {
      triedAt = downAt;
      connect();
    }
    if (info.state == Talk::Connecting || info.state == Talk::Ready || info.state == Talk::After ||
        info.state == Talk::Speaking) {
      if (live.connected() && !live.overRelay() && relay::phoneReady() && path.takeHandover()) handOver();
      startTalking();  // also interrupts a reply
    }
  }

  if (info.state == Talk::Listening) {
    static int16_t chunk[voice::kChunkFrames];
    size_t frames = 0;
    while (voice::nextChunk(chunk, frames)) {
      sendVoice(chunk, frames);
      peakDb = max(peakDb, voice::levelDb());
      if (voice::levelDb() >= kVoiceDb) loudChunks++;
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
  pollLines();
  pollPhoneWake();
  pollNotePlay();
  pollNoteSend();
  pollReaction();
  pollTypedTurn();
  if (deckPending && millis() - deckPendingAt > 120000) {
    deckPending = false;
    LOGW("note", "the deck didn't open in time: dropped");
  }
  if (deckPending && !deckOpen && (info.state == Talk::After || info.state == Talk::Ready) && !voice::playing() &&
      !live.inTurn() && !writing && !reading) {
    deckPending = false;
    openDeck(deckPendingTitle);
  }
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
  const uint32_t idleMs = live.overRelay() ? kRelayIdleMs : kIdleMs;
  if ((info.state == Talk::Ready || info.state == Talk::After) && millis() - lastTurnAt > idleMs && !bgSession && !deckOpen &&
      pendingTypedText.empty()) {
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
  if (input.gesture == Touch::Gesture::LongPress && timerShowing() && !timerConfirm) {  // "Cancel the timer?"
    timerConfirm = true;
    redraw = true;
  }
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
  if (input.key == 'n' && !note::active()) {  // dev: a 10 s test voice note for the active companion
    if (note::begin(companions::active().id, "Test note") && voice::startListening()) {
      note::partStart();
      noteTestUntil = millis() + 10000;
    }
  }
  if (noteTestUntil) {
    size_t frames = 0;
    while (voice::nextChunk(noteChunk, frames)) note::add(noteChunk, frames);
    if (millis() > noteTestUntil) {
      noteTestUntil = 0;
      voice::stopListening();
      note::finish(true);
    }
  }
  if (input.key == 'w' && HWCDC::isPlugged()) {  // dev: the newest voice note over serial (#WAV … #END, as Tape's)
    const std::vector<RecordingMeta> all = note::list(companions::active().id);
    File f = all.empty() ? File() : SD_MMC.open(note::path(companions::active().id, all.front().name, "wav"));
    if (f) {
      Serial.printf("\n#WAV %u %s.wav\n", static_cast<unsigned>(f.size()), all.front().name.c_str());
      uint8_t buf[50];
      char line[2 * sizeof(buf) + 2];
      for (size_t n; (n = f.read(buf, sizeof(buf))) > 0;) {
        size_t len = 0;
        for (size_t j = 0; j < n; j++) len += snprintf(line + len, sizeof(line) - len, "%02x", buf[j]);
        line[len++] = '\n';
        Serial.write(reinterpret_cast<uint8_t *>(line), len);
      }
      Serial.flush();
      Serial.print("#END\n");
    }
  }
  if (input.key == '%') {  // dev: internal RAM now (hunting a leak)
    LOGI("heap", "internal %u KB free, largest %u KB, min ever %u KB; PSRAM %u KB", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024, heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024,
         heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
  }
  if (input.key == '^') {  // dev: chats over Wi-Fi for 10 minutes (the phone is left out)
    relayDistrustUntil = millis() + 600000;
    ble::stop();  // the phone can't win the race (Bluetooth comes back after the chat)
    LOGI("dev", "Wi-Fi chats for 10 minutes");
  }
  if (input.key == 'y') {  // dev: an app-made reminder 40 minutes out (recorded at the next chance)
    const std::string r = ble::runHere("{\"cmd\":\"companion.moment.save\",\"kind\":\"reminder\",\"at\":" +
                                       std::to_string(time(nullptr) + 2400) + ",\"what\":\"stretch your back\",\"style\":\"calm\"}");
    LOGI("dev", "%s", r.c_str());
  }
  if (input.key == 'z' || input.key == '!') {  // dev: plan today's lines now, in hours around now (! puts them back)
    const int64_t now = time(nullptr);
    const int minute = static_cast<int>(now % 86400 / 60);
    if (input.key == '!') {  // and the test moments go (every companion's upcoming ones)
      for (const CompanionMoments &c : moments::all()) {
        std::vector<int> ids;
        for (const Moment &m : c.list->all()) ids.push_back(m.id);
        for (const int id : ids) {
          c.list->remove(id);
          moments::dropRecording(c.companion, std::to_string(id));
        }
        moments::save(c.companion);
        if (!ids.empty()) LOGI("dev", "%s: %u test moments removed", c.companion.c_str(), static_cast<unsigned>(ids.size()));
      }
      loadWakeTimes();
      plannedDay = 0;  // today's real plan still happens
      saveWakeTimes();
    }
    const std::string hours = input.key == '!' ? "\"from\":600,\"to\":1260,\"perDay\":2"
                                               : "\"from\":" + std::to_string((minute + 5) % 1440) + ",\"to\":" +
                                                     std::to_string(std::min(minute + 300, 1439));
    LOGI("dev", "%s", ble::runHere("{\"cmd\":\"companion.moment.rules\"," + hours + "}").c_str());
    if (input.key == 'z') {
      forcePlan = true;
      loadWakeTimes();
      lastWake = 0;  // the next locked pass wakes
      LOGI("dev", "plan forced, phone wake due");
    }
  }
  if (input.key == '@') {  // dev: the app's moment commands, run here (an alarm + a reminder at one minute)
    const int64_t at = time(nullptr) + 120;
    const std::string who = companions::active().id;
    const std::string cmds[] = {
        "{\"cmd\":\"companion.moment.save\",\"companion\":\"" + who + "\",\"kind\":\"alarm\",\"at\":" +
            std::to_string(at) + ",\"what\":\"time to get up\",\"style\":\"excited\"}",
        "{\"cmd\":\"companion.moment.save\",\"companion\":\"" + who + "\",\"kind\":\"reminder\",\"at\":" +
            std::to_string(at) + ",\"what\":\"take the vitamins\"}",
        "{\"cmd\":\"companion.moment.rules\",\"perDay\":9}",
        "{\"cmd\":\"companion.moments\"}",
        "{\"cmd\":\"companion.timer\"}"};
    for (const std::string &c : cmds) {
      const std::string r = ble::runHere(c);
      for (size_t at = 0; at < r.size(); at += 100) LOGI("dev", "%s", r.substr(at, 100).c_str());
    }
  }
  if (input.key == 'F') {  // dev: the next moment, now
    const int64_t now = time(nullptr);
    for (const CompanionMoments &c : moments::all()) {
      Moment *best = nullptr;
      for (const Moment &m : c.list->all())
        if (m.state == MomentState::Upcoming && (!best || m.at < best->at)) best = c.list->find(m.id);
      if (!best) continue;
      best->at = now - now % 60;
      moments::save(c.companion);
      LOGI("moments", "dev: %s's moment %d moved to now", c.companion.c_str(), best->id);
      break;
    }
  }
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
                      : input.key == 'L' ? "Remind me in 2 minutes to drink a glass of water, and sing it to me."
                      : input.key == 'C' ? "Start a 1 minute timer."
                      : input.key == 'u' ? "What reminders do I have?"
                      : input.key == 'b' ? "I want to record something about eating bananas in space."
                      : input.key == 'j' ? "Play my bananas recording."
                      : input.key == 'H' ? "" : nullptr;
  if (typed && live.ready() && (info.state == Talk::Ready || info.state == Talk::After)) {
    const std::string text = *typed ? typed
                                    : std::string("(Dotty, not the user: a reading test.) Read the text below aloud exactly, "
                                                  "word for word, in a calm, slow voice. Don't add, skip or change "
                                                  "anything, don't call write_long, and say nothing else.\n\n") + kScript;
    typedTurn(text, text);
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
  if (input.key == 'N') {  // dev: show the note (each press the next: short, then two lines)
    static int n = 0;
    static const char *kNotes[] = {"Remembered", "Saving your reminder...", "Thinking back..."};
    info.note = kNotes[n++ % 3];
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
  if (millis() - lastFrameAt > kFrameMs && info.state != Talk::Listening && !timerShowing()) {
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
