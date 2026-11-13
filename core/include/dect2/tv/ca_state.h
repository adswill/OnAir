// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Is a TV service really scrambled? Decided from evidence, never from the flags alone (free_CA_mode and CA descriptors are set on free
// services too). Order of trust: the scrambling bits of the packets of its audio/video PIDs, then whether the player decodes it, then the flags
// as a hint. Nothing is descrambled.
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 {

enum class CaState { Unknown = 0, Clear, CaFlagClear, Scrambled };

struct CaStreamEvidence {
    bool video = false;                  // video stream (otherwise audio)
    uint32_t scrambledPackets = 0;       // recent payload packets with transport_scrambling_control != 0
    uint32_t clearPackets = 0;           // recent payload packets with 00
};

struct CaEvidence {
    std::vector<CaStreamEvidence> streams;   // the audio and video streams of the service
    bool flagged = false;                    // free_CA_mode set or a CA descriptor seen (weak hint)
    bool decodes = false;                    // the player is decoding frames or audio of this service right now
};

inline constexpr uint32_t kCaMinPackets = 4;   // payload packets of a stream before it counts as examined

inline CaState classifyCa(const CaEvidence& e) {
    const CaState clear = e.flagged ? CaState::CaFlagClear : CaState::Clear;
    if (e.decodes) return clear;
    bool anyVideoScr = false, anyAudioScr = false, anyAudioClear = false, examined = false;
    for (const CaStreamEvidence& s : e.streams) {
        if (s.scrambledPackets + s.clearPackets < kCaMinPackets) continue;
        examined = true;
        const bool scr = s.scrambledPackets >= kCaMinPackets && s.scrambledPackets > s.clearPackets;
        if (s.video) anyVideoScr = anyVideoScr || scr;
        else if (scr) anyAudioScr = true; else anyAudioClear = true;
    }
    if (anyVideoScr || (anyAudioScr && !anyAudioClear)) return CaState::Scrambled;
    return examined ? clear : CaState::Unknown;
}

inline const char* caStateName(CaState s) {
    switch (s) {
        case CaState::Clear: return "clear";
        case CaState::CaFlagClear: return "CA flag set, but the stream is clear";
        case CaState::Scrambled: return "scrambled";
        default: return "not checked yet";
    }
}

}  // namespace dect2
