// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TX studio (no GUI): make the signal of a transmit mode into memory, measure it, impair it, decode it with the app's own receiver, and write it to a file.
// Nothing here opens a radio or transmits. The window is app/panels/tx_studio.cpp; the pure measurements are in tx_measure.h.
#pragma once
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_measure.h"
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {

enum class StudioFormat { Cu8 = 0, Cs8 = 1, Cs16 = 2, Cf32 = 3, SigMF = 4, WavAudio = 5 };
constexpr int kStudioFormats = 6;
const char* studioFormatName(StudioFormat f);        // "cu8 (unsigned 8-bit)"
const char* studioFormatExt(StudioFormat f);         // ".cu8", ".sigmf-data", ".wav"
// Size of the data file for n complex samples (a WAV audio file has one 16-bit channel). Metadata files are not counted.
uint64_t studioFileBytes(StudioFormat f, uint64_t n);
// The WAV audio format holds the real part of the baseband (the audio of an upper-sideband mode whose tones sit above DC). Offered below 100 kHz sample rate.
bool studioAudioFormatOk(double rateHz);
// The FileFormat a player must be told for a file written in f (WAV audio: none, returns false)
bool studioPlayFormat(StudioFormat f, FileFormat& out);

struct StudioSignal {
    std::vector<cf32> iq;
    double rateHz = 0, seconds = 0;
    bool finite = false;       // the generator ended by itself (a message, a file) before the time asked for
    bool capped = false;       // the sample limit cut the length
    std::string note;
};
constexpr size_t kStudioMaxSamples = 24u << 20;   // 24 M samples (192 MB as cf32)

// The signal of `mode` for r, `seconds` long (a finite mode may end sooner), at rateHz (0: the mode's own choice). Goes through the same output stage and
// limiter as a real transmission. A microphone or camera is never opened (audioSrc 0 becomes the tone). False with err when the generator cannot work.
bool studioGenerate(const TxModeSpec& mode, const TxRequest& r, double rateHz, double seconds, StudioSignal& out, std::string& err);

struct StudioMeasure {
    bool valid = false;
    double rateHz = 0, seconds = 0;
    uint64_t samples = 0;
    double obw99Hz = 0, obw26Hz = 0, centroidHz = 0;
    double peakDb = -200, rmsDb = -200, paprDb = 0, papr999Db = 0;
    txm::Acpr acpr;                  // channel = the 99 % width, offset = one channel width
    txm::Mask mask;
    txm::MaskResult maskResult;
};
StudioMeasure studioMeasure(const StudioSignal& s, const char* modeId);

// Write the signal (and its metadata file) to pathBase + the format's extension. SigMF: .sigmf-data + .sigmf-meta; the others get pathBase + ".meta.json".
// impair / measure may be null. centerHz is only information for the metadata.
struct StudioWriteInfo {
    const TxModeSpec* mode = nullptr;
    const TxRequest* req = nullptr;
    const StudioMeasure* measure = nullptr;
    const txm::Impair* impair = nullptr;
    double centerHz = 0;
    std::string sweepKey; double sweepValue = 0; bool sweep = false;
};
bool studioWrite(const std::string& pathBase, const StudioSignal& s, StudioFormat f, const StudioWriteInfo& info, std::string& dataPath, std::string& metaPath, std::string& err);

// The app's own receiver on the generated signal (an engine fed from a temporary cf32 file). Stops at the first lock or at timeoutSec.
struct StudioLoopback {
    bool ran = false, locked = false, services = false;
    double snrDb = 0;
    uint64_t blocks = 0;
    std::string summary;
};
StudioLoopback studioLoopback(const TxModeSpec& mode, const StudioSignal& s, double timeoutSec, const std::atomic<bool>* cancel);

// Batch: set a numeric parameter by key on a request. Keys: the request's own numbers (wpm, toneHz, baudIdx, shiftIdx, kind, variant, dbm, function, slot, colorCode,
// talkgroup, address, serviceId, bandwidthMhz, deviationHz, preemphUs, lat, lon); anything else goes to TxRequest::opt[key].
void studioSetParam(TxRequest& r, const std::string& key, double v);
// "10, 20, 30" or "10:50:10" (first:last:step) -> values. Empty on a malformed list; at most 200 values.
std::vector<double> studioParseList(const std::string& s);
struct StudioBatchItem { double value = 0; std::string dataPath, metaPath, err; bool ok = false; double seconds = 0; };
// One file per value of `key` (the special keys impair.snr and impair.cfo sweep the impairment instead). Files are named <folder>/<base>_<key>_<value>.
std::vector<StudioBatchItem> studioBatch(const TxModeSpec& mode, const TxRequest& base, const std::string& key, const std::vector<double>& values, double rateHz, double seconds,
                                         const txm::Impair& impair, StudioFormat f, const std::string& folder, const std::string& fileBase, double centerHz,
                                         const std::function<void(size_t, size_t)>& progress, const std::atomic<bool>* cancel);

} // namespace dect2
