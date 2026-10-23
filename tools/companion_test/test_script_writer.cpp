// uses: cartridges/companion/script_writer.cpp cartridges/companion/companion_store.cpp
// Long answers: a text model writes them in the companion's voice (Live keeps its own replies to
// ~30 s), then Live reads the text aloud word for word.
#include <ArduinoJson.h>

#include "check.h"
#include "script_writer.h"

static bool has(const std::string &s, const std::string &part) {
  return s.find(part) != std::string::npos;
}

int main() {
  CHECK_EQ("words: a minute is ~150 (read at ~2.5 words a second)", writerWords(60), 150);
  CHECK_EQ("words: capped at a minute (Dotty keeps a minute of audio)", writerWords(600), 150);
  CHECK_EQ("words: at least 30 s worth", writerWords(5), 75);

  Profile mia = presetProfile("partner");
  mia.name = "Mia";
  const std::string body = writerRequest(mia, "What you remember:\n1. The user is called Pedro.", "a one-minute guided meditation", 60);
  JsonDocument doc;
  CHECK("request: valid JSON", deserializeJson(doc, body) == DeserializationError::Ok);
  const std::string system = doc["systemInstruction"]["parts"][0]["text"] | "";
  CHECK("request: in the companion's voice", has(system, "Mia") && has(system, mia.personality) && has(system, "first person"));
  CHECK("request: knows the memories", has(system, "The user is called Pedro."));
  CHECK("request: the length in words", has(system, "About 150 words"));
  CHECK("request: spoken, plain text only", has(system, "no headings, lists, markdown, emoji or stage directions"));
  CHECK_EQ("request: what to write", std::string(doc["contents"][0]["parts"][0]["text"] | ""), std::string("a one-minute guided meditation"));
  CHECK("request: an output cap", (doc["generationConfig"]["maxOutputTokens"] | 0) > 300);
  Profile asleep = presetProfile("coach");
  CHECK("request: a nameless companion reads fine", has(writerRequest(asleep, "", "a story", 40), "a small desk companion"));

  const Written ok = parseWritten(200, R"({"candidates":[{"content":{"parts":[{"text":"  Breathe in.\nBreathe out.  "}]}}],"usageMetadata":{"totalTokenCount":321}})");
  CHECK("answer: the text, trimmed", ok.ok && ok.text == "Breathe in.\nBreathe out.");
  CHECK_EQ("answer: tokens", ok.tokens, 321);
  const Written quota = parseWritten(429, R"({"error":{"message":"Resource has been exhausted"}})");
  CHECK("answer: an error says why", !quota.ok && quota.error == "Resource has been exhausted");
  CHECK("answer: no text = failed", !parseWritten(200, R"({"candidates":[]})").ok);
  CHECK("answer: not JSON = failed", !parseWritten(200, "<html>").ok);

  const std::string note = readAloudNote("Breathe in.");
  CHECK("read note: exactly, word for word", has(note, "Read the text below aloud exactly, word for word"));
  CHECK("read note: no tools, nothing else", has(note, "don't call any tool") && has(note, "say nothing else"));
  CHECK("read note: the text last", note.size() > 11 && note.compare(note.size() - 11, 11, "Breathe in.") == 0);
  CHECK("failed note: answer it yourself", has(writerFailedNote(), "answer the request yourself"));
  return failures ? 1 : 0;
}
