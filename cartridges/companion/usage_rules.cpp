#include "usage_rules.h"

#include <stdio.h>

namespace {

constexpr int64_t kDay = 86400;

// Days since 1970-01-01 for a civil date, and back (Howard Hinnant's algorithms).
int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int &y, int &m, int &d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yoe + era * 400 + (m <= 2));
}

int64_t floorDiv(int64_t a, int64_t b) {
  return a / b - ((a % b != 0) && ((a < 0) != (b < 0)));
}

// The day number of the n-th Sunday (n = 1, 2) of a month.
int64_t nthSunday(int year, int month, int n) {
  const int64_t first = daysFromCivil(year, month, 1);
  const int weekday = static_cast<int>(((first % 7) + 7 + 4) % 7);  // 1970-01-01 was a Thursday (4)
  return first + (7 - weekday) % 7 + 7 * (n - 1);
}

// Pacific DST in force at this UTC instant?
bool pacificDst(int64_t utc) {
  int y, m, d;
  civilFromDays(floorDiv(utc, kDay), y, m, d);
  const int64_t start = nthSunday(y, 3, 2) * kDay + 10 * 3600;  // 02:00 PST = 10:00 UTC
  const int64_t end = nthSunday(y, 11, 1) * kDay + 9 * 3600;    // 02:00 PDT = 09:00 UTC
  return utc >= start && utc < end;
}

}  // namespace

int64_t nextPacificMidnightUtc(int64_t nowUtc) {
  const int64_t offset = pacificDst(nowUtc) ? -7 * 3600 : -8 * 3600;
  const int64_t nextLocalDay = floorDiv(nowUtc + offset, kDay) + 1;  // the Pacific date of the next midnight
  int y, m, d;
  civilFromDays(nextLocalDay, y, m, d);
  // Midnight is DST from the day after the March change through the day of the November one.
  const bool dst = nextLocalDay > nthSunday(y, 3, 2) && nextLocalDay <= nthSunday(y, 11, 1);
  return nextLocalDay * kDay + (dst ? 7 : 8) * 3600;
}

namespace {
std::string jsonString(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out + "\"";
}
}  // namespace

std::string historyLine(int64_t localEpoch, const std::string &you, const std::string &dotty, const std::string &mood,
                        int ms, int tokens, const std::string &memory) {
  return "{\"t\":" + std::to_string(localEpoch) + ",\"you\":" + jsonString(you) + ",\"dotty\":" + jsonString(dotty) +
         ",\"mood\":" + jsonString(mood) + ",\"ms\":" + std::to_string(ms) + ",\"tokens\":" + std::to_string(tokens) +
         (memory.empty() ? "" : ",\"memory\":" + jsonString(memory)) + "}";
}

std::string dayFile(int64_t localEpoch) {
  int y, m, d;
  civilFromDays(floorDiv(localEpoch, kDay), y, m, d);
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d.jsonl", y, m, d);
  return buf;
}

std::string trimKey(const std::string &raw) {
  const char *space = " \t\r\n";
  const size_t start = raw.find_first_not_of(space);
  if (start == std::string::npos) return "";
  return raw.substr(start, raw.find_last_not_of(space) - start + 1);
}
