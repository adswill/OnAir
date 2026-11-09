// Wideband LoRa survey: one capture of a few MHz in, a table of every LoRa transmission found in it out, whatever the spreading factor, bandwidth,
// coding rate or sync word, with a guess at which network each belongs to (Meshtastic preset, MeshCore, LoRaWAN plan channel, LoRa APRS, other).
// The user does not pick a preset first: a spectral activity pass finds the occupied channels (centre and width), the width limits the bandwidths
// to try, and a dechirp bank per channel and bandwidth runs every spreading factor on the same channel stream (the machinery of the Signal Lab
// auto search, mesh_lora.h). Receive only; payloads are not interpreted or decrypted here.
#pragma once
#include "dect2/mesh/mesh_lora.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace lora {

struct SurveyOptions {
    double maxSeconds = 30;      // how much of the capture is searched
    double centerHz = 0;         // the radio's centre frequency (0: unknown: frequencies are offsets only and the plans cannot be matched)
    bool lowSf = false;          // also SF5 and SF6
    bool tryInverted = true;     // search the empty channels again with inverted IQ (LoRaWAN downlinks)
    double activityDb = 0;       // extra margin in dB over the statistical detection threshold (raise it to find fewer, stronger signals)
    size_t maxBanks = 24;        // upper bound on (channel, bandwidth) combinations dechirped
};

struct SurveyChannel { double offsetHz = 0, widthHz = 0, peakDb = 0; };   // an occupied stretch of spectrum

struct SurveyEntry {
    double offsetHz = 0;         // of the channel centre in the capture
    double freqHz = 0;           // centerHz + offsetHz plus the median carrier error (0 when centerHz is unknown)
    double bwHz = 0;
    int sf = 0, cr = 0, syncWord = 0;
    bool invertIq = false;
    bool decoded = false;        // at least one frame came out (else only preambles were heard)
    int frames = 0, crcOk = 0, preambles = 0;
    double snrDb = 0;            // median over the frames
    double cfoHz = 0;            // median carrier error of the frames
    double firstSec = 0, lastSec = 0;
    double airtimeSec = 0;       // summed over the frames
    double dutyPct = 0;          // airtime / the searched duration
    std::string network;         // "Meshtastic LongFast", "LoRaWAN EU868", "LoRa APRS", "MeshCore EU/UK (Narrow)", "private LoRa (sync 0x12)", ...
    std::string detail;          // why it was classified so (the matching channel, preset or sync word)
    std::vector<uint8_t> sample; // payload of the first good frame (at most 32 bytes)
};

struct SurveyResult {
    std::vector<SurveyChannel> channels;   // the occupied channels the spectral pass found
    std::vector<SurveyEntry> entries;      // best first: frames with a good CRC, then frames, then preambles
    double seconds = 0;                    // how much of the capture was searched
    size_t banks = 0;                      // (channel, bandwidth) combinations dechirped
    std::string note;
};

// The occupied stretches of the spectrum, one per LoRa bandwidth that fits (matched smoothing of the burst maximum against a local noise floor),
// strongest first; widthHz is the bandwidth of the scale that fired.
std::vector<SurveyChannel> findActiveChannels(const cf32* x, size_t n, double fs, double marginDb = 6);
// Which network a signal belongs to; `centerFreqHz` 0 = unknown. Fills network and detail of the entry.
void classifyLora(SurveyEntry& e);

SurveyResult loraSurvey(const cf32* x, size_t n, double fs, const SurveyOptions& o = {});

} // namespace lora
} // namespace dect2
