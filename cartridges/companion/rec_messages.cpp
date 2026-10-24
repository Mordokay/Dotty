#include "rec_messages.h"

namespace {

std::string serialize(const JsonDocument &doc) {
  std::string out;
  serializeJson(doc, out);
  return out;
}

void readFields(JsonObjectConst args, RecText &t) {
  t.id = args["id"] | t.id.c_str();
  t.companion = args["companion"] | t.companion.c_str();
  t.summary = args["summary"] | t.summary.c_str();
  t.title = args["title"] | t.title.c_str();
  t.isDraft = args["isDraft"] | t.isDraft;
  if (args["points"].is<JsonArrayConst>()) {
    t.points.clear();
    for (JsonVariantConst p : args["points"].as<JsonArrayConst>()) t.points.push_back(p | "");
  }
}

}  // namespace

std::string recBeginEvent(const std::string &id, const std::string &companion, const std::string &title, uint32_t from) {
  JsonDocument d;
  d["event"] = "rec.begin";
  d["id"] = id;
  d["companion"] = companion;
  d["title"] = title;
  if (from) d["from"] = from;
  return serialize(d);
}

std::string recPartEvent(const std::string &id, int n) {
  JsonDocument d;
  d["event"] = "rec.part";
  d["id"] = id;
  d["n"] = n;
  return serialize(d);
}

std::string recUndoEvent(const std::string &id) {
  JsonDocument d;
  d["event"] = "rec.undo";
  d["id"] = id;
  return serialize(d);
}

std::string recEndEvent(const std::string &id, bool done) {
  JsonDocument d;
  d["event"] = "rec.end";
  d["id"] = id;
  d["how"] = done ? "done" : "discard";
  return serialize(d);
}

size_t recFrameHeader(uint8_t *out, uint8_t seq) {
  out[0] = 0x1F;
  out[1] = 0x02;
  out[2] = seq;
  return 3;
}

bool RecTextAssembler::command(JsonObjectConst args, RecText &complete) {
  const std::string id = args["id"] | "";
  if (id.empty()) return false;
  if (args["done"] | false) {  // the pieces' end
    const auto it = pending_.find(id);
    if (it == pending_.end()) return false;
    Pending &p = it->second;
    const bool whole = p.transcript.complete() && (!p.hasDraft || p.draft.complete());
    if (!whole) {
      pending_.erase(it);  // the phone sends it all again
      return false;
    }
    complete = p.text;
    complete.transcript = p.transcript.data();
    if (p.hasDraft) complete.draft = p.draft.data();
    pending_.erase(it);
    return true;
  }
  RecText t;
  readFields(args, t);
  if (args["pieces"] | false) {
    Pending &p = pending_[id];
    p = Pending();
    p.text = t;
    return false;
  }
  t.transcript = args["transcript"] | "";
  t.draft = args["draft"] | "";
  if (t.transcript.empty()) return false;
  complete = t;
  return true;
}

bool RecTextAssembler::piece(JsonObjectConst args) {
  const auto it = pending_.find(args["id"] | "");
  if (it == pending_.end()) return false;
  const std::string field = args["field"] | "";
  PieceBuffer *buf = field == "transcript" ? &it->second.transcript : field == "draft" ? &it->second.draft : nullptr;
  if (!buf) return false;
  const size_t offset = args["offset"] | 0u, total = args["total"] | 0u;
  if (offset == 0) {
    if (!buf->begin(total, kMaxText)) return false;
    if (field == "draft") it->second.hasDraft = true;
  }
  return buf->add(offset, args["text"] | "");
}
