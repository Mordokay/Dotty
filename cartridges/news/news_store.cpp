#include "news_store.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD_MMC.h>

#include <algorithm>

#include "log.h"
#include "net.h"
#include "shell.h"
#include "storage.h"

namespace news {
namespace {

constexpr int kScanItems = 20;          // read this many from each feed (then stop the download)
constexpr int kPerTopic = 10;           // keep this many stories per topic
constexpr uint32_t kMaxAge = 48 * 3600; // older stories are dropped
constexpr uint32_t kRefreshMs = 30 * 60 * 1000;
constexpr uint32_t kRetryMs = 10 * 60 * 1000;
constexpr uint32_t kAfterChangeMs = 5000;  // a topic change fetches soon (edits come in bursts)

constexpr size_t kMaxSummary = 250;    // summaries are cut to whole sentences within this
constexpr const char *kKagiIndex = "https://kite.kagi.com/kite.json";

// Outlets with working feeds (checked 2026-10-06: fresh, a summary on every story). CNN and
// WSJ feeds stopped updating, Reuters and AP have none: they're reachable as keywords
// (Google News, e.g. "site:wsj.com"), headlines only. The BBC ids stay short (the first topics).
#define BBC "https://feeds.bbci.co.uk/news/"
#define NYT "https://rss.nytimes.com/services/xml/rss/nyt/"
const std::vector<Feed> kFeeds = {
    {"top", "BBC", "Top stories", BBC "rss.xml"},
    {"world", "BBC", "World", BBC "world/rss.xml"},
    {"uk", "BBC", "UK", BBC "uk/rss.xml"},
    {"business", "BBC", "Business", BBC "business/rss.xml"},
    {"politics", "BBC", "Politics", BBC "politics/rss.xml"},
    {"tech", "BBC", "Tech", BBC "technology/rss.xml"},
    {"science", "BBC", "Science", BBC "science_and_environment/rss.xml"},
    {"health", "BBC", "Health", BBC "health/rss.xml"},
    {"culture", "BBC", "Culture", BBC "entertainment_and_arts/rss.xml"},
    {"sport", "BBC", "Sport", "https://feeds.bbci.co.uk/sport/rss.xml"},
    {"nyt-home", "NYT", "Top stories", NYT "HomePage.xml"},
    {"nyt-world", "NYT", "World", NYT "World.xml"},
    {"nyt-us", "NYT", "U.S.", NYT "US.xml"},
    {"nyt-politics", "NYT", "Politics", NYT "Politics.xml"},
    {"nyt-business", "NYT", "Business", NYT "Business.xml"},
    {"nyt-tech", "NYT", "Tech", NYT "Technology.xml"},
    {"nyt-science", "NYT", "Science", NYT "Science.xml"},
    {"nyt-health", "NYT", "Health", NYT "Health.xml"},
    {"nyt-arts", "NYT", "Arts", NYT "Arts.xml"},
    {"wapo-politics", "Washington Post", "Politics", "https://feeds.washingtonpost.com/rss/politics"},
    {"wapo-world", "Washington Post", "World", "https://feeds.washingtonpost.com/rss/world"},
    {"wapo-business", "Washington Post", "Business", "https://feeds.washingtonpost.com/rss/business"},
    {"nbc", "NBC News", "", "https://feeds.nbcnews.com/nbcnews/public/news"},
    {"abc", "ABC News", "Top stories", "https://abcnews.go.com/abcnews/topstories"},
    {"abc-world", "ABC News", "World", "https://abcnews.go.com/abcnews/internationalheadlines"},
    {"cbs", "CBS News", "Latest", "https://www.cbsnews.com/latest/rss/main"},
    {"cbs-world", "CBS News", "World", "https://www.cbsnews.com/latest/rss/world"},
    {"npr", "NPR", "News", "https://feeds.npr.org/1001/rss.xml"},
    {"npr-world", "NPR", "World", "https://feeds.npr.org/1004/rss.xml"},
    {"npr-politics", "NPR", "Politics", "https://feeds.npr.org/1014/rss.xml"},
    {"fox", "Fox News", "Latest", "https://moxie.foxnews.com/google-publisher/latest.xml"},
    {"bloomberg", "Bloomberg", "Markets", "https://feeds.bloomberg.com/markets/news.rss"},
    {"bloomberg-tech", "Bloomberg", "Tech", "https://feeds.bloomberg.com/technology/news.rss"},
    {"bloomberg-politics", "Bloomberg", "Politics", "https://feeds.bloomberg.com/politics/news.rss"},
    {"ft", "FT", "Top stories", "https://www.ft.com/rss/home"},
    {"ft-world", "FT", "World", "https://www.ft.com/world?format=rss"},
    {"guardian-world", "Guardian", "World", "https://www.theguardian.com/world/rss"},
    {"guardian-uk", "Guardian", "UK", "https://www.theguardian.com/uk/rss"},
    {"guardian-us", "Guardian", "US", "https://www.theguardian.com/us-news/rss"},
    {"guardian-business", "Guardian", "Business", "https://www.theguardian.com/business/rss"},
    {"guardian-tech", "Guardian", "Tech", "https://www.theguardian.com/technology/rss"},
    {"guardian-science", "Guardian", "Science", "https://www.theguardian.com/science/rss"},
    {"aljazeera", "Al Jazeera", "", "https://www.aljazeera.com/xml/rss/all.xml"},
    {"sky", "Sky News", "", "https://feeds.skynews.com/feeds/rss/home.xml"},
    {"euronews", "Euronews", "", "https://www.euronews.com/rss"},
    {"techcrunch", "TechCrunch", "", "https://techcrunch.com/feed/"},
    {"ars", "Ars Technica", "", "https://feeds.arstechnica.com/arstechnica/index"},
    {"verge", "The Verge", "", "https://www.theverge.com/rss/index.xml"},
};
#undef BBC
#undef NYT

const Feed *findFeed(const String &id) {
  for (const Feed &f : kFeeds) {
    if (id == f.id) return &f;
  }
  return nullptr;
}

std::vector<Topic> topicList;
std::vector<Story> pool;   // texts point into `text`
char *text = nullptr;      // stories.tsv in PSRAM

// Fetch state, shared with the fetch task.
portMUX_TYPE fetchLock = portMUX_INITIALIZER_UNLOCKED;
volatile bool fetchRunning = false;
volatile bool fetchFinished = false;
volatile int fetchDone = 0, fetchTotal = 0;
char fetchError[64] = "";
uint32_t fetchedAt = 0;
uint32_t lastAttempt = 0;
bool attempted = false;
uint32_t kagiEdition = 0;  // the Kagi edition (its index timestamp) the pool holds
uint32_t changedAt = 0;  // millis() of the last topic change (0 = none pending)
// What the task works from: a copy of the topics, and each topic's current stories (kept
// when its feed can't be read this time).
std::vector<Topic> fetchTopics;
std::vector<String> previousLines;

String path(const char *name) {
  return storage::myDataDir() + "/" + name;
}

// Seconds to add to UTC for local time (the phone sends it with the clock; 0 until then).
int32_t utcOffset() {
  int32_t offset = 0;
  shell::utcOffset(offset);
  return offset;
}

uint32_t nowUtc() {
  return static_cast<uint32_t>(time(nullptr) - utcOffset());
}

// ---------- RSS ----------

String feedUrl(const Topic &t) {
  if (t.kagi.length()) return "https://kite.kagi.com/" + t.kagi;
  if (t.section.isEmpty()) {
    String q;
    for (const char *p = t.query.c_str(); *p; p++) {
      const uint8_t c = *p;
      if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') q += static_cast<char>(c);
      else if (c == ' ') q += "%20";
      else q += "%" + String(c < 16 ? "0" : "") + String(c, HEX);
    }
    return "https://news.google.com/rss/search?q=" + q + "%20when%3A2d&hl=en-GB&gl=GB&ceid=GB%3Aen";
  }
  const Feed *feed = findFeed(t.section);
  return feed ? String(feed->url) : String();
}

void appendUtf8(String &out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// XML text → one clean line: CDATA unwrapped, entities decoded, tags/tabs/newlines gone.
String cleanText(String s) {
  s.trim();
  if (s.startsWith("<![CDATA[") && s.endsWith("]]>")) s = s.substring(9, s.length() - 3);
  String out;
  bool inTag = false;
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '<') {
      inTag = true;
      continue;
    }
    if (inTag) {
      if (c == '>') {
        inTag = false;
        out += ' ';  // "…Saturday</p><p>Kenya" must not become "SaturdayKenya"
      }
      continue;
    }
    if (c == '&') {
      const int semi = s.indexOf(';', i);
      if (semi > 0 && semi - static_cast<int>(i) <= 8) {
        const String name = s.substring(i + 1, semi);
        uint32_t cp = 0;
        if (name == "amp") cp = '&';
        else if (name == "lt") cp = '<';
        else if (name == "gt") cp = '>';
        else if (name == "quot") cp = '"';
        else if (name == "apos") cp = '\'';
        else if (name == "nbsp") cp = ' ';
        else if (name.startsWith("#x") || name.startsWith("#X")) cp = strtoul(name.c_str() + 2, nullptr, 16);
        else if (name.startsWith("#")) cp = strtoul(name.c_str() + 1, nullptr, 10);
        if (cp) {
          appendUtf8(out, cp);
          i = semi;
          continue;
        }
      }
    }
    out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
  }
  while (out.indexOf("  ") >= 0) out.replace("  ", " ");
  for (const char *p : {" .", " ,", " ;", " :", " !", " ?"}) out.replace(p, p + 1);
  out.trim();
  return out;
}

// Whole sentences from the start, at least ~80 characters when there are more, at most
// kMaxSummary ("Oct." or "U.S." doesn't end a sentence); a word-cut "..." as the last resort.
// Kagi's source marks ("[bbc.co.uk#1]") go first.
String shortSummary(String s) {
  for (int open; (open = s.indexOf('[')) >= 0;) {
    const int close = s.indexOf(']', open);
    if (close < 0) break;
    s.remove(open, close - open + 1);
  }
  s = cleanText(s);
  if (s.length() <= kMaxSummary) return s;
  int cut = -1;
  for (size_t i = 1; i + 2 < s.length() && i < kMaxSummary; i++) {
    const char c = s[i];
    if ((c != '.' && c != '!' && c != '?') || s[i + 1] != ' ') continue;
    const char next = s[i + 2];
    if (!isupper(static_cast<uint8_t>(next)) && next != '"' && static_cast<uint8_t>(next) < 0x80) continue;
    if (c == '.') {  // an abbreviation? (a short capitalised word, or one with dots: "U.S.")
      int start = i;
      while (start > 0 && s[start - 1] != ' ') start--;
      const String word = s.substring(start, i);
      if (word.indexOf('.') >= 0 || (word.length() <= 3 && word.length() && isupper(static_cast<uint8_t>(word[0])))) continue;
    }
    cut = i + 1;
    if (cut >= 80) break;
  }
  if (cut > 0) return s.substring(0, cut);
  const int space = s.lastIndexOf(' ', kMaxSummary - 3);
  return s.substring(0, space > 0 ? space : kMaxSummary - 3) + "...";
}

// The text inside <tag ...>…</tag> ("" if missing).
String tagText(const String &item, const char *tag) {
  const String open = String("<") + tag;
  int start = item.indexOf(open);
  while (start >= 0) {
    const char after = item[start + open.length()];
    if (after == '>' || after == ' ') break;
    start = item.indexOf(open, start + 1);
  }
  if (start < 0) return "";
  const int contentStart = item.indexOf('>', start);
  const int end = item.indexOf(String("</") + tag + ">", contentStart);
  if (contentStart < 0 || end < 0) return "";
  return cleanText(item.substring(contentStart + 1, end));
}

int64_t daysFromCivil(int y, unsigned m, unsigned d) {  // Howard Hinnant's algorithm
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// "Tue, 06 Oct 2026 17:50:19 GMT" (or +0100) → UTC epoch; 0 if unreadable.
uint32_t parseDate(const String &s) {
  static const char *kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int day, year, h, m, sec;
  char month[4] = {}, zone[8] = {};
  const char *p = strchr(s.c_str(), ',');
  p = p ? p + 1 : s.c_str();
  if (sscanf(p, " %d %3s %d %d:%d:%d %7s", &day, month, &year, &h, &m, &sec, zone) < 6) return 0;
  const char *found = strstr(kMonths, month);
  if (!found) return 0;
  int64_t t = daysFromCivil(year, (found - kMonths) / 3 + 1, day) * 86400 + h * 3600 + m * 60 + sec;
  if (zone[0] == '+' || zone[0] == '-') {
    const int hhmm = atoi(zone + 1);
    const int offset = (hhmm / 100) * 3600 + (hhmm % 100) * 60;
    t -= zone[0] == '+' ? offset : -offset;
  }
  return t > 0 ? static_cast<uint32_t>(t) : 0;
}

struct Item {
  uint32_t published;
  uint16_t position;
  String source, title, summary;
};

// "2026-10-06T17:50:19Z" / "+01:00" (Atom) → UTC epoch; 0 if unreadable.
uint32_t parseIsoDate(const String &s) {
  int y, mo, d, h, mi, sec;
  if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
  int64_t t = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
  const int zone = std::max(s.lastIndexOf('+'), s.lastIndexOf('-'));
  if (zone > 10) {
    const int offset = atoi(s.c_str() + zone + 1) * 3600 + atoi(s.c_str() + zone + 4) * 60;
    t -= s[zone] == '+' ? offset : -offset;
  }
  return t > 0 ? static_cast<uint32_t>(t) : 0;
}

// One outlet or Google News feed's best stories (feed order, recent ones only; RSS <item>
// or Atom <entry>). False if it couldn't be read.
bool fetchFeed(const Topic &topic, std::vector<Item> &items, String &error) {
  String buffer;
  int seen = 0;
  const uint32_t now = nowUtc();
  const bool keyword = topic.section.isEmpty();
  const Feed *feed = keyword ? nullptr : findFeed(topic.section);
  if (!keyword && !feed) {
    error = "unknown feed";
    return false;
  }
  auto takeItem = [&](const String &item) {
    const int position = seen++;
    Item it;
    const String rss = tagText(item, "pubDate");
    it.published = rss.length() ? parseDate(rss) : parseIsoDate(tagText(item, "published").length() ? tagText(item, "published") : tagText(item, "updated"));
    if (it.published && now > it.published && now - it.published > kMaxAge) return;
    it.position = position;
    it.title = tagText(item, "title");
    if (keyword) {
      // Google News: "Headline - Source", the source also in <source>; no usable summary.
      it.source = tagText(item, "source");
      const int dash = it.title.lastIndexOf(" - ");
      if (dash > 0) {
        if (it.source.isEmpty()) it.source = it.title.substring(dash + 3);
        it.title = it.title.substring(0, dash);
      }
    } else {
      it.source = feed->outlet;
      const String description = tagText(item, "description");
      it.summary = shortSummary(description.length() ? description : tagText(item, "summary"));
    }
    if (it.title.length() && static_cast<int>(items.size()) < kPerTopic) items.push_back(it);
  };
  const bool ok = net::download(
      feedUrl(topic),
      [&](const uint8_t *data, size_t len) {
        buffer.concat(reinterpret_cast<const char *>(data), len);
        for (;;) {
          int start = buffer.indexOf("<item>");
          if (start < 0) start = buffer.indexOf("<item ");
          const bool atom = start < 0;
          if (atom) start = buffer.indexOf("<entry>");
          if (start < 0) {
            if (buffer.length() > 16) buffer.remove(0, buffer.length() - 16);  // a split "<item>"
            break;
          }
          const char *closing = atom ? "</entry>" : "</item>";
          const int end = buffer.indexOf(closing, start);
          if (end < 0) {
            buffer.remove(0, start);
            break;
          }
          takeItem(buffer.substring(start, end));
          buffer.remove(0, end + strlen(closing));
        }
        return seen < kScanItems;  // enough: stop downloading (Google's feeds are 130 KB)
      },
      nullptr, error);
  if (!ok && seen < kScanItems) return false;  // a real failure, not our early stop
  error = "";
  return true;
}

// Kagi's index: when the current edition came out (it publishes once a day).
bool kagiIndex(uint32_t &edition, String &error) {
  String body;
  if (!net::getString(kKagiIndex, body, error)) return false;
  JsonDocument filter;
  filter["timestamp"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    error = "Kagi index unreadable";
    return false;
  }
  edition = doc["timestamp"] | 0u;
  return edition > 0;
}

// A Kagi category: its file (~100-450 KB) goes to the card first, then only the parts Dotty
// uses are read back (title, summary, how many outlets): the whole file wouldn't fit in RAM.
bool fetchKagi(const Topic &topic, std::vector<Item> &items, String &error) {
  const String temp = path("kagi.part");
  File out = SD_MMC.open(temp, FILE_WRITE);
  if (!out) {
    error = "cannot write to the card";
    return false;
  }
  const bool ok = net::download(
      feedUrl(topic), [&](const uint8_t *data, size_t len) { return out.write(data, len) == len; }, nullptr, error);
  out.close();
  if (!ok) {
    SD_MMC.remove(temp);
    return false;
  }
  JsonDocument filter;
  filter["timestamp"] = true;
  JsonObject cluster = filter["clusters"].add<JsonObject>();
  cluster["title"] = true;
  cluster["short_summary"] = true;
  cluster["unique_domains"] = true;
  JsonDocument doc;
  File in = SD_MMC.open(temp);
  const DeserializationError parsed = deserializeJson(doc, in, DeserializationOption::Filter(filter));
  in.close();
  SD_MMC.remove(temp);
  if (parsed != DeserializationError::Ok) {
    error = String("Kagi file unreadable (") + parsed.c_str() + ")";
    return false;
  }
  const uint32_t published = doc["timestamp"] | 0u;
  if (published && nowUtc() > published && nowUtc() - published > kMaxAge) return true;  // stale edition
  int position = 0;
  for (JsonObjectConst c : doc["clusters"].as<JsonArrayConst>()) {
    Item it;
    it.published = published;
    it.position = position++;
    it.title = cleanText(c["title"] | "");
    it.summary = shortSummary(c["short_summary"] | "");
    const int outlets = c["unique_domains"] | 0;
    it.source = outlets > 1 ? String(outlets) + " sources" : String("Kagi");  // the topic says it's Kagi
    if (it.title.length() && static_cast<int>(items.size()) < kPerTopic) items.push_back(it);
  }
  return true;
}

String oneLine(const String &s) {
  String out = s;
  out.replace('\t', ' ');
  out.replace('\n', ' ');
  return out;
}

void setFetchError(const String &message) {
  portENTER_CRITICAL(&fetchLock);
  strlcpy(fetchError, message.c_str(), sizeof(fetchError));
  portEXIT_CRITICAL(&fetchLock);
}

// Reads every topic's feed into stories.part, keeping a topic's old stories when its feed
// fails, then swaps it in. Its own task, so Dotty stays responsive.
void fetchTask(void *) {
  String error;
  int fetched = 0;
  bool ok = net::connect(error);
  File out;
  if (ok) {
    out = SD_MMC.open(path("stories.part"), FILE_WRITE);
    ok = static_cast<bool>(out);
    if (!ok) error = "cannot write to the card";
  }
  if (ok) {
    uint32_t edition = 0;  // Kagi's current edition (asked once, when a topic needs it)
    bool askedKagi = false, kagiOk = true;
    for (size_t i = 0; i < fetchTopics.size(); i++) {
      const Topic &t = fetchTopics[i];
      std::vector<Item> items;
      String feedError;
      if (t.kagi.length()) {
        if (!askedKagi) {
          askedKagi = true;
          String indexError;
          if (!kagiIndex(edition, indexError)) LOGW("news", "Kagi index: %s", indexError.c_str());
        }
        // Kagi publishes once a day: the same edition needn't be downloaded again.
        if (edition && edition == kagiEdition && previousLines[i].length()) {
          out.print(previousLines[i]);
          fetched++;
          fetchDone = fetchDone + 1;
          continue;
        }
      }
      const bool got = t.kagi.length() ? fetchKagi(t, items, feedError) : fetchFeed(t, items, feedError);
      if (t.kagi.length() && !got) kagiOk = false;
      if (got) {
        fetched++;
        for (const Item &it : items) {
          out.printf("%s\t%lu\t%u\t%s\t%s\t%s\n", t.key().c_str(), static_cast<unsigned long>(it.published),
                     it.position, oneLine(it.source).c_str(), oneLine(it.title).c_str(), oneLine(it.summary).c_str());
        }
        LOGI("news", "%s: %u stories", t.name.c_str(), static_cast<unsigned>(items.size()));
      } else {
        out.print(previousLines[i]);
        error = t.name + ": " + feedError;
        LOGW("news", "%s", error.c_str());
      }
      fetchDone = fetchDone + 1;
    }
    out.close();
    if (askedKagi && kagiOk && edition) {
      kagiEdition = edition;
      Preferences prefs;
      prefs.begin("news", false);
      prefs.putULong("kagi", edition);
      prefs.end();
    }
    if (fetched > 0) {
      SD_MMC.remove(path("stories.tsv"));
      if (!SD_MMC.rename(path("stories.part"), path("stories.tsv"))) {
        fetched = 0;
        error = "cannot write to the card";
      }
    } else {
      SD_MMC.remove(path("stories.part"));
    }
  }
  net::disconnect();
  if (fetched > 0) {
    fetchedAt = time(nullptr);
    Preferences prefs;
    prefs.begin("news", false);
    prefs.putULong("fetched", fetchedAt);
    prefs.end();
  }
  // Some topics failing is worth saying even when others worked.
  setFetchError(fetched == static_cast<int>(fetchTopics.size()) ? "" : (error.length() ? error : "no news came back"));
  LOGI("news", "fetched %d of %u topics", fetched, static_cast<unsigned>(fetchTopics.size()));
  fetchFinished = true;
  fetchRunning = false;
  vTaskDelete(nullptr);
}

// ---------- the pool ----------

void scorePool() {
  const uint32_t now = nowUtc();
  for (Story &s : pool) {
    float hours = s.published && now > s.published ? (now - s.published) / 3600.0f : 0;
    if (s.topic[0] == 'c') hours = min(hours, 12.0f);  // a daily briefing stays today's news all day
    s.score = s.position + hours * 0.5f;
    // The same story in several of the user's topics: a big one.
    int copies = 0;
    for (const Story &o : pool) copies += strcasecmp(o.title, s.title) == 0;
    s.score -= 2.0f * (copies - 1);
  }
}

void loadPool() {
  pool.clear();
  free(text);
  text = nullptr;
  File f = SD_MMC.open(path("stories.tsv"));
  if (!f) return;
  const size_t size = f.size();
  text = static_cast<char *>(ps_malloc(size + 1));
  if (!text) return;
  f.read(reinterpret_cast<uint8_t *>(text), size);
  text[size] = '\0';
  for (char *line = text; line && *line;) {
    char *next = strchr(line, '\n');
    if (next) *next++ = '\0';
    char *fields[6] = {line};
    int n = 1;
    for (; n < 6; n++) {
      char *tab = strchr(fields[n - 1], '\t');
      if (!tab) break;
      *tab = '\0';
      fields[n] = tab + 1;
    }
    if (n == 6) {
      Story s;
      s.topic = fields[0];
      s.published = strtoul(fields[1], nullptr, 10);
      s.position = atoi(fields[2]);
      s.source = fields[3];
      s.title = fields[4];
      s.summary = fields[5];
      pool.push_back(s);
    }
    line = next;
  }
  scorePool();
}

void loadTopics() {
  topicList.clear();
  File f = SD_MMC.open(path("topics.json"));
  JsonDocument doc;
  if (f && deserializeJson(doc, f) == DeserializationError::Ok) {
    for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
      Topic t;
      t.section = o["section"] | "";
      t.kagi = o["kagi"] | "";
      t.query = o["query"] | "";
      t.name = o["name"] | "";
      t.star = o["star"] | false;
      if (t.section.length() || t.kagi.length() || t.query.length()) topicList.push_back(t);
    }
    return;
  }
  // First run: a sensible start the app can change.
  topicList = {{"top", "", "", "Top stories", true}, {"tech", "", "", "Tech", true}, {"world", "", "", "World", false},
               {"science", "", "", "Science", false}};
}

// refetch: the topics' stories change (added/removed), not just their star or order.
void saveTopics(bool refetch) {
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  for (const Topic &t : topicList) {
    JsonObject o = list.add<JsonObject>();
    if (t.section.length()) o["section"] = t.section;
    else if (t.kagi.length()) o["kagi"] = t.kagi;
    else o["query"] = t.query;
    o["name"] = t.name;
    o["star"] = t.star;
  }
  File out = SD_MMC.open(path("topics.json"), FILE_WRITE);
  if (out) serializeJson(doc, out);
  if (refetch) changedAt = max<uint32_t>(1, millis());
}

int indexOf(const String &key) {
  for (size_t i = 0; i < topicList.size(); i++) {
    if (topicList[i].key() == key) return i;
  }
  return -1;
}

}  // namespace

