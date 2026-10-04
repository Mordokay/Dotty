#include "power.h"

#include <driver/gpio.h>
#include <esp_sleep.h>

#include "board_pins.h"
#include "log.h"

namespace power {
namespace {

uint32_t locks = 0;

constexpr gpio_num_t kHeldDuringSleep[] = {
    static_cast<gpio_num_t>(PIN_EPD_PWR),
    static_cast<gpio_num_t>(PIN_AUDIO_PWR),
    static_cast<gpio_num_t>(PIN_PA_EN),
};

}  // namespace

void begin() {
  pinMode(PIN_VBAT_PWR, OUTPUT);
  digitalWrite(PIN_VBAT_PWR, HIGH);
  gpio_hold_en(static_cast<gpio_num_t>(PIN_VBAT_PWR));  // must never glitch, even in sleep
  pinMode(PIN_AUDIO_PWR, OUTPUT);
}

void setWakeLock(uint32_t lock, bool held) {
  const uint32_t before = locks;
  locks = held ? locks | lock : locks & ~lock;
  if (locks != before) LOGI("power", "wake locks 0x%02lx", locks);
}

uint32_t wakeLocks() {
  return locks;
}

bool usbHostConnected() {
  return HWCDC::isPlugged();
}

void setAudioRail(bool on) {
  digitalWrite(PIN_AUDIO_PWR, on ? LOW : HIGH);
}

bool lightSleep(uint32_t ms) {
  for (gpio_num_t pin : kHeldDuringSleep) gpio_hold_en(pin);
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(ms) * 1000);
  gpio_wakeup_enable(static_cast<gpio_num_t>(PIN_BTN_PWR), GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  esp_light_sleep_start();

  gpio_wakeup_disable(static_cast<gpio_num_t>(PIN_BTN_PWR));
  for (gpio_num_t pin : kHeldDuringSleep) gpio_hold_dis(pin);
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
}

void shutdown() {
  LOGI("power", "shutting down");
  gpio_hold_dis(static_cast<gpio_num_t>(PIN_VBAT_PWR));
  digitalWrite(PIN_VBAT_PWR, LOW);

  // Still here: powered over USB. Wait for PWR to be let go (it is held right
  // now and would wake us instantly), then sleep until the next press.
  while (digitalRead(PIN_BTN_PWR) == LOW) delay(10);
  delay(500);
  setAudioRail(false);
  esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(PIN_BTN_PWR), 0);
  esp_deep_sleep_start();
}

}  // namespace power
