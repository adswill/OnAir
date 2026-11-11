// Ham science modes: telemetry shared by the receiver and the screens (one structure for all ten modes, `kind` says which fields count).
// Receive only; nothing is transmitted.
#pragma once
#include "dect2/hamsci/hamsci_dsp.h"
#include "dect2/hamsci/hamsci_evt.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

// kinds (HamsciReceiver(kind)): family numbers 460 .. 465, 500 .. 502, 750 (JT6M, 3.9) and 820, 823 .. 835 (the event monitors, 3.9), engine standard = family + 2, poll slot 300 + kind
enum HamsciKind { kHsQrss = 0, kHsDfcw, kHsFsk441, kHsIscat, kHsPi4, kHsThrob, kHsSid, kHsRiometer, kHsPulsar, kHsJt6m,
                  kHsSchumann, kHsWhistler, kHsEme, kHsJove, kHsCallisto, kHsAurora, kHamsciKinds };   // 10 .. 15: the event monitors (3.9, hamsci_evt.h), families 820 + 3 * (kind - 10)
constexpr bool hamsciEvtKind(int kind) { return kind >= kHsSchumann; }
inline int hamsciFamily(int kind) { return kind < 6 ? 460 + kind : kind < 9 ? 500 + (kind - 6) : kind == 9 ? 750 : 820 + 3 * (kind - 10); }
inline int hamsciKindOfFamily(int fam) { return fam >= 460 && fam <= 465 ? fam - 460 : fam >= 500 && fam <= 502 ? 6 + (fam - 500) : fam == 750 ? 9 : fam >= 820 && fam <= 835 && (fam - 820) % 3 == 0 ? 10 + (fam - 820) / 3 : -1; }
inline int hamsciStd(int kind) { return hamsciFamily(kind) + 2; }
inline int hamsciKindOfStd(int std) { return hamsciKindOfFamily(std - 2); }
inline int hamsciKindOfActive(int a) { return hamsciKindOfFamily(a - 1); }   // the active standard the engine reports is the family + 1
constexpr bool hamsciAudioKind(int kind) { return kind <= kHsThrob || kind == kHsJt6m; }   // the digital modes: audio in, a list of decodes out

struct HamsciLine {
    double timeSec = 0;        // signal time of the decode
    int64_t utc = 0;           // wall clock of the capture start + timeSec
    std::string text;
    double freqHz = 0;         // audio frequency of the signal (or tuning error)
    double snrDb = 0;
    std::string note;          // variant, sync score, ...
};

struct HamsciTelemetry {
    uint64_t seq = 0;
    int kind = 0;
    int state = 0;                  // 0 no samples, 1 listening / measuring, 2 signal seen, 3 decoding / event
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    float levelDb = -200;           // input or audio level, dBFS
    double inputRate = 0, centerHz = 0, timeSec = 0;
    bool autoParams = true;         // the automatic settings are on
    std::string found;              // what Auto found, for the status line ("12 s dots, tone at 1234 Hz")
    // digital modes
    std::vector<HamsciLine> lines;  // newest first
    std::string text;               // running text of the slow keyboard modes and the grabbers
    int variant = -1;               // Throb variant, ISCAT variant
    // QRSS / DFCW
    hamsci::QrssTelemetryCore qrss;
    // SuperSID
    hamsci::SidState sid;
    // riometer
    hamsci::RioState rio;
    // pulsar
    hamsci::PulsarResult pulsar;
    double pulsarObservedSec = 0;
    bool pulsarSearching = false;
    double pulsarNeededM2 = 0;      // the radiometer equation for the target set, 0 = unknown
    // event monitors (kinds 10 .. 15): spectrogram, trend, peaks, counts; the events themselves are `lines` (newest first, up to 2000)
    hamsci::EvtState evt;
    int64_t startUtc = 0;           // wall clock of the capture start (the events' times are signal times)
};

}  // namespace dect2
