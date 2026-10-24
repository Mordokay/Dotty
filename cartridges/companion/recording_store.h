#pragma once

#include <stdint.h>

#include <string>
#include <vector>

// Voice notes recorded through a companion (spec: docs/superpowers/specs/2026-10-24-companion-
// recordings-design.md): data/companions/<id>/recordings/<YYYYMMDD-HHMMSS>.wav + .json (these details)
// + .txt (the transcript). Finding one by what the user says, the summaries the companion knows, the
// text recall reads, and the repair of a WAV a power cut left with a stale header. Plain C++
// (host-tested). Times are local epoch seconds.

struct RecordingMeta {
  std::string name;  // "20261024-153012" (no extension)
  std::string title;
  int64_t created = 0;
  uint32_t ms = 0;
  int parts = 0;
  bool synced = false;   // the iPhone has the whole recording
  bool isDraft = false;  // the transcript is Apple's draft (the final one hasn't come yet)
  int64_t textAt = 0;    // when the transcript arrived (0 = none yet)
  bool open = false;     // still being recorded (a power cut leaves it so: boot repairs only these)
  std::string summary;
  std::vector<std::string> points;
};

std::string recordingName(int64_t localEpoch);  // "20261024-153012"
bool parseRecordingMeta(const std::string &json, RecordingMeta &out);
std::string recordingMetaJson(const RecordingMeta &m);
// What the user said ("the last one", "yesterday", "2026-10-24", a title or summary word) → the index in
// the newest-first list, -1 for none.
int matchRecording(const std::vector<RecordingMeta> &newestFirst, const std::string &query, int64_t now);
// For the instruction: the newest `max` recordings' date, title and summary ("" when there are none).
std::string recordingsSection(const std::vector<RecordingMeta> &newestFirst, int max);
// For recall: "Fri 23 Oct 18:00 - Recording 'Bananas in space': <transcript>".
std::string transcriptBlock(const RecordingMeta &m, const std::string &transcript);

// The recordings list on Dotty: a row's title ("Voice note" when untitled) and "Sat 24 Oct - 1:17".
std::string recordingRowTitle(const RecordingMeta &m);
std::string recordingRowDetail(const RecordingMeta &m);

struct HeaderFix {
  bool needed = false;
  uint32_t dataBytes = 0;  // the samples actually in the file (whole ones)
};
HeaderFix headerFix(uint32_t fileBytes, uint32_t headerDataBytes);
