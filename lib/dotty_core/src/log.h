#pragma once

#include <Arduino.h>

// Non-blocking logger. Lines go into a RAM ring buffer and a background task
// forwards them to USB serial only when the host has room, so logging never
// stalls the firmware (a plain Serial.print blocks for up to 2 s when the cable
// is plugged in but no serial monitor is reading).
//
// The ring always keeps the most recent 16 KB. Send 'd' over serial to replay
// that history (lines sent while no monitor was open are otherwise lost).
//
//   LOGI("player", "volume %u%%", vol);
//   -> [   12.345] I player: volume 80%
namespace dlog {

void begin();
void write(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

}  // namespace dlog

#define LOGE(tag, ...) dlog::write('E', tag, __VA_ARGS__)
#define LOGW(tag, ...) dlog::write('W', tag, __VA_ARGS__)
#define LOGI(tag, ...) dlog::write('I', tag, __VA_ARGS__)
