// uses: cartridges/companion/script_writer.cpp cartridges/companion/companion_store.cpp cartridges/companion/moment_lines.cpp cartridges/companion/moment_store.cpp cartridges/companion/moment_schedule.cpp cartridges/companion/persona.cpp
// The recording of a moment's line, step by step: written by the text model, read by Live (in chunks of
// ~100 words), saved; any interruption ends it Failed so the moment stays not ready.
#include "check.h"
#include "line_job.h"

static bool has(const std::string &s, const std::string &part) {
  return s.find(part) != std::string::npos;
}

static Written ok(const std::string &text) {
  Written w;
  w.ok = true;
  w.text = text;
  return w;
}

int main() {
  LineWork work;
  work.companion = "friend";
  work.key = "3";
  work.ids = {3};
  work.style = "calm";
  {
    std::string text;
    for (int s = 0; s < 25; s++) text += "This is sentence number " + std::to_string(s) + " of ten words here. ";  // 250 words
    LineJob j;
    j.start(work);
    const bool writing = j.step() == LineJob::Step::Writing;
    j.written(ok(text));
    const bool waiting = j.step() == LineJob::Step::WaitSession;
    std::string note;
    const bool early = j.takeSend(note);
    j.sessionReady();
    std::vector<std::string> notes;
    while (j.step() == LineJob::Step::Reading) {
      if (j.takeSend(note)) notes.push_back(note);
      j.chunkDone();
    }
    const bool saving = j.step() == LineJob::Step::Saving;
    j.saved(true);
    CHECK("happy path: Writing -> WaitSession -> Reading -> Saving -> Done",
          writing && waiting && !early && saving && j.step() == LineJob::Step::Done && notes.size() == 3 &&
              has(notes[0], "word for word") && has(notes[0], "calm") && has(notes[1], "Carry on") &&
              has(notes[2], "Carry on") && j.text() == text.substr(0, text.size() - 1));
  }
  {
    LineJob j;
    j.start(work);
    Written failed;
    failed.error = "HTTP 500";
    j.written(failed);
    CHECK("written failure -> Failed", j.step() == LineJob::Step::Failed);
  }
  {
    LineJob j;
    j.start(work);
    j.written(ok("Time to rest."));
    std::string note;
    CHECK("no send before sessionReady", !j.takeSend(note) && j.step() == LineJob::Step::WaitSession);
  }
  {
    LineJob j;
    j.start(work);
    j.written(ok("Time to rest. And drink water."));
    j.sessionReady();
    std::string note;
    j.takeSend(note);
    j.abort();
    CHECK("abort from Reading -> Failed, takeSend gives nothing", j.step() == LineJob::Step::Failed && !j.takeSend(note));
  }
  {
    LineJob j;
    j.start(work);
    j.written(ok("Time to rest."));
    j.sessionReady();
    std::string note;
    j.takeSend(note);
    j.chunkDone();
    j.saved(false);
    CHECK("saved(false) -> Failed", j.step() == LineJob::Step::Failed);
  }
  {
    LineJob j;
    j.abort();
    CHECK("abort while idle stays idle", j.step() == LineJob::Step::Idle);
  }
  CHECK("readingCapMs: half a second a word plus 30 s (300 words fit)",
        readingCapMs(40) == 50000 && readingCapMs(300) == 180000 && readingCapMs(300) > 300 * 400 + 6 * 2500);
  return failures ? 1 : 0;
}
