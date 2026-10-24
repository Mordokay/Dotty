#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "piece_buffer.h"

// A recording streamed to the iPhone and its text coming back (spec: docs/superpowers/specs/2026-10-24-
// companion-recordings-design.md, "The iPhone side"). Dotty → phone: events rec.begin {id, companion,
// title}, rec.part {id, n}, rec.undo {id}, rec.end {id, how: done|discard}, and audio notifications
// 0x1F 0x02 seq + an ADPCM frame (0x1F 0x01 is the chat's voice). Phone → Dotty: companion.recording.text
// {id, companion, transcript, draft?, isDraft, summary, points[], title?} — or {…, pieces: true} with the
// long fields (transcript, draft) following as companion.recording.piece {id, field, offset, total, text}
// and a closing {id, done: true}. Matched by id, so texts for older recordings land where they belong.
// Plain C++ (host-tested).

std::string recBeginEvent(const std::string &id, const std::string &companion, const std::string &title);
std::string recPartEvent(const std::string &id, int n);
std::string recUndoEvent(const std::string &id);
std::string recEndEvent(const std::string &id, bool done);
size_t recFrameHeader(uint8_t *out, uint8_t seq);  // 0x1F 0x02 seq; returns 3

struct RecText {
  std::string id, companion, transcript, draft, summary, title;
  std::vector<std::string> points;
  bool isDraft = false;
};

class RecTextAssembler {
 public:
  static constexpr size_t kMaxText = 64 * 1024;  // 30 minutes are ~27 KB of words
  // A text command: complete at once (inline transcript) or after its pieces (`done`). True with `complete`
  // filled when a recording's text is whole.
  bool command(JsonObjectConst args, RecText &complete);
  bool piece(JsonObjectConst args);  // false: out of order, unknown id or field, too long

 private:
  struct Pending {
    RecText text;
    PieceBuffer transcript, draft;
    bool hasDraft = false;
  };
  std::map<std::string, Pending> pending_;
};
