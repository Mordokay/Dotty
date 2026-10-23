#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "script_writer.h"

// Looking back at past chats (spec: the user, 2026-10-23 — "what was I saying yesterday morning?",
// "when I talked about bananas the other day, what was I saying?"). The model calls recall
// {question, from?, to?, topic?}; Dotty reads those days of the companion's own history, keeps what
// mentions the topic (with the exchanges around it), and a text model (kWriterModel) says what was
// said and when; that goes back to Live as the tool's result. Plus a summary of every chat (written
// in the end-of-chat review), the last ten in the instruction. Plain C++ (host-tested).

constexpr size_t kRecallMaxChars = 60000;  // the newest history sent per recall (~15k tokens)
constexpr int kRecallDays = 30;            // no dates: the last 30 days
constexpr int kRecallMaxSpan = 31;         // a longer range keeps its newest 31 days

std::string nowText(int64_t localEpoch);  // "Friday 23 October 2026, 14:05 (2026-10-23)"
// Days to read, oldest first ("YYYY-MM-DD"); empty or unreadable dates fall back to the last 30.
std::vector<std::string> recallDays(const std::string &from, const std::string &to, int64_t nowLocal);
// A day's history (.jsonl) as "Thu 22 Oct 09:14 - User: … / Mia: …", one per exchange.
std::vector<std::string> exchanges(const std::string &dayLog, const std::string &name);
// Exchanges mentioning any of the topic's words (and the one before and after); none = all.
std::vector<std::string> byTopic(const std::vector<std::string> &all, const std::string &topic);
std::string historyBlock(const std::vector<std::string> &exchanges);  // newest kept, ≤ kRecallMaxChars
std::string recallRequest(const std::string &name, const std::string &question, int64_t nowLocal,
                          const std::string &history);
std::string recallResult(const Written &found);  // the tool result Live talks about

std::string summaryLine(int64_t localEpoch, const std::string &text);       // a line of summaries.jsonl
std::string summariesSection(const std::string &summariesJsonl, int max);  // "" when none
