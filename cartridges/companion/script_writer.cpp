#include "script_writer.h"

#include <ArduinoJson.h>

namespace {

std::string trim(const std::string &s) {
  const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string join(const std::vector<std::string> &items, const char *sep) {
  std::string out;
  for (const std::string &i : items) out += (out.empty() ? "" : sep) + i;
  return out;
}

}  // namespace

int writerWords(int seconds) {
  const int s = seconds < 30 ? 30 : seconds > 60 ? 60 : seconds;
  return s * 5 / 2;
}

std::string writerRequest(const Profile &p, const std::string &memories, const std::string &what, int seconds) {
  const std::string who = p.awake() ? p.name : "the companion";
  std::string system = "You write what " + who + ", a small desk companion who lives on the user's desk, will say "
                       "out loud to the user.";
  if (!p.personality.empty()) system += " " + who + "'s personality: " + p.personality;
  if (!p.traits.empty()) system += " Traits: " + join(p.traits, ", ") + ".";
  if (!p.quirks.empty()) system += " Quirks: " + join(p.quirks, "; ") + ".";
  if (!p.speaking.empty()) system += " Way of speaking: " + p.speaking;
  if (!p.backstory.empty()) system += " Story: " + p.backstory;
  if (!memories.empty()) system += "\n\n" + memories + "\n\n";
  system += " Write it in " + who + "'s own voice, in the first person, talking to the user. Spoken English in "
            "full, flowing sentences; no headings, lists, markdown, emoji or stage directions. About " +
            std::to_string(writerWords(seconds)) + " words. Only the words to say.";
  JsonDocument doc;
  doc["systemInstruction"]["parts"][0]["text"] = system;
  doc["contents"][0]["role"] = "user";
  doc["contents"][0]["parts"][0]["text"] = what;
  doc["generationConfig"]["maxOutputTokens"] = 600;  // ~150 words is ~200 tokens
  std::string out;
  serializeJson(doc, out);
  return out;
}

Written parseWritten(int httpStatus, const std::string &body) {
  Written w;
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    w.error = "HTTP " + std::to_string(httpStatus) + ", not JSON";
    return w;
  }
  if (httpStatus != 200) {
    w.error = doc["error"]["message"] | ("HTTP " + std::to_string(httpStatus)).c_str();
    return w;
  }
  w.tokens = doc["usageMetadata"]["totalTokenCount"] | 0;
  w.text = trim(doc["candidates"][0]["content"]["parts"][0]["text"] | "");
  w.ok = !w.text.empty();
  if (!w.ok) w.error = "no text";
  return w;
}

std::string readAloudNote(const std::string &text) {
  return "(Dotty, not the user: here is the text you asked to have written.) Read the text below aloud exactly, word "
         "for word, as yourself, with the feeling it calls for. Don't add, skip or change anything, don't call any "
         "tool, and say nothing else.\n\n" + text;
}

std::string writerFailedNote() {
  return "(Dotty, not the user: writing that out didn't work.) Please answer the request yourself, as fully as you "
         "can in one reply.";
}
