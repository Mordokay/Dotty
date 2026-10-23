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
  CHECK_EQ("words: up to two minutes, 300 (read in chunks)", writerWords(120), 300);
  CHECK_EQ("words: capped at two minutes", writerWords(600), 300);
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

  // Read in chunks of about 100 words, cut at sentence ends: a reading never runs a minute ahead of
  // the speaker (Dotty buffers a minute of reply audio).
  std::string story;
  for (int i = 0; i < 30; i++) story += "Sentence number " + std::to_string(i) + " has exactly seven words. ";
  const std::vector<std::string> chunks = readingChunks(story, 100);
  CHECK("chunks: 210 words in 100-word chunks = 3", chunks.size() == 3);
  bool sentences = true, sized = true;
  std::string joined;
  for (const std::string &c : chunks) {
    sentences &= c.back() == '.';
    int words = 0;
    for (size_t i = 0; i < c.size(); i++) words += c[i] != ' ' && (i == 0 || c[i - 1] == ' ');
    sized &= words <= 100;
    joined += (joined.empty() ? "" : " ") + c;
  }
  CHECK("chunks: each ends a sentence, ≤ 100 words", sentences && sized);
  CHECK("chunks: nothing lost", joined + " " == story);
  CHECK("chunks: a short text is one", readingChunks("Just this. And that.", 100).size() == 1);
  CHECK("chunks: one huge sentence is still cut", readingChunks(std::string(800, 'w') + " " + std::string(5, 'x'), 100).size() >= 1);
  const std::string more = continueReadingNote("Then the boat came home.");
  CHECK("continue note: carry on exactly, no recap", more.find("Carry on reading") != std::string::npos &&
                                                         more.find("word for word") != std::string::npos &&
                                                         more.find("Then the boat came home.") != std::string::npos);
  const std::string note = readAloudNote("Breathe in.");
  CHECK("read note: exactly, word for word", has(note, "Read the text below aloud exactly, word for word"));
  CHECK("read note: no tools, nothing else", has(note, "don't call any tool") && has(note, "say nothing else"));
  CHECK("read note: the text last", note.size() > 11 && note.compare(note.size() - 11, 11, "Breathe in.") == 0);
  CHECK("failed note: answer it yourself", has(writerFailedNote(), "answer the request yourself"));
  return failures ? 1 : 0;
}
