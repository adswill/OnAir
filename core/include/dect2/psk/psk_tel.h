// PSK31 / PSK63 receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kPskModes = 7;                 // 0 BPSK31, 1 BPSK63, 2 QPSK31, 3 QPSK63, 4 BPSK125, 5 BPSK250, 6 QPSK125 (new ones at the end: saved prefs keep their meaning)
inline const char* pskModeName(int m) { static const char* n[kPskModes] = {"BPSK31", "BPSK63", "QPSK31", "QPSK63", "BPSK125", "BPSK250", "QPSK125"}; return m >= 0 && m < kPskModes ? n[m] : "?"; }
inline double pskBaud(int m) { static const double b[kPskModes] = {31.25, 62.5, 31.25, 62.5, 125, 250, 125}; return m >= 0 && m < kPskModes ? b[m] : 31.25; }
inline bool pskQpsk(int m) { return m == 2 || m == 3 || m == 6; }

// One line of a signal's chat log: the characters between two line ends (or a pause)
struct PskLine {
    double timeSec = 0;          // signal seconds of its first character
    std::string text;
};

// One signal in the window: its own mixer, matched filter, symbol clock, AFC and decoder
struct PskSignal {
    int id = 0;                  // stable while the signal is followed
    double offsetHz = 0;         // from the dial frequency (AFC: where the decoder is now)
    double afcHz = 0;            // how far the AFC moved it from where it was found
    float snrDb = 0;             // in the signal's bandwidth
    float quality = 0;           // 0 .. 1: how clean the phase decisions are (the DCD)
    bool dcd = false;            // a PSK signal is being decoded (text comes out)
    bool manual = false;         // picked by the user, kept while it is the focus
    double lastActiveSec = 0;    // signal seconds of the last character
    double firstSec = 0;
    uint64_t chars = 0;
    std::string text;            // the newest kPskTextMax characters
    std::vector<PskLine> lines;  // the chat log, oldest first (the last one may still be growing)
    std::vector<std::string> calls;   // callsigns in the text, newest last
};

constexpr size_t kPskTextMax = 2000;
constexpr size_t kPskLinesMax = 60;
constexpr int kPskMaxSignals = 24;

struct PskTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing heard, 1 signals in the window, 2 decoding text
    double cfoHz = 0;            // AFC of the focused signal
    float snrDb = 0;             // of the focused (else the strongest) signal
    float levelDb = -200;        // input power, dBFS
    bool dataValid = false;      // some signal is decoding
    uint64_t blocksOk = 0;       // characters decoded
    uint64_t blocksBad = 0;      // codes not in the table
    // details
    int mode = 0;                // the modulation looked for (kPskModes)
    bool autoOn = false, autoLocked = false;   // Auto is on / it found a modulation that gives text
    int autoMode = -1;           // what it found (kPskModes), -1 none yet
    double timeSec = 0;
    double inputRate = 0;
    double loHz = 0, hiHz = 0;   // the window: dial + loHz .. dial + hiHz (the audio passband of an upper sideband receiver)
    float floorDb = -200;        // noise floor of the window spectrum, dB per bin
    int focusId = -1;
    std::vector<PskSignal> signals;    // by frequency
    std::vector<float> spectrum;       // dB per bin, loHz .. hiHz, for the waterfall
    uint64_t specSeq = 0;
};

inline const PskSignal* pskFocus(const PskTelemetry& t) {
    for (const auto& s : t.signals) if (s.id == t.focusId) return &s;
    return nullptr;
}

inline std::string pskSummary(const PskTelemetry& t) {
    char b[200];
    int dec = 0;
    for (const auto& s : t.signals) if (s.dcd) dec++;
    if (t.signals.empty()) { snprintf(b, sizeof b, "searching for %s", pskModeName(t.mode)); return b; }
    const PskSignal* f = pskFocus(t);
    if (f) snprintf(b, sizeof b, "%s: %zu signals, %d decoding; focus %.0f Hz, %.0f dB", pskModeName(t.mode), t.signals.size(), dec, f->offsetHz, f->snrDb);
    else snprintf(b, sizeof b, "%s: %zu signals, %d decoding", pskModeName(t.mode), t.signals.size(), dec);
    return b;
}

} // namespace dect2
