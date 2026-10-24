// uses: cartridges/companion/rec_messages.cpp
// A recording streamed to the iPhone (rec.* events, audio frames 0x1F 0x02 seq) and its text coming back
// (companion.recording.text, long fields in companion.recording.piece), matched by the recording's id.
#include <ArduinoJson.h>

#include "check.h"
#include "rec_messages.h"

static JsonDocument parse(const std::string &s) {
  JsonDocument d;
  deserializeJson(d, s);
  return d;
}

int main() {
  {
    JsonDocument b = parse(recBeginEvent("20261024-153012", "c1", "Bananas in space"));
    JsonDocument p = parse(recPartEvent("20261024-153012", 2));
    JsonDocument u = parse(recUndoEvent("20261024-153012"));
    JsonDocument e = parse(recEndEvent("20261024-153012", false));
    CHECK("events carry their fields",
          std::string(b["event"] | "") == "rec.begin" && std::string(b["companion"] | "") == "c1" &&
              std::string(b["title"] | "") == "Bananas in space" && std::string(p["event"] | "") == "rec.part" &&
              (p["n"] | 0) == 2 && std::string(u["event"] | "") == "rec.undo" && std::string(e["event"] | "") == "rec.end" &&
              std::string(e["how"] | "") == "discard" && std::string(e["id"] | "") == "20261024-153012");
  }
  {
    uint8_t h[3];
    CHECK("frame header", recFrameHeader(h, 7) == 3 && h[0] == 0x1F && h[1] == 0x02 && h[2] == 7);
  }
  {
    RecTextAssembler a;
    RecText t;
    JsonDocument cmd = parse(R"({"id":"A","companion":"c1","transcript":"Bananas on the moon.","draft":"Bananas on the mood.",)"
                             R"("isDraft":false,"summary":"A story.","points":["bananas","moon"],"title":"Moon bananas"})");
    const bool done = a.command(cmd.as<JsonObjectConst>(), t);
    CHECK("short text in one command", done && t.id == "A" && t.transcript == "Bananas on the moon." &&
                                           t.draft == "Bananas on the mood." && t.points.size() == 2 && t.title == "Moon bananas" &&
                                           !t.isDraft && t.summary == "A story.");
  }
  {
    RecTextAssembler a;
    RecText t;
    std::string text;
    while (text.size() < 700) text += "word ";
    JsonDocument head = parse(R"({"id":"A","companion":"c1","pieces":true,"summary":"S","points":[]})");
    const bool early = a.command(head.as<JsonObjectConst>(), t);
    bool ok = true;
    for (size_t at = 0; at < text.size(); at += 240) {
      JsonDocument p;
      p["id"] = "A";
      p["field"] = "transcript";
      p["offset"] = at;
      p["total"] = text.size();
      p["text"] = text.substr(at, 240);
      ok = ok && a.piece(p.as<JsonObjectConst>());
    }
    JsonDocument end = parse(R"({"id":"A","done":true})");
    CHECK("long transcript in pieces", !early && ok && a.command(end.as<JsonObjectConst>(), t) && t.transcript == text &&
                                           t.summary == "S");
  }
  {
    RecTextAssembler a;
    RecText t;
    JsonDocument head = parse(R"({"id":"A","companion":"c1","pieces":true,"summary":"S"})");
    a.command(head.as<JsonObjectConst>(), t);
    JsonDocument p0 = parse(R"({"id":"A","field":"transcript","offset":0,"total":600,"text":")" + std::string(240, 'x') + "\"}");
    JsonDocument p2 = parse(R"({"id":"A","field":"transcript","offset":480,"total":600,"text":")" + std::string(120, 'x') + "\"}");
    a.piece(p0.as<JsonObjectConst>());
    const bool refused = !a.piece(p2.as<JsonObjectConst>());
    JsonDocument end = parse(R"({"id":"A","done":true})");
    CHECK("pieces with a gap -> not complete, nothing returned", refused && !a.command(end.as<JsonObjectConst>(), t) && t.id.empty());
  }
  {
    RecTextAssembler a;
    RecText t;
    JsonDocument ha = parse(R"({"id":"A","companion":"c1","pieces":true,"summary":"first"})");
    JsonDocument hb = parse(R"({"id":"B","companion":"c1","pieces":true,"summary":"second"})");
    a.command(ha.as<JsonObjectConst>(), t);
    a.command(hb.as<JsonObjectConst>(), t);
    JsonDocument pb = parse(R"({"id":"B","field":"transcript","offset":0,"total":3,"text":"bbb"})");
    JsonDocument pa = parse(R"({"id":"A","field":"transcript","offset":0,"total":3,"text":"aaa"})");
    a.piece(pb.as<JsonObjectConst>());
    a.piece(pa.as<JsonObjectConst>());
    JsonDocument ea = parse(R"({"id":"A","done":true})");
    RecText ta, tb;
    const bool gotA = a.command(ea.as<JsonObjectConst>(), ta);
    JsonDocument eb = parse(R"({"id":"B","done":true})");
    const bool gotB = a.command(eb.as<JsonObjectConst>(), tb);
    CHECK("text for an older id", gotA && gotB && ta.transcript == "aaa" && ta.summary == "first" && tb.transcript == "bbb" &&
                                      tb.summary == "second");
  }
  return failures ? 1 : 0;
}
