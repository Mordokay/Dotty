#pragma once

// The Companion's packs on the card (spec: docs/superpowers/specs/2026-11-03-companion-make-room-design.md):
// the avatars but the robot (data/avatars.bin, drawn from a PSRAM copy) and the voice samples (unpacked into
// data/voices/<name>.mp3). A pack that's missing or from another firmware (its stamp, data/assets/<stem>.stamp,
// isn't the manifest's SHA-256) is downloaded over Wi-Fi by a background task while Dotty is idle, checked
// (size, SHA-256), installed, then stamped; rules in asset_plan.h.
namespace assets {

void begin();                // setup, after storage: reads the stamps, loads the avatars
void poll(bool idle);        // every loop pass (locked too): starts a due download while idle, stops one that
                             // isn't any more, takes a finished one over
void stop(const char *why);  // before anything that needs Wi-Fi or a session: ends a download (≤ ~5 s)
bool busy();
bool takeChanged();          // the avatars changed since the last call: redraw
const char *state(const char *stem);  // "ready" | "downloading" | "missing"
bool voicesReady();

}  // namespace assets
