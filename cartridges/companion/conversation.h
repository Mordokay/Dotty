#pragma once

#include <stdint.h>

#include <string>
#include <vector>

// The open conversation (user, 2026-10-24: unlocking again should carry on the chat, like Claude Code
// carries on a session; "let's start a new conversation" / "switch tape" / "change topic" starts
// fresh). A conversation runs until 6 hours of silence or a new_conversation call. Every session's
// instruction carries it: a summary of its older part plus the latest exchanges word for word, read
// from the companion's history. Once the word-for-word part passes 2,000 words, the end-of-chat review
// folds its older half into the summary (the text model). Card: companions/<id>/conversation.json
// {start, from, summary}. Plain C++ (host-tested); times are local epoch seconds.
//
// The history is read as one text (the days' .jsonl joined) and indexed: a turn is its time, where its
// line is and its words — never a string per exchange (small strings live in internal RAM, which a
// session can't spare; recall.h does the same).

constexpr int64_t kConversationGap = 6 * 3600;  // this much silence starts a new conversation
constexpr int kFoldWords = 2000;                // past this, the review folds the older half
constexpr int kSectionWords = 3000;             // the instruction never carries more (a fold that failed)

struct Conversation {
  int64_t start = 0;  // when it began (0 = nothing stored)
  int64_t from = 0;   // the first exchange kept word for word; the summary covers [start, from)
  std::string summary;
};

struct TurnRef {
  int64_t t = 0;
  uint32_t at = 0, len = 0;  // its line in the log
  int words = 0;
};

std::string conversationJson(const Conversation &c);
bool parseConversation(const std::string &json, Conversation &out);
std::vector<TurnRef> indexTurns(const std::string &log);  // history .jsonl text, oldest first
// Where the current run of exchanges began (no 6-hour gap since); `now` when it has gone quiet that long.
int64_t runStart(const std::vector<TurnRef> &turns, int64_t now);
// The stored conversation if it began inside the current run, else a new one starting with the run.
Conversation current(const Conversation &stored, int64_t runStarted);
int countWords(const std::string &text);
// For the instruction ("" when nothing has been said yet): the summary, then the latest turns
// (≥ c.from, the newest kSectionWords), and how long ago the last one was.
std::string conversationSection(const Conversation &c, const std::string &log, const std::vector<TurnRef> &turns,
                                const std::string &name, int64_t now);

struct Fold {
  bool due = false;
  size_t first = 0, end = 0;  // the turns folded into the summary: [first, end)
  int64_t newFrom = 0;        // the first turn kept word for word after it
};
Fold foldPlan(const std::vector<TurnRef> &turns, int64_t from);
std::string foldRequest(const std::string &name, const std::string &summary, const std::string &log,
                        const std::vector<TurnRef> &turns, const Fold &fold);
