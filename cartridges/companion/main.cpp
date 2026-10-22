// Companion: talk to an AI (Gemini Live) by holding BOOT; it answers out loud in about a second,
// with the text on screen and an animated avatar. See
// docs/superpowers/specs/2026-10-22-companion-talking-design.md.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include "audio_player.h"
#include "cartridge.h"
#include "images/sleep_panda.h"
#include "core_ble.h"
#include "log.h"
#include "net.h"
#include "gemini_live.h"
#include "ws_client.h"
#if __has_include("dev_key.h")
#include "dev_key.h"  // bench only (gitignored): COMPANION_DEV_KEY
#endif
#include <Preferences.h>
#include "shell.h"
#include "storage.h"
#include "ui.h"

DOTTY_CARTRIDGE("companion", "Companion", "0.1.0");

namespace {

using shell::epd;

AudioPlayer player;

void drawApp() {
  epd.fillScreen(EpdDisplay::kWhite);
  epd.setTextColor(EpdDisplay::kBlack);
  epd.setFont(&FreeSansBold9pt7b);
  ui::drawCentered(epd, "Companion", 100);
}

// Developer aid (serial S): 2 s of a 440 Hz tone at 24 kHz through the streaming API, written
// in 480-frame pieces like network audio would arrive.
void streamTestTone() {
  constexpr uint32_t kRate = 24000;
  if (!player.startStream(kRate)) {
    LOGW("companion", "stream test: startStream failed");
    return;
  }
  static int16_t piece[480];
  uint32_t phase = 0;
  const uint32_t step = static_cast<uint32_t>(440.0 / kRate * 4294967296.0);
  for (int sent = 0; sent < 2 * kRate;) {
    for (int i = 0; i < 480; i++, phase += step) piece[i] = static_cast<int16_t>(6000 * sinf(phase / 4294967296.0f * 2 * PI));
    size_t done = 0;
    while (done < 480) {
      done += player.writeStream(piece + done, 480 - done);
      if (done < 480) delay(5);
    }
    sent += 480;
  }
  player.endStream();
  LOGI("companion", "stream test: sent 2 s");
}

// Developer aid (serial W): the WebSocket client against a public echo server.
void echoTest() {
  ble::stop();
  String error;
  if (!net::connect(error)) {
    LOGW("ws", "echo test: Wi-Fi: %s", error.c_str());
    return;
  }
  LOGI("ws", "heap before TLS: internal %u free, largest %u", heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
       heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  WsClient ws;
  const uint32_t start = millis();
  if (!ws.connect("echo.websocket.org", "/", error)) {
    LOGW("ws", "echo test: %s", error.c_str());
    return;
  }
  LOGI("ws", "connected in %lu ms; heap internal %u free, largest %u", millis() - start,
       heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  ws.sendText(std::string("hello"));
  const uint32_t sent = millis();
  bool echoed = false;
  while (!echoed && millis() - sent < 5000) {
    ws.poll([&](const std::string &m) {
      LOGI("ws", "message: \"%s\"", m.substr(0, 60).c_str());
      if (m == "hello") echoed = true;
    });
    delay(10);
  }
  LOGI("ws", "echo %s", echoed ? "\"hello\"" : "missing");
  ws.close();
  net::disconnect();
  ble::start();
}

GeminiLive live;

String apiKey() {
  Preferences p;
  p.begin("companion", true);
  String key = p.getString("key", "");
  p.end();
#ifdef COMPANION_DEV_KEY
  if (key.isEmpty()) key = COMPANION_DEV_KEY;
#endif
  return key;
}

// Developer aid (serial c, then T): connect to Gemini Live and send a typed turn.
void liveTest(bool typed) {
  if (!live.connected()) {
    ble::stop();
    String error;
    if (!net::connect(error)) {
      LOGW("live", "Wi-Fi: %s", error.c_str());
      return;
    }
    LiveConfig config;
    config.systemInstruction = systemInstruction("12:00", "");
    GeminiLive::Callbacks cb;
    cb.ready = [] { LOGI("live", "test: ready"); };
    cb.said = [](const std::string &t) { LOGI("live", "said: %s", t.c_str()); };
    cb.mood = [](const std::string &m) { LOGI("live", "mood: %s", m.c_str()); };
    cb.audio = [](const int16_t *, size_t frames) {
      static size_t total = 0;
      total += frames;
      if (total % 24000 < frames) LOGI("live", "audio: %u frames so far", static_cast<unsigned>(total));
    };
    cb.turnDone = [](int tokens) { LOGI("live", "turn done, %d tokens", tokens); };
    cb.failed = [](const std::string &e, int retry, bool quota, bool badKey) {
      LOGW("live", "failed: %s (retry %d, quota %d, badKey %d)", e.c_str(), retry, quota, badKey);
    };
    live.begin(apiKey().c_str(), config, cb);
  }
  if (typed && live.ready()) live.sendText("Say hello to me in one short sentence.");
}

const shell::Picture kOffPictures[] = {{kSleepPanda, kSleepPandaWidth, kSleepPandaHeight}};

}  // namespace

void setup() {
  shell::Config config;
  config.drawApp = drawApp;
  config.offPictures = kOffPictures;
  config.offPictureCount = 1;
  config.sleepApp = [] { player.powerDown(); };
  config.wakeApp = [] { player.powerUp(); };
  shell::begin(config);
  storage::begin();
  LOGI("boot", "audio %s", player.begin(16000) ? "ok" : "FAILED");
  shell::showApp();
}

void loop() {
  shell::Input input;
  if (!shell::update(input)) return;
  if (input.key == 'S') streamTestTone();
  if (input.key == 'W') echoTest();
  if (input.key == 'c') liveTest(false);
  if (input.key == 'T') liveTest(true);
  live.poll();
  delay(10);
}
