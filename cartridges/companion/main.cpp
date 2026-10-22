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

#include <string>
#include <vector>

#include "audio_player.h"
#include "avatar.h"
#include "board_pins.h"
#include "cartridge.h"
#include "core_ble.h"
#include "gemini_live.h"
#include "history.h"
#include "memories.h"
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
#if __has_include("dev_key.h")
#include "dev_key.h"  // bench only (gitignored): COMPANION_DEV_KEY seeds an empty key
#endif

DOTTY_CARTRIDGE("companion", "Companion", "0.2.0");

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
  String key, voice = "Zephyr", avatar = "robot";
  bool textOnly = false;
  int volume = 70;
  String favourites = kDefaultFavourites;  // the voices Dotty steps through (picked in the app)
} settings;
bool settingsOpen = false;
bool settingsChanged = false;  // voice or speech changed: the session restarts with them
int quotaStrikes = 0;          // quota errors since the last good turn

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
uint32_t noteUntil = 0;        // "Remembered"… shows until then
// Bluetooth comes back a moment after a session's Wi-Fi is off: once, with it started in the same
// instant, the iPhone's reconnect hung the Bluetooth controller (interrupt watchdog in
// ll_connection_update_ind_handler, 2026-10-22).
uint32_t bleBackAt = 0;
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
  settings.key = p.getString("key", "");
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
  if (settings.key.isEmpty()) settings.key = COMPANION_DEV_KEY;
#endif
  info.textOnly = settings.textOnly;
  info.avatar = settings.avatar;
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

void disconnect(const char *why) {
  reviewStart = 0;
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

// A memory call from the model: change the store, save it, show a note for 3 s, note it for the
// history line. Returns the memory's id for remember (sent back so the model can name it).
int applyMemory(const ToolCall &c) {
  MemoryStore &store = memories::store();
  const int64_t now = time(nullptr);
  int id = -1;
  const char *note = nullptr;
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
  } else if (c.name == "forget_memory") {
    const Memory *m = store.find(c.memoryId);
    const std::string text = m ? m->text : "";
    if (store.remove(c.memoryId)) {
      note = "Forgotten";
      change = "forgot: " + text;
    }
  }
  LOGI("companion", "%s %d: %s %.40s", c.name.c_str(), c.memoryId, note ? note : "ignored", change.c_str());
  if (!change.empty()) {
    memories::save();
    turnMemory += (turnMemory.empty() ? "" : "; ") + change;
  }
  if (note) {
    info.note = note;
    noteUntil = millis() + 3000;
    redraw = true;
  }
  return id;
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
    const char *status = net::rssi() < -75 ? "Weak Wi-Fi: replies may stutter" : "Hold BOOT to talk";
    if (net::rssi() < -75) LOGW("companion", "weak Wi-Fi: %d dBm", net::rssi());
    if (info.state == Talk::Connecting) setState(Talk::Ready, status);  // else talking already
  };
  cb.heard = [](const std::string &t) {
    heard += t;
    info.heard = heard.c_str();
    redraw = true;
  };
  cb.said = [](const std::string &t) {
    if (info.state != Talk::Thinking && info.state != Talk::Speaking) return;  // an interrupted reply
    if (settings.textOnly) {
      showText(t);
      beginReply();
    } else {
      textQueue.push_back({replyFrames, t});
    }
  };
  cb.audio = [](const int16_t *pcm, size_t frames) {
    if (info.state != Talk::Thinking && info.state != Talk::Speaking) return;
    if (!voice::playing()) beginReply();
    voice::play(pcm, frames);
    replyFrames += frames;
  };
  cb.mood = [](const std::string &m) {
    turnMood = m.c_str();
    info.mood = turnMood;
  };
  cb.spent = [](int tokens) { usage::addTokens(tokens); };
  cb.memory = [](const ToolCall &c) { return applyMemory(c); };
  cb.turnDone = [](int tokens) {
    quotaStrikes = 0;
    if (reviewStart) {  // the end-of-chat review: its memory calls are applied, nothing else to keep
      usage::addTokens(tokens);
      LOGI("companion", "memory review done (%d tokens)", tokens);
      disconnect("idle");
      reviewStart = 0;
      setState(Talk::Offline, "Hold BOOT to talk");
      return;
    }
    voice::playEnd();
    std::string full = said;  // history gets the whole reply, even before it's all been heard
    for (const TextPiece &p : textQueue) full += p.text;
    const uint32_t ms = firstAudioAt ? firstAudioAt - releasedAt : millis() - releasedAt;
    history::append(heard, full, turnMood.c_str(), ms, tokens, turnMemory);
    usage::addTurn(tokens);
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
      const bool daily = retry <= 0 && quotaStrikes > 0;
      const int64_t until = retry > 0 ? nowUtc() + retry : daily ? nextPacificMidnightUtc(nowUtc()) : nowUtc() + 60;
      quotaStrikes++;
      usage::markOut(until);
      disconnect("quota");
      setState(Talk::Error, daily ? outOfWords(until) : String("Gemini is busy. Trying again in a minute."));
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
    config.voice = settings.voice.c_str();
    config.textOnly = settings.textOnly;
    config.systemInstruction = systemInstruction(localClock(nowUtc()).c_str(), placeName().c_str(),
                                                 "You are Dotty, a small desk companion.", memorySection);  // Task 4: the companion's persona
    ok = live.begin(settings.key.c_str(), config, callbacks());
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
  if (settings.key.isEmpty()) return setState(Talk::Error, "Key missing. Add it in the app.");
  if (const int64_t out = usage::outUntilUtc()) return setState(Talk::Error, outOfWords(out));
  // Bluetooth is off for the session: with it on (and the iPhone connected) TLS left 36 KB of
  // internal RAM, largest block 12 KB, and a session hung in its sends until the watchdog reset.
  ble::stop();
  power::setWakeLock(power::kWakeLockNetwork, true);
  memorySection = memories::store().promptSection();  // the task only reads this copy
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

void drawApp() {
  if (settingsOpen) {
    screens::drawSettings(settingsView());
  } else if (historyOpen) {
    std::vector<std::pair<String, String>> exchanges;
    loadHistory(exchanges);
    screens::drawHistory(exchanges, historyScroll);
  } else {
    screens::draw(info);
  }
}

void onTap(uint16_t x, uint16_t y) {
  if (settingsOpen) return onSettingsTap(x, y);
  if (historyOpen) {
    if (nav::hit(x, y) < 0) historyOpen = false;
    redraw = true;
    return;
  }
  if (y < 40 && x < 50) {  // settings
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
  if (info.state != Talk::Error) setState(Talk::Offline, "Hold BOOT to talk");
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

void registerCommands() {
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
    reply["keySet"] = !settings.key.isEmpty();
    if (settings.key.length() >= 4) reply["keyHint"] = settings.key.substring(settings.key.length() - 4);
    JsonObject today = reply["today"].to<JsonObject>();
    today["turns"] = usage::turnsToday();
    today["tokens"] = usage::tokensToday();
    if (const int64_t out = usage::outUntilUtc()) reply["outUntil"] = out;
    reply["memories"] = static_cast<int>(memories::store().all().size());
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
    if (unavailable(reply)) return;
    const std::string text = args["text"] | "", group = args["group"] | "";
    const bool ok = memories::store().update(args["id"] | -1, args["text"].is<const char *>() ? &text : nullptr,
                                             args["group"].is<const char *>() ? &group : nullptr, time(nullptr));
    reply["ok"] = ok;
    if (ok) changed();
  });
  ble::on("companion.memory.delete", [unavailable, changed](JsonObjectConst args, JsonObject reply) {
    if (unavailable(reply)) return;
    const bool ok = memories::store().remove(args["id"] | -1);
    reply["ok"] = ok;
    if (ok) changed();
  });
  ble::on("companion.memory.clear", [unavailable, changed](JsonObjectConst, JsonObject reply) {
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
    }
    if (args["key"].is<const char *>()) {
      settings.key = trimKey(args["key"].as<const char *>()).c_str();
      Preferences p;
      p.begin("companion", false);
      p.putString("key", settings.key);
      p.end();
      reconnect = true;
      usage::markOut(0);  // a new key = a new allowance
      LOGI("companion", "key %s", settings.key.isEmpty() ? "cleared" : ("set (..." + settings.key.substring(max(0, (int)settings.key.length() - 4)) + ")").c_str());
    }
    saveSettings();
    if (reconnect && live.connected() && (info.state == Talk::Ready || info.state == Talk::After)) {
      disconnect("settings changed");  // the next press opens a session with them
    }
    redraw = true;
    notifyChanged();
  });
  ble::on("companion.history", [](JsonObjectConst args, JsonObject reply) {
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
    reply["ok"] = history::forget(args["day"] | "");
    notifyChanged();
  });
}

