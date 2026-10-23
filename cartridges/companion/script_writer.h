#pragma once

#include <string>

#include "companion_store.h"

// Long answers (a story, a guided exercise, a deep explanation): Gemini Live keeps its own spoken
// replies to ~25-30 s however it's asked, but reads a given text word for word in one turn (157 of
// 157 words, 57.6 s). So Live calls write_long, a text model writes the answer in the companion's
// voice (generateContent over a second TLS connection, which fits since mbedTLS's buffers are in
// PSRAM), and Live reads it aloud. Plain C++ (host-tested): the request, the answer, the notes.

constexpr const char *kWriterModel = "gemini-flash-lite-latest";  // 138 words in 2.7 s on the device

int writerWords(int seconds);  // ~2.5 words a second, 30-60 s (Dotty keeps a minute of audio)
// The generateContent body: the companion's character and memories, the length, what to write.
std::string writerRequest(const Profile &p, const std::string &memories, const std::string &what, int seconds);

struct Written {
  bool ok = false;
  std::string text, error;
  int tokens = 0;
};
Written parseWritten(int httpStatus, const std::string &body);

// What Dotty tells Live (as a typed turn; the user never sees it).
std::string readAloudNote(const std::string &text);
std::string writerFailedNote();
