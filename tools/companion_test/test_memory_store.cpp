// uses: cartridges/companion/memory_store.cpp
// What the companion remembers (cartridges/companion/memory_store.h): the file, the rules, the
// list given to Gemini at the start of a chat.
#include <string>

#include "check.h"
#include "memory_store.h"

static size_t codePoints(const std::string &s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) n++;
  return n;
}

static bool validUtf8(const std::string &s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = s[i];
    const int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!len || i + len > s.size()) return false;
    for (int k = 1; k < len; k++)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    i += len;
  }
  return true;
}

int main() {
  MemoryStore m;
  CHECK_EQ("add: first id", m.add("You live in Lisbon", "you", "dotty", 100).id, 1);
  CHECK_EQ("add: second id", m.add("Your sister is called Ana", "people", "dotty", 200).id, 2);
  CHECK_EQ("add: third id", m.add("You love swimming", "likes", "app", 300).id, 3);
  CHECK("remove: ok", m.remove(2));
  MemoryStore copy;
  CHECK("save/load: loads", copy.load(m.save()));
  CHECK_EQ("save/load: count", copy.all().size(), size_t(2));
  CHECK_EQ("save/load: a deleted id is never reused", copy.add("New", "other", "app", 400).id, 4);
  const Memory *loaded = copy.find(3);
  CHECK("save/load: fields", loaded && loaded->text == "You love swimming" && loaded->group == "likes" &&
                                 loaded->by == "app" && loaded->added == 300 && loaded->changed == 300);

  CHECK_EQ("add: bad group", m.add("x", "bogus", "app", 1).error, std::string("group"));
  CHECK_EQ("add: empty", m.add("   ", "you", "app", 1).error, std::string("empty"));

  // Dedupe: the same memory (case and spaces aside) isn't added twice.
  const MemoryStore::Added again = m.add("you  love Swimming ", "you", "dotty", 500);
  CHECK("dedupe: ok with the existing id", again.ok && again.id == 3);
  CHECK_EQ("dedupe: nothing added", m.all().size(), size_t(2));

  // Cut: at most 200 characters, at a word boundary, never a broken UTF-8 character.
  std::string longText;
  while (codePoints(longText) < 250) longText += "café 🎉 swim ";
  const std::string cut = MemoryStore::cut(longText);
  CHECK("cut: at most 200 characters", codePoints(cut) <= 200);
  CHECK("cut: valid UTF-8", validUtf8(cut));
  CHECK("cut: ends at a word", !cut.empty() && cut.back() != ' ' && longText.compare(0, cut.size(), cut) == 0 &&
                                   longText[cut.size()] == ' ');
  CHECK_EQ("cut: short text untouched", MemoryStore::cut("  Short one  "), std::string("Short one"));

  // Awkward text survives the file.
  const std::string awkward = "He said \"hi\"\nthen left \xE2\x80\x94 caf\xC3\xA9 \xF0\x9F\x8E\x89";
  MemoryStore a;
  const int awkwardId = a.add(awkward, "other", "app", 1).id;
  MemoryStore b;
  CHECK("round trip with awkward text", b.load(a.save()) && b.find(awkwardId) && b.find(awkwardId)->text == awkward);

  // Unknown ids change nothing.
  const std::string before = m.save();
  const std::string text = "x";
  CHECK("unknown id: update refused", !m.update(99, &text, nullptr, 9));
  CHECK("unknown id: remove refused", !m.remove(99));
  CHECK_EQ("unknown id: nothing changed", m.save(), before);

  // Update: text and group change, `added` stays.
  const std::string moved = "You live in Porto", group = "you";
  CHECK("update: ok", m.update(1, &moved, &group, 900));
  CHECK("update: fields", m.find(1)->text == moved && m.find(1)->changed == 900 && m.find(1)->added == 100);
  const std::string badGroup = "nope";
  CHECK("update: bad group refused", !m.update(1, nullptr, &badGroup, 950) && m.find(1)->group == "you");

  // Pages: newest first.
  MemoryStore p;
  for (int i = 1; i <= 5; i++) p.add("Memory number " + std::to_string(i), "other", "app", i);
  const std::vector<Memory> first = p.page(0, 2), second = p.page(2, 2);
  CHECK("page: newest first", first.size() == 2 && first[0].id == 5 && first[1].id == 4);
  CHECK("page: the next two", second.size() == 2 && second[0].id == 3 && second[1].id == 2);
  CHECK_EQ("page: past the end", p.page(10, 2).size(), size_t(0));

  // Full at 300.
  MemoryStore full;
  for (int i = 0; i < MemoryStore::kMax; i++) full.add("Fact " + std::to_string(i), "other", "app", i);
  CHECK_EQ("full: the 301st is refused", full.add("One more", "other", "app", 1).error, std::string("full"));

  // The list given to Gemini.
  CHECK_EQ("prompt: empty store, no section", MemoryStore().promptSection(), std::string(""));
  MemoryStore s;
  s.add("You hate mornings", "likes", "dotty", 1);
  s.add("You live in Lisbon", "you", "dotty", 2);
  s.add("Your sister is called Ana", "people", "dotty", 3);
  s.add("You swim every morning", "you", "dotty", 4);
  const std::string section = s.promptSection();
  CHECK("prompt: header", section.rfind("What you remember about the user (use it naturally, don't recite it; "
                                        "numbers are for the memory tools):\n", 0) == 0);
  CHECK("prompt: groups in order, oldest first, no empty groups",
        section.find("About you: [2] You live in Lisbon. [4] You swim every morning.\n"
                     "People: [3] Your sister is called Ana.\n"
                     "Likes & dislikes: [1] You hate mornings.") != std::string::npos &&
            section.find("Plans & dates") == std::string::npos && section.find("Other") == std::string::npos);
  CHECK("prompt: full says so", full.promptSection().find("Memory is full (300): ask the user to delete some in the "
                                                          "app before remembering more.") != std::string::npos);

  // A broken file: refused, the store left empty.
  MemoryStore broken;
  broken.add("Something", "other", "app", 1);
  CHECK("broken file: refused", !broken.load("{not json"));
  CHECK_EQ("broken file: empty store", broken.all().size(), size_t(0));

  CHECK("groups: valid ones", MemoryStore::validGroup("plans") && !MemoryStore::validGroup("Plans"));
  CHECK_EQ("groups: label", std::string(MemoryStore::label("likes")), std::string("Likes & dislikes"));
  return failures ? 1 : 0;
}