const std::vector<Feed> &feeds() {
  return kFeeds;
}

bool begin() {
  storage::makeDirs(storage::myDataDir());
  loadTopics();
  loadPool();
  Preferences prefs;
  prefs.begin("news", true);
  fetchedAt = prefs.getULong("fetched", 0);
  kagiEdition = prefs.getULong("kagi", 0);
  prefs.end();
  LOGI("news", "%u topics, %u stories", static_cast<unsigned>(topicList.size()), static_cast<unsigned>(pool.size()));
  return !pool.empty();
}

const std::vector<Topic> &topics() {
  return topicList;
}

bool addTopic(const Topic &topic, String &error) {
  if (topicList.size() >= static_cast<size_t>(kMaxTopics)) {
    error = "Dotty follows up to " + String(kMaxTopics) + " topics";
    return false;
  }
  if (indexOf(topic.key()) >= 0) {
    error = "already a topic";
    return false;
  }
  topicList.push_back(topic);
  saveTopics(true);
  return true;
}

bool removeTopic(const String &key) {
  const int i = indexOf(key);
  if (i < 0) return false;
  topicList.erase(topicList.begin() + i);
  saveTopics(true);
  return true;
}

bool starTopic(const String &key, bool on) {
  const int i = indexOf(key);
  if (i < 0) return false;
  topicList[i].star = on;
  saveTopics(false);
  return true;
}

