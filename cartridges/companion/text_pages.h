#pragma once

// A long reply is shown a page of lines at a time: lines fill the page top to bottom, and the
// line after a full page clears it and starts at the top again, so words never move once shown
// (scrolling them up while the voice played was hard to read).

inline int pageCount(int lines, int perPage) {
  return lines <= perPage ? 1 : (lines + perPage - 1) / perPage;
}

// The page holding the newest line.
inline int livePage(int lines, int perPage) {
  return lines <= 0 ? 0 : (lines - 1) / perPage;
}

// The page to draw: `chosen` (swiped to), or -1 = follow the newest.
inline int shownPage(int chosen, int lines, int perPage) {
  const int last = pageCount(lines, perPage) - 1;
  return chosen < 0 || chosen > last ? livePage(lines, perPage) : chosen;
}
