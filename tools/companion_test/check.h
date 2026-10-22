// Tiny test helpers for tools/companion_test (host builds with clang++, see run.py).
#pragma once
#include <cstdio>
#include <string>

static int failures = 0;

#define CHECK(name, cond)                                     \
  do {                                                        \
    if (cond) std::printf("PASS %s\n", name);                 \
    else {                                                    \
      std::printf("FAIL %s: %s\n", name, #cond);              \
      failures++;                                             \
    }                                                         \
  } while (0)

#define CHECK_EQ(name, actual, expected)                                          \
  do {                                                                            \
    const auto a_ = (actual);                                                     \
    const auto e_ = (expected);                                                   \
    if (a_ == e_) std::printf("PASS %s\n", name);                                 \
    else {                                                                        \
      std::printf("FAIL %s: got %s, expected %s\n", name, show(a_).c_str(), show(e_).c_str()); \
      failures++;                                                                 \
    }                                                                             \
  } while (0)

template <typename T> std::string show(const T &v) { return std::to_string(v); }
inline std::string show(const std::string &v) { return "\"" + v + "\""; }
inline std::string show(const char *v) { return "\"" + std::string(v) + "\""; }