bool moveTopic(int from, int to) {
  const int n = topicList.size();
  if (from < 0 || from >= n || to < 0 || to >= n) return false;
  const Topic t = topicList[from];
  topicList.erase(topicList.begin() + from);
  topicList.insert(topicList.begin() + to, t);
  saveTopics(false);
  return true;
}

std::vector<const Story *> stories(const String &topicKey) {
  std::vector<const Story *> out;
  // Favourites: the starred topics (every topic when none is starred).
  std::vector<String> keys;
  if (topicKey.length()) {
    keys.push_back(topicKey);
  } else {
    for (const Topic &t : topicList) {
      if (t.star) keys.push_back(t.key());
    }
    if (keys.empty()) {
      for (const Topic &t : topicList) keys.push_back(t.key());
    }
  }
  for (const Story &s : pool) {
    if (std::find(keys.begin(), keys.end(), String(s.topic)) == keys.end()) continue;
    const bool dup = std::any_of(out.begin(), out.end(), [&](const Story *o) { return strcasecmp(o->title, s.title) == 0; });
    if (!dup) out.push_back(&s);
  }
  std::stable_sort(out.begin(), out.end(), [](const Story *a, const Story *b) { return a->score < b->score; });
  return out;
}

size_t storyCount() {
  return pool.size();
}

