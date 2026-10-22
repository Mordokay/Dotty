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
#include "ws_client.h"
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
  delay(10);
}
