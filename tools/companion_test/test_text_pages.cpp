// uses:
// A long reply is shown a page of lines at a time, so the words never move while they appear.
#include "check.h"
#include "text_pages.h"

int main() {
  CHECK_EQ("pages: none for no lines", pageCount(0, 3), 1);
  CHECK_EQ("pages: 3 lines fit one page", pageCount(3, 3), 1);
  CHECK_EQ("pages: the 4th line starts a second", pageCount(4, 3), 2);
  CHECK_EQ("live: the first page while it fills", livePage(2, 3), 0);
  CHECK_EQ("live: a full page stays", livePage(3, 3), 0);
  CHECK_EQ("live: the next line clears it", livePage(4, 3), 1);
  CHECK_EQ("live: seventh line, third page", livePage(7, 3), 2);
  CHECK_EQ("shown: -1 follows the newest page", shownPage(-1, 7, 3), 2);
  CHECK_EQ("shown: a chosen page", shownPage(1, 7, 3), 1);
  CHECK_EQ("shown: past the end is the last page", shownPage(9, 7, 3), 2);
  return failures ? 1 : 0;
}
