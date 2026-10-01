// Input classification into calendar events via Jev (OpenCode Zen System
// One). Dates are found in code; Jev only answers kind and is-deadline.
#ifndef GENDA_CLASSIFY_H
#define GENDA_CLASSIFY_H

#include "common.h"

// Once from main, after curl_global_init and before any thread starts:
// builds the one shared Zen handle from OPENCODE_API_KEY, GENDA_ZEN_URL and
// GENDA_ZEN_MODEL. Returns -1 (logged) when the key is empty or curl fails.
int classifyInit(void);
// One Zen request per input; safe from any thread (calls are serialized).
// Any failure returns the zero Classified (ok == 0), logged. Stores nothing.
Classified classifyInput(const Input *in);

#endif
