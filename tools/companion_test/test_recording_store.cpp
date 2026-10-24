// uses: cartridges/companion/recording_store.cpp
// Voice notes recorded through a companion (piece 3b): their details on the card, finding one by what
// the user says ("the last one", a date, a word), the summaries the companion knows, recall's text, and
// repairing a WAV a power cut left with a stale header.
#include <ctime>

#include "check.h"
#include "recording_store.h"

static int64_t local(int y, int mo, int d, int h, int mi, int s = 0) {
  tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_sec = s;
  return timegm(&t);
}

static bool has(const std::string &s, const std::string &part) {
  return s.find(part) != std::string::npos;
}

static RecordingMeta rec(const std::string &title, int64_t created, const std::string &summary) {
  RecordingMeta m;
  m.name = recordingName(created);
  m.title = title;
  m.created = created;
  m.summary = summary;
  m.ms = 134000;
  return m;
}

int main() {
  const int64_t now = local(2026, 10, 24, 16, 0);
  CHECK_EQ("recordingName", recordingName(local(2026, 10, 24, 15, 30, 12)), std::string("20261024-153012"));
  {
    RecordingMeta m = rec("Bananas in space", local(2026, 10, 24, 15, 30), "A kids' story idea.");
    m.parts = 3;
    m.synced = true;
    m.isDraft = true;
    m.textAt = now;
    m.points = {"bananas", "the moon", "a rocket", "a squire"};
    m.open = true;
    RecordingMeta back;
    const bool ok = parseRecordingMeta(recordingMetaJson(m), back);
    CHECK("meta round trip", ok && back.name == m.name && back.title == m.title && back.created == m.created &&
                                 back.ms == 134000 && back.parts == 3 && back.synced && back.isDraft && back.textAt == now &&
                                 back.summary == m.summary && back.points.size() == 4 && back.points[3] == "a squire" && back.open);
    CHECK("broken meta is false", !parseRecordingMeta("{x", back));
  }
  const std::vector<RecordingMeta> list = {
      rec("Shopping list", local(2026, 10, 24, 12, 0), "Milk, eggs and coffee."),
      rec("Bananas in space", local(2026, 10, 23, 18, 0), "A story about bananas landing on the moon."),
      rec("Gym plan", local(2026, 10, 22, 9, 0), "Three days a week, with a rest day."),
      rec("Morning thoughts", local(2026, 10, 22, 7, 0), "About moving to Lisbon next year."),
  };
  CHECK_EQ("match: the last one", matchRecording(list, "the last one", now), 0);
  CHECK_EQ("match: a title word", matchRecording(list, "my BANANAS recording", now), 1);
  CHECK_EQ("match: a date", matchRecording(list, "2026-10-22", now), 2);
  CHECK_EQ("match: yesterday", matchRecording(list, "the one from yesterday", now), 1);
  CHECK_EQ("match: a summary word", matchRecording(list, "the one about Lisbon", now), 3);
  CHECK_EQ("match: nothing", matchRecording(list, "the piano piece", now), -1);
  {
    std::vector<RecordingMeta> many;
    for (int i = 0; i < 12; i++) many.push_back(rec("Note " + std::to_string(i), now - i * 3600, "Summary " + std::to_string(i)));
    many[0].points = {"a point"};
    const std::string s = recordingsSection(many, 10);
    CHECK("section: newest 10, with date and title, no points",
          has(s, "Note 0") && has(s, "Note 9") && !has(s, "Note 10") && has(s, "24 Oct") && has(s, "Summary 0") &&
              !has(s, "a point"));
    CHECK("section: empty -> empty", recordingsSection({}, 10).empty());
  }
  {
    const std::string b = transcriptBlock(list[1], "The bananas land on the moon and plant a tree.");
    CHECK("transcriptBlock has the date and title", has(b, "Fri 23 Oct") && has(b, "Bananas in space") && has(b, "plant a tree"));
  }
  {
    RecordingMeta m = rec("Bananas in space", local(2026, 10, 24, 15, 30), "");
    m.ms = 77000;
    RecordingMeta longOne = m;
    longOne.ms = 3725000;
    RecordingMeta empty = m;
    empty.title = "";
    CHECK_EQ("row detail", recordingRowDetail(m), std::string("Sat 24 Oct - 1:17"));
    CHECK_EQ("row detail: over an hour", recordingRowDetail(longOne), std::string("Sat 24 Oct - 1:02:05"));
    CHECK_EQ("row title: untitled", recordingRowTitle(empty), std::string("Voice note"));
    CHECK_EQ("row title", recordingRowTitle(m), std::string("Bananas in space"));
  }
  {
    RecordingMeta m = rec("Gym plan", local(2026, 10, 22, 9, 0), "Three days a week.");
    m.points = {"Monday legs", "Rest on Thursday"};
    RecordingMeta bare = rec("x", 0, "");
    CHECK_EQ("info text: summary, then the points", recordingInfoText(m),
             std::string("Three days a week.\n\n- Monday legs\n- Rest on Thursday"));
    CHECK_EQ("info text: nothing yet", recordingInfoText(bare), std::string("No summary yet."));
  }
  CHECK("page after a delete: stays, or steps back from an emptied last page",
        listPageAfterDelete(9, 1) == 1 && listPageAfterDelete(8, 2) == 1 && listPageAfterDelete(4, 1) == 0 &&
            listPageAfterDelete(0, 0) == 0);
  {
    const HeaderFix cut = headerFix(44 + 32000, 16000);
    const HeaderFix tiny = headerFix(30, 0);
    const HeaderFix odd = headerFix(44 + 32001, 0);
    const HeaderFix fine = headerFix(44 + 32000, 32000);
    CHECK("header repair plan", cut.needed && cut.dataBytes == 32000 && !tiny.needed && tiny.dataBytes == 0 &&
                                    odd.needed && odd.dataBytes == 32000 && !fine.needed);
  }
  return failures ? 1 : 0;
}
