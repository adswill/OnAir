// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The "why isn't it decoding" line of a digital voice mode's Receiver tab (dect2/voice/voice_why.h): which stage of the chain (sync, frame identity,
// FEC / CRC) stops the signal and what to try. Its own header: some mode screens define their own kvRow and cannot include ui_common.h.
#pragma once
#include "dect2/voice/voice_why.h"

struct App;
void voiceWhyRow(const App& a, dect2::VoiceWhy w);
