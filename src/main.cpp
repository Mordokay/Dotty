#include <Arduino.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

#include "board_pins.h"
#include "epd_display.h"

namespace {

constexpr uint32_t kStatusIntervalMs = 10000;
constexpr uint32_t kPowerOffHoldMs = 2000;
constexpr int kPartialRefreshesPerFull = 30;

EpdDisplay epd;

uint32_t readBatteryMv() {
  return analogReadMilliVolts(PIN_VBAT_ADC) * 2;  // 1:2 divider
}

void drawCentered(const char *text, int16_t y) {
  int16_t x1, y1;
  uint16_t w, h;
  epd.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  epd.setCursor((EpdDisplay::kSize - w) / 2 - x1, y);
  epd.print(text);
}

void drawFace() {
  const int16_t cx = EpdDisplay::kSize / 2;
  // Smile: a 3 px ring with its top half erased.
  epd.fillCircle(cx, 80, 28, EpdDisplay::kBlack);
  epd.fillCircle(cx, 80, 25, EpdDisplay::kWhite);
  epd.fillRect(cx - 29, 51, 59, 30, EpdDisplay::kWhite);
  epd.fillCircle(cx - 22, 70, 7, EpdDisplay::kBlack);
  epd.fillCircle(cx + 22, 70, 7, EpdDisplay::kBlack);
}

void drawStatus() {
  char line[32];
  const uint32_t s = millis() / 1000;
  snprintf(line, sizeof(line), "up %02lu:%02lu  %.2fV", s / 60, s % 60, readBatteryMv() / 1000.0f);
  epd.fillRect(0, 170, EpdDisplay::kSize, 30, EpdDisplay::kWhite);
  epd.setFont(&FreeSans9pt7b);
  drawCentered(line, 190);
}

void drawScreen() {
  epd.fillScreen(EpdDisplay::kWhite);
  epd.setTextColor(EpdDisplay::kBlack);
  drawFace();
  epd.setFont(&FreeSansBold12pt7b);
  drawCentered("Hello, Dotty!", 150);
  drawStatus();
}

void logChipInfo() {
  Serial.printf("\n=== Dotty ===\n");
  Serial.printf("Chip: %s rev %d, %d cores @ %lu MHz\n", ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("Flash: %lu MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("PSRAM: %lu KB (free %lu KB)\n", ESP.getPsramSize() / 1024, ESP.getFreePsram() / 1024);
  Serial.printf("Heap free: %lu KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf("Battery: %lu mV\n", readBatteryMv());
}

// On battery the board only stays on while PIN_VBAT_PWR is held high.
// Holding PWR for 2 s releases it. The button must first be let go after
// the long press that powered the board on.
void checkPowerButton() {
  static bool armed = false;
  static uint32_t pressedAt = 0;

  const bool pressed = digitalRead(PIN_BTN_PWR) == LOW;
  if (!pressed) {
    armed = true;
    pressedAt = 0;
    return;
  }
  if (!armed) return;
  if (pressedAt == 0) pressedAt = millis();
  if (millis() - pressedAt < kPowerOffHoldMs) return;

  Serial.println("Power off");
  epd.fillRect(0, 170, EpdDisplay::kSize, 30, EpdDisplay::kWhite);
  epd.setFont(&FreeSans9pt7b);
  drawCentered("sleeping...", 190);
  epd.refreshPartial();
  epd.powerOff();
  digitalWrite(PIN_VBAT_PWR, LOW);
  delay(1000);  // still running here when powered over USB
  armed = false;
}

}  // namespace

void setup() {
  // Latch power first so the board survives releasing PWR on battery.
  pinMode(PIN_VBAT_PWR, OUTPUT);
  digitalWrite(PIN_VBAT_PWR, HIGH);
  pinMode(PIN_BTN_PWR, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Serial.begin(115200);
  delay(1500);  // give the USB serial monitor time to attach
  logChipInfo();

  epd.begin();
  drawScreen();
  const uint32_t start = millis();
  epd.refreshFull();
  Serial.printf("Full refresh: %lu ms\n", millis() - start);
  digitalWrite(PIN_LED, HIGH);
}

void loop() {
  static uint32_t lastStatus = 0;
  static int partialCount = 0;

  checkPowerButton();

  if (millis() - lastStatus >= kStatusIntervalMs) {
    lastStatus = millis();
    drawStatus();
    const uint32_t start = millis();
    if (++partialCount >= kPartialRefreshesPerFull) {
      partialCount = 0;
      epd.refreshFull();
    } else {
      epd.refreshPartial();
    }
    Serial.printf("Status refresh: %lu ms\n", millis() - start);
  }
  delay(20);
}
