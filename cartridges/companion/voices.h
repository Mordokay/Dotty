#pragma once

#include <string>
#include <vector>

// Gemini's 30 prebuilt voices with Google's one-word description, the user's favourites first
// (the app lists the same: Features/Companion/VoicesView.swift).
struct Voice {
  const char *name, *style;
};

constexpr Voice kVoices[] = {
    {"Zephyr", "Bright"},         {"Achernar", "Soft"},         {"Puck", "Upbeat"},
    {"Charon", "Informative"},    {"Kore", "Firm"},             {"Fenrir", "Excitable"},
    {"Leda", "Youthful"},         {"Orus", "Firm"},             {"Aoede", "Breezy"},
    {"Callirrhoe", "Easy-going"}, {"Autonoe", "Bright"},        {"Enceladus", "Breathy"},
    {"Iapetus", "Clear"},         {"Umbriel", "Easy-going"},    {"Algieba", "Smooth"},
    {"Despina", "Smooth"},        {"Erinome", "Clear"},         {"Algenib", "Gravelly"},
    {"Rasalgethi", "Informative"}, {"Laomedeia", "Upbeat"},     {"Alnilam", "Firm"},
    {"Schedar", "Even"},          {"Gacrux", "Mature"},         {"Pulcherrima", "Forward"},
    {"Achird", "Friendly"},       {"Zubenelgenubi", "Casual"},  {"Vindemiatrix", "Gentle"},
    {"Sadachbia", "Lively"},      {"Sadaltager", "Knowledgeable"}, {"Sulafat", "Warm"},
};
constexpr int kVoiceCount = sizeof(kVoices) / sizeof(kVoices[0]);

// Index of `name`, or -1.
inline int voiceIndex(const char *name) {
  for (int i = 0; i < kVoiceCount; i++) {
    const char *a = kVoices[i].name, *b = name;
    while (*a && *a == *b) a++, b++;
    if (!*a && !*b) return i;
  }
  return -1;
}

constexpr const char *kDefaultFavourites = "Zephyr,Achernar";

// The known voices in a comma-separated favourites list (picked in the app), in its order.
inline std::vector<int> favouriteIndexes(const std::string &favourites) {
  std::vector<int> out;
  size_t start = 0;
  while (start <= favourites.size()) {
    const size_t comma = favourites.find(',', start);
    const std::string name = favourites.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    const int i = voiceIndex(name.c_str());
    if (i >= 0) out.push_back(i);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

inline int favouriteCount(const std::string &favourites) {
  return favouriteIndexes(favourites).size();
}

// The favourite after (step 1) or before (-1) `current`; a voice that isn't a favourite goes to
// the first one. No favourites = every voice.
inline const char *nextFavourite(const std::string &favourites, const char *current, int step) {
  std::vector<int> list = favouriteIndexes(favourites);
  if (list.empty())
    for (int i = 0; i < kVoiceCount; i++) list.push_back(i);
  const int n = list.size(), now = voiceIndex(current);
  for (int i = 0; i < n; i++)
    if (list[i] == now) return kVoices[list[((i + step) % n + n) % n]].name;
  return kVoices[list[0]].name;
}
