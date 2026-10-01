// Input classification into calendar events.
#ifndef GENDA_CLASSIFY_H
#define GENDA_CLASSIFY_H

#include "common.h"

// TODO: real classifier (LLM) lands here. Mock = keyword heuristic only.
// Stores nothing; returns 0 always (result is in *c).
int classifyInput(const Input *in, Classified *c);

#endif