const Story *lockStory(const tm &now) {
  std::vector<const Story *> fitting;
  for (const Story *s : stories("")) {
    if (s->fitsLock) fitting.push_back(s);
    if (fitting.size() >= 12) break;  // the most important ones take turns
  }
  if (fitting.empty()) return nullptr;
  tm day = now;
  const uint32_t days = static_cast<uint32_t>(mktime(&day) / 86400);
  const uint32_t slot = days * 288 + (now.tm_hour * 60 + now.tm_min) / 5;
  return fitting[slot % fitting.size()];
}

void measureLockFit(std::function<bool(const Story &)> fits) {
  size_t n = 0;
  for (Story &s : pool) n += (s.fitsLock = fits(s));
  LOGI("news", "%u headlines fit the lock screen", static_cast<unsigned>(n));
}

String age(const Story &story) {
  const uint32_t now = nowUtc();
  if (!story.published || now <= story.published) return "just now";
  const uint32_t minutes = (now - story.published) / 60;
  if (minutes < 60) return String(max<uint32_t>(1, minutes)) + "m ago";
  if (minutes < 48 * 60) return String(minutes / 60) + "h ago";
  return String(minutes / 1440) + "d ago";
}

bool fetchDue() {
  if (fetchRunning || topicList.empty() || net::saved().empty()) return false;
  if (changedAt && millis() - changedAt >= kAfterChangeMs) return true;
  const uint32_t since = millis() - lastAttempt;
  if (attempted) return since >= (fetchError[0] ? kRetryMs : kRefreshMs);
  // First check after a start: fetch unless the card's stories are fresh.
  return pool.empty() || static_cast<uint32_t>(time(nullptr)) - fetchedAt >= kRefreshMs / 1000;
}

