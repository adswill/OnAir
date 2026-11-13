// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Multitone paging detector telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kMtTxMax = 200;
constexpr size_t kMtBurstMax = 100;
constexpr size_t kMtToneMax = 24;

// A steady tone heard in the audio (the discriminator output), clustered by frequency (6 Hz)
struct MtTone {
    float hz = 0;
    int count = 0;                      // how many times it was heard
    float totalSec = 0;
    double lastSec = 0;
};

// One transmission: the carrier was up, then it went away.
struct MtTx {
    uint64_t id = 0;
    int64_t wall = 0;
    double timeSec = 0, durSec = 0;
    float snrDb = 0;
    float devHz = 0;                    // FM deviation (peak) of the audio
    std::vector<float> lineHz, lineDb;  // spectral lines of the audio, strongest first (dB over the audio floor)
    std::string tones;                  // steady tones in order of arrival: "474.6 Hz 0.50 s; 238.3 Hz 0.95 s"
    int toneCount = 0;
    float periodMs = 0, periodStrength = 0;   // a repeating pattern in the audio (autocorrelation peak, 0..1); 0 none
    uint64_t burst = 0;                 // id of the FSK data found in it (0 none)
    std::string kind;                   // "tone sequence", "multi-tone audio", "repeating multi-tone audio", "FSK data", "other"
};

// FSK-like data found in a transmission: what is measured, never what it means
struct MtBurst {
    uint64_t id = 0, txId = 0;
    int64_t wall = 0;
    double timeSec = 0, durSec = 0;
    double baud = 0;                    // measured symbol rate
    std::string nearestStd;             // "1200" when within 1.5% of 512 / 1200 / 2400 and so on, else ""
    double coherence = 0;               // 0..1: how well the edges sit on the bit clock
    int pol = 1;                        // polarity of the best alignment: +1 higher frequency = 1
    int nBits = 0;                      // sliced symbols
    bool ffsk = false;                  // audio FFSK: tones f and 2f below
    float toneLoHz = 0, toneHiHz = 0;
    int syncCount = 0;                  // how often the 16-bit word 0x605A (or its complement, up to 1 bit wrong) is in the stream
    std::vector<int> sync16At;          // bit offsets of the first 8
    std::string afterSync;              // the 16-bit words that follow the first double 0x605A (or the first one): hex, as seen
    bool manchester = false;            // every pair of symbols was 01 or 10: the words below are from the decoded bits
    int dataBits = 0;                   // bits after Manchester decoding (= nBits when not coded)
    std::string bitsHex;                // the first bytes of the bit stream (polarity +1, as sliced)
    int validWords = 0, totalWords = 0; // 32-bit words with a valid BCH(31,21) + parity check, at the best alignment
    int bestRun = 0;                    // most consecutive valid words
    int repeatBits = 0;                 // the bit stream repeats with this period (0 none); a preamble or an idle pattern, not a message
    int align = 0;                      // bit offset of the best alignment
    bool syncFound = false;             // the POCSAG sync word 0x7CD215D8 (up to 2 bit errors) is in the stream
    int syncAt = -1;                    // its bit offset
    std::vector<uint32_t> words;        // up to 32 words from the best alignment
    std::vector<uint8_t> wordOk;
    std::string verdict;                // what can and cannot be said about it
    // Framing measured from the bit stream (analyseFraming; what repeats, never what it means)
    int dupBits = 1;                    // every bit was sent this many times in a row (2: the audio FFSK of the recordings sends each bit twice)
    double dataBaud = 0;                // baud / dupBits
    int markerBits = 0;                 // length of a marker pattern that comes back at a fixed spacing (0 none)
    std::string marker;                 // as 0 / 1 characters
    int markerCount = 0, markerGap = 0; // how often, and the usual distance between two (bits, after collapsing)
    std::vector<std::string> frameWords;// the bits between a marker and the next position one gap on, up to 64, as 0 / 1 characters
    int doubledWords = 0;               // of those, how many equal the one before (the words come in repeated pairs)
    int distinctWords = 0;
    int halfEqualWords = 0;             // of those, how many have their last two halves (after a first byte) equal within one bit: a value sent twice
};

struct MultitoneTelemetry {
    uint64_t seq = 0;
    int state = 0;                      // 0 nothing heard, 1 a carrier is up now or one ended in the last 2 s
    double cfoHz = 0;                   // unused
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;             // a transmission was logged
    uint64_t blocksOk = 0;              // BCH-valid 32-bit words found
    uint64_t blocksBad = 0;             // 32-bit words of FSK data that were not valid
    // details
    double timeSec = 0, inputRate = 0;
    bool signal = false;
    uint64_t txTotal = 0, burstTotal = 0;
    std::vector<MtTone> tones;          // heard so far, most often first
    std::vector<MtTx> tx;               // newest first
    std::vector<MtBurst> bursts;        // newest first
};

inline std::string multitoneSummary(const MultitoneTelemetry& t) {
    char b[200];
    if (t.tx.empty()) { snprintf(b, sizeof b, "Multitone: listening, level %.1f dBFS", t.levelDb); return b; }
    const MtTx& x = t.tx.front();
    snprintf(b, sizeof b, "Multitone: %llu transmissions, last: %s%s (unverified)", (unsigned long long)t.txTotal, x.kind.c_str(), t.bursts.empty() ? "" : ", FSK data seen");
    return b;
}

} // namespace dect2
