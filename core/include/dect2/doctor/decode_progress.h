// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Decode help on top of the Reception Doctor: how far each decoder got (signal, carrier, sync, FEC), when to open the doctor by itself, the clip of a signal
// that failed (only when the user agrees to that one clip), and the antenna and gain coach. Pure logic, tests in tests/doctor/test_decode_help.cpp.
#pragma once
#include "dect2/doctor/reception_doctor.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::doctor {

enum class StageState { Unknown, No, Partial, Yes };

struct StageInput {
    bool samples = false;       // samples are arriving
    int syncState = -1;         // the receiver's own state: -1 not reported, 0 searching, 1 partly locked / signal seen, 2 locked
    int64_t blocksOk = -1, blocksBad = -1;   // frames that passed / failed the error check since the start, -1 not reported
};

struct Progress {
    StageState signal = StageState::Unknown, carrier = StageState::Unknown, sync = StageState::Unknown, fec = StageState::Unknown;
    int furthest = -1;          // 0 signal, 1 carrier, 2 sync, 3 FEC: the last stage that was reached (-1 none)
};

// `rep` may be invalid (the doctor has not looked yet): the carrier stage is then Unknown. Reaching a later stage implies the earlier ones:
// a decoder delivering good frames has a carrier, whatever the doctor saw in the short capture.
Progress progress(const Report& rep, const StageInput& in);

// Opens the doctor by itself once per mode and receiver start, when a live radio has run for a while and the decoder has got nowhere.
class AutoOpen {
public:
    static constexpr double kAfterSec = 25;
    // now: seconds, monotonic. running: a live radio receiver is running. decoding: locked or producing frames. mode: the current mode (a change starts over)
    bool update(double now, bool running, bool decoding, const std::string& mode);
private:
    double since_ = -1;
    std::string mode_;
    bool done_ = false;
};

// A clip of the failed signal, as 8-bit interleaved I/Q and a SigMF meta file. Written only after the user agreed to this one clip in the app.
struct ClipInfo {
    double rateHz = 0, freqHz = 0;
    std::string modeId, note;
};
constexpr size_t kMaxClipSamples = 8000000;    // 16 MB of cs8
size_t clipBytes(size_t samples);              // the size of the data file
// Writes <basePath>.sigmf-data and <basePath>.sigmf-meta (no location, no receiver identity, no date beyond what the user can see). False when a write fails.
bool writeClip(const std::string& basePath, const cf32* x, size_t n, const ClipInfo& info, std::string* err = nullptr);
std::string clipMeta(const ClipInfo& info);    // the meta file's JSON

// Antenna and gain coach: short advice from the doctor's numbers, the most useful first. antennaKnown false: the user entered no antenna (My station).
enum class Advice { GainDown, GainUp, GainGood, LnaHelps, NeedsAntennaForBand, AntennaOk, MoveAntenna, NoAdvice };
struct Coaching {
    std::vector<Advice> advice;
    double gainDeltaDb = 0;     // GainDown / GainUp: how far
};
Coaching coach(const Report& rep, const Context& c, bool antennaKnown, bool antennaForBand);

} // namespace dect2::doctor
