#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "companion_store.h"
#include "moment_store.h"

// The words of a moment (spec: "Making the lines"): the text model (kWriterModel) writes the line in the
// companion's voice — one topic, or every topic due at one minute together — Live reads it word for word
// for the recording, and once a day the text model plans the companion's own lines. Plain C++
// (host-tested).

struct Topic {
  std::string kind, what, style;  // kind: reminder, alarm, spontaneous, timer
};

// How Live should deliver it: sung → a simple melody; whispered, excited, calm; spoken → "".
std::string styleNote(const std::string &style);
// generateContent body: the companion's character + memories, the topics, when it will play, at most
// `words` words and as short as the message allows.
std::string lineRequest(const Profile &p, const std::string &memories, const std::vector<Topic> &topics, int64_t at,
                        int words);
// The recording turn: read it aloud exactly, word for word, in that style (more chunks: continueReadingNote).
std::string readLineNote(const std::string &text, const std::string &style);
// A recording session's system instruction (no chat: lines only).
std::string recordingInstruction(const std::string &persona);

// The day's spontaneous lines: generateContent body asking for JSON {"lines": [{at: "HH:MM", what, line,
// style}]} on `day` (its midnight, local epoch), inside the allowed hours, outside quiet windows, ≥ 2 h
// apart, ≥ 30 minutes from the times in `taken`.
std::string planRequest(const Profile &p, const std::string &memories, const std::string &summaries, int64_t day,
                        const MomentRules &rules, int count, const std::vector<int64_t> &taken);
struct PlannedLine {
  int64_t at = 0;
  std::string what, line, style;
};
// The plan checked (Dotty doesn't trust it): lines breaking a rule, over 150 words or over `count` are
// dropped; a bad style becomes spoken.
std::vector<PlannedLine> parsePlan(int httpStatus, const std::string &body, int64_t day, const MomentRules &rules,
                                   const std::vector<int64_t> &taken, int count);
// Once a day, from 30 minutes before the allowed hours start (lastPlannedDay: the midnight of the day
// last planned, 0 = never). Never with perDay 0.
bool planDue(int64_t now, int64_t lastPlannedDay, const MomentRules &rules);

// For the companion's instruction: how many moments the user has set and the next three ("" for none).
// Its own spontaneous lines aren't told: they're surprises.
std::string momentsSection(const MomentList &list, int64_t now);
// list_moments' answer: every upcoming reminder and alarm, "id 3: Today 18:00, reminder: cook dinner (sung)".
std::string listMomentsText(const MomentList &list, int64_t now);
// For the chat that may follow a moment: what the companion said by itself a moment ago.
std::string firedNote(const std::string &line, const std::string &kind);
// "Today 18:00", "Tomorrow 07:30", "Mon 26 Oct 09:00"; repeating: "Daily 07:30", "Weekdays 07:30",
// "Mon, Wed 08:00".
std::string whenText(const Moment &m, int64_t now);