void startFetch() {
  if (fetchRunning) return;
  attempted = true;
  lastAttempt = millis();
  changedAt = 0;
  fetchTopics = topicList;
  previousLines.assign(fetchTopics.size(), String());
  for (size_t i = 0; i < fetchTopics.size(); i++) {
    const String key = fetchTopics[i].key();
    for (const Story &s : pool) {
      if (key != s.topic) continue;
      previousLines[i] += key + "\t" + String(s.published) + "\t" + String(s.position) + "\t" + s.source + "\t" +
                          s.title + "\t" + s.summary + "\n";
    }
  }
  fetchRunning = true;
  fetchFinished = false;
  fetchDone = 0;
  fetchTotal = fetchTopics.size();
  LOGI("news", "fetching %u topics", static_cast<unsigned>(fetchTopics.size()));
  // HTTPS needs a deep stack.
  if (xTaskCreatePinnedToCore(fetchTask, "news-fetch", 12288, nullptr, 3, nullptr, 0) != pdPASS) {
    fetchRunning = false;
    setFetchError("not enough memory");
  }
}

FetchState fetchState() {
  FetchState s;
  s.running = fetchRunning;
  s.done = fetchDone;
  s.total = fetchTotal;
  char error[sizeof(fetchError)];
  portENTER_CRITICAL(&fetchLock);
  memcpy(error, fetchError, sizeof(error));  // no allocation inside the critical section
  portEXIT_CRITICAL(&fetchLock);
  s.error = error;
  s.fetchedAt = fetchedAt;
  return s;
}

bool takeFetchFinished() {
  if (!fetchFinished) return false;
  fetchFinished = false;
  loadPool();
  return true;
}

}  // namespace news
