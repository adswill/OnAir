// Voiceband modes of the W1A batch (families 800 - 802): HF packet 300 Bd, voice inversion scrambler, ELT 121.5 MHz. Telemetry shared by the
// receiver and the screens. The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

constexpr int kW1aFamily0 = 800;     // family of kind 0 (w1a::Kind order: HF packet 300 Bd, voice inversion, ELT)
constexpr int kW1aKinds = 3;
constexpr size_t kW1aEventsMax = 60;
constexpr size_t kW1aTextMax = 12000;

struct W1aEvent {
    int64_t utc = 0;
    double timeSec = 0;
    std::string text;
};

struct W1aTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 signal found, 2 decoded / confirmed
    double cfoHz = 0;            // packet: the tone offset found; inversion: the carrier found, Hz; ELT: sweep rate, Hz
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // packet: frames with a good / bad FCS; inversion: speech frames judged; ELT: sweeps counted
    int kind = 0;                // w1a::Kind
    std::string mode;            // one line: what was found
    std::vector<std::pair<std::string, std::string>> params;
    std::string text;            // packet: the newest frames, one per line
    std::vector<W1aEvent> events;   // newest first
    double timeSec = 0, inputRate = 0;
    std::vector<float> spec;     // inversion: the long-term audio spectrum, dB, 0 .. 4.8 kHz in 50 Hz steps
    bool recording = false;      // inversion: the descrambled audio is being written to a WAV file
    std::string recordPath;
};

inline const char* w1aName(int kind) {
    static const char* n[kW1aKinds] = {"HF packet 300 Bd", "Voice inversion scrambler", "ELT 121.5 MHz"};
    return kind >= 0 && kind < kW1aKinds ? n[kind] : "W1A";
}
// What the mode reads (shown in the list and the panel): "full decode" of the frames, "detection only" for the other two
inline const char* w1aScope(int kind) {
    static const char* n[kW1aKinds] = {"full decode", "detection and descrambling of full-band inversion", "detection only"};
    return kind >= 0 && kind < kW1aKinds ? n[kind] : "";
}

inline std::string w1aSummary(const W1aTelemetry& t) {
    std::string s = std::string(w1aName(t.kind)) + ": " + (t.mode.empty() ? "searching" : t.mode);
    if (s.size() > 120) s.resize(120);
    return s;
}

} // namespace dect2
