#pragma once

#include <Arduino.h>

#include <vector>

#include "audio_player.h"

// The tape: microphone → WAV file on the SD card (data/recordings/), 16-bit mono. Like a
// tape deck, record() and pause() can alternate any number of times on the same tape;
// finish() closes it. A reader task pulls samples from I2S into a PSRAM buffer and a
// writer task moves them to the card, so card hiccups don't drop sound.
namespace tape {

constexpr uint32_t kRate = 32000;  // see CLAUDE.md: the mic has content above 8 kHz
constexpr uint8_t kMicGainDb = 30;
// +2.5 dB (x1.33) digital: a normal speaking voice sounded a bit low (the user asked for
// 20-30 % more; projecting was already right, so not the next 6 dB analog step).
constexpr float kMicBoostDb = 2.5f;

enum class State { Idle, Recording, Paused };

struct Recording {
  String name;  // file name in recordings/, e.g. "20261006-091412.wav"
  size_t size = 0;
  uint32_t durationMs = 0;
  time_t added = 0;  // last change on the card (local time)
};

bool begin(AudioPlayer &player);  // after storage::begin()

State state();
bool record();  // starts a new tape or continues the paused one
void pause();
String finish();  // closes the tape; returns its name ("" if nothing was recorded)
// Paused: removes the last part (everything since the last record()); the tape stays open,
// maybe empty. Returns the milliseconds removed, 0 if there was nothing to undo.
uint32_t undo();
uint32_t lastPartMs();  // length of what undo() would remove
int parts();            // parts on the open tape
void discard();         // throws the open tape away
uint32_t elapsedMs();
uint8_t level();  // 0..100, loudness of the last fraction of a second
String current();  // the open tape's name, "" when idle

std::vector<Recording> list();  // newest first
String path(const String &name);
bool remove(const String &name);
bool rename(const String &name, const String &title);  // keeps ".wav"; false if taken

// "Mon 6 Oct 09:14" for "20261006-091412.wav"; the title for renamed ones.
String displayName(const String &name);

}  // namespace tape