// Locked: the avatar asleep under the clock, with today's chats beside it once there are some.
int16_t drawLockAvatar(Adafruit_GFX &gfx, const tm &, int16_t maxHeight) {
  if (maxHeight < avatar::kSmall) return 0;
  const int turns = usage::turnsToday();
  const String line = turns ? String(turns) + (turns == 1 ? " chat today" : " chats today") : String();
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
  memories::begin();
  LOGI("boot", "audio %s", player.begin(voice::kInRate) ? "ok" : "FAILED");
  voice::begin(player);
  loadSettings();
  registerCommands();
  setState(Talk::Offline, "Hold BOOT to talk");
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
    if (info.state != Talk::Offline && info.state != Talk::Error) onLock();
    return;
  }
  if (!connecting) live.poll();  // the connect task owns the socket until it ends
  voice::poll();

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
      setState(Talk::Offline, "Hold BOOT to talk");
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
  if (info.state == Talk::Thinking && !awaitingSend && millis() - releasedAt > 20000) setState(Talk::Ready, "No reply. Try again?");
  if (info.state == Talk::Speaking && !textQueue.empty() && (millis() - lastTextAt > 1000 || !voice::playing())) {
    lastTextAt = millis();  // at most a redraw a second: a partial refresh takes 0.6 s
    releaseText(!voice::playing());
  }
  if (info.state == Talk::Speaking && !voice::playing() && !live.inTurn()) {
    LOGI("companion", "spoken; longest loop pause %lu ms", static_cast<unsigned long>(longestPause));
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
      setState(Talk::Offline, "Hold BOOT to talk");
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
      live.sendText(
          "(Dotty, not the user: the chat is over.) Go over it: call remember or update_memory for anything worth "
          "keeping that you haven't saved yet. Then reply with just: Bye.");
    } else if (!reviewStart || millis() - reviewStart > 20000) {
      reviewStart = 0;
      disconnect("idle");
      setState(Talk::Offline, "Hold BOOT to talk");
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
    setState(Talk::Offline, "Hold BOOT to talk");
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
