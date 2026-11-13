// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// classifyCa: scrambled only on evidence from the packets; a CA flag on a clear stream is not "scrambled".
#include "dect2/tv/ca_state.h"
#include <cstdio>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static CaStreamEvidence st(bool v, uint32_t scr, uint32_t clr) { CaStreamEvidence s; s.video = v; s.scrambledPackets = scr; s.clearPackets = clr; return s; }
int main() {
    { CaEvidence e; e.flagged = true; e.streams = {st(true, 0, 300), st(false, 0, 80)};
      CHECK(classifyCa(e) == CaState::CaFlagClear, "flag set on a clear stream"); }
    { CaEvidence e; e.streams = {st(true, 0, 300), st(false, 0, 80)};
      CHECK(classifyCa(e) == CaState::Clear, "clear"); }
    { CaEvidence e; e.flagged = true; e.streams = {st(true, 300, 0), st(false, 80, 0)};
      CHECK(classifyCa(e) == CaState::Scrambled, "truly scrambled"); }
    { CaEvidence e; e.streams = {st(true, 300, 2), st(false, 0, 80)};
      CHECK(classifyCa(e) == CaState::Scrambled, "video scrambled, audio clear"); }
    { CaEvidence e; e.streams = {st(true, 0, 300), st(false, 80, 0)};
      CHECK(classifyCa(e) == CaState::Scrambled, "video clear, only audio scrambled"); }
    { CaEvidence e; e.streams = {st(true, 0, 300), st(false, 80, 0), st(false, 0, 80)};
      CHECK(classifyCa(e) == CaState::Clear, "a second, clear audio track keeps it clear"); }
    { CaEvidence e; e.streams = {st(true, 2, 600)};
      CHECK(classifyCa(e) == CaState::Clear, "stray scrambled packets"); }
    { CaEvidence e; e.flagged = true; e.streams = {st(true, 1, 1), st(false, 0, 2)};
      CHECK(classifyCa(e) == CaState::Unknown, "too few packets: unknown"); }
    { CaEvidence e; e.flagged = true;
      CHECK(classifyCa(e) == CaState::Unknown, "no streams: unknown"); }
    { CaEvidence e; e.flagged = true; e.decodes = true; e.streams = {st(true, 300, 0)};
      CHECK(classifyCa(e) == CaState::CaFlagClear, "the player decodes it: clear whatever the bits say"); }
    if (!fails) printf("ca_state: all passed\n");
    return fails ? 1 : 0;
}
