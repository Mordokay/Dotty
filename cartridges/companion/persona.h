#pragma once

#include <string>

#include "companion_store.h"

// Who the companion is told it is, at the start of every session: its profile (named), or the
// waking-up text when it has no name yet. Both end with the acting notes: every line performed with
// the emotion the moment calls for (the user: "like an actor in a theater play").
std::string personaText(const Profile &p);
