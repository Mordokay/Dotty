#pragma once

#include <Arduino.h>

#include <string>
#include <vector>

#include "recording_store.h"

// The voice-note recorder (spec: docs/superpowers/specs/2026-10-24-companion-recordings-design.md): the
// companion's own 16 kHz capture → a WAV in the companion's recordings/ folder through a writer task
// (a pause writes the header's length, so a power cut keeps everything up to the last pause), and, while
// the phone is there, ADPCM frames to it (rec.* events, 0x1F 0x02 frames) for the live transcript and its
// copy. Undo cuts the file back to where the last part began. Tape's recorder is untouched (its own
// capture at 32 kHz); this one shares nothing but the idea.
namespace note {

bool begin(const std::string &companion, const std::string &title);  // a new recording (false: no card)
void add(const int16_t *pcm, size_t n);  // while a part is being recorded
void partStart();
void partEnd();                          // its header length written: safe from a power cut
uint32_t undo();                         // ms removed (0: nothing)
std::string finish(bool keep);           // closes it (discard deletes it); its name ("" if none)
bool active();
bool streaming();                        // the phone is getting it live
const RecordingMeta &current();

// Every companion's recordings: headers a power cut left stale (boot).
void repairAll();
// One companion's recordings, newest first; their files.
std::vector<RecordingMeta> list(const std::string &companion);
String path(const std::string &companion, const std::string &name, const char *ext);  // "wav", "json", "txt"
bool saveMeta(const std::string &companion, const RecordingMeta &m);
bool remove(const std::string &companion, const std::string &name);

}  // namespace note
