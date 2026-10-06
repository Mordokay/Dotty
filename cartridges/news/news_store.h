#pragma once

#include <Arduino.h>
#include <time.h>

#include <functional>
#include <vector>

// The news pool: short stories for the topics the user follows, fetched over Wi-Fi every
// 30 minutes and kept on the SD card, so Dotty can be read offline for hours.
//
// A topic comes from one of three sources:
//   - an outlet's feed (feeds(): BBC, NYT, NBC, Bloomberg, …): headline + a short summary,
//     fresh all day (key "s:<feed id>");
//   - a Kagi News category (kite.kagi.com): a daily briefing of the day's 12 big stories, each
//     clustered from dozens of outlets with an AI summary; fetched only when Kagi publishes a
//     new edition (key "c:<file>");
//   - a keyword searched on Google News over the last 2 days: headlines only (key "k:<query>").
// Summaries are cut to their first sentence(s), about 250 characters at most. Each topic keeps
// its 10 best stories of the last 48 hours. Files in the cartridge's data folder:
//   topics.json   [{section | kagi | query, name, star}] in the user's order
//   stories.tsv   topic key \t published (UTC epoch) \t feed position \t source \t title \t summary
// "Best" = a score from the feed's own order (outlets list by editorial importance, Kagi by
// size, Google by relevance), the story's age, and a boost when several topics carry it.
namespace news {

constexpr int kMaxTopics = 12;

struct Feed {
  const char *id, *outlet, *section, *url;
  String name() const { return section[0] ? String(outlet) + " " + section : String(outlet); }
};
const std::vector<Feed> &feeds();  // the outlets' feeds a topic can be

struct Topic {
  String section;  // an outlet feed id ("section": the first topics were BBC sections)
  String kagi;     // a Kagi News category file, e.g. "formula_1.json"
  String query;    // a Google News keyword
  String name;     // shown on Dotty and in the app
  bool star = false;
  String key() const {
    return section.length() ? "s:" + section : kagi.length() ? "c:" + kagi : "k:" + query;
  }
};

struct Story {
  const char *topic = "";  // the topic's key
  uint32_t published = 0;  // UTC epoch
  uint16_t position = 0;   // place in its feed (0 = the feed's top story)
  const char *source = "", *title = "", *summary = "";
  float score = 0;         // lower = more important (see header comment)
  bool fitsLock = false;   // headline short enough for the lock screen
};

bool begin();  // loads topics and the pool from the card (after storage::begin)

// Topics (changed from the app). Changing them schedules a fetch.
const std::vector<Topic> &topics();
bool addTopic(const Topic &topic, String &error);
bool removeTopic(const String &key);
bool starTopic(const String &key, bool on);
bool moveTopic(int from, int to);

// Stories, best first: one topic's, or (key "") the starred topics' together without
// duplicates. Pointers stay valid until the pool reloads (takeFetchFinished).
std::vector<const Story *> stories(const String &topicKey);
size_t storyCount();
// The lock screen's headline for this 5-minute slot: the starred topics' best stories in
// turn. nullptr if none fits.
const Story *lockStory(const tm &now);
void measureLockFit(std::function<bool(const Story &)> fits);

// "2h ago" for a story, from Dotty's local clock and the phone's UTC offset.
String age(const Story &story);

// Fetching, in the background.
struct FetchState {
  bool running = false;
  int done = 0, total = 0;  // topics
  String error;             // the last run's problem ("" if it worked)
  uint32_t fetchedAt = 0;   // local time of the last good fetch (0 = never)
};
bool fetchDue();     // every 30 minutes, sooner after a topic change; retries after 10 min
void startFetch();   // no-op while one is running
FetchState fetchState();
// Call from the main loop: true once after a fetch finished (the pool was reloaded).
bool takeFetchFinished();

}  // namespace news
