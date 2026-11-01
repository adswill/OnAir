// GK-2A (GEO-KOMPSAT-2A, Korea, 128.2 E) LRIT and HRIT telemetry. The two links are separate modes with the same receiver chain:
//   LRIT  1692.14 MHz  BPSK  128 ksym/s   rate 1/2 K=7 + RS(255,223) x4   64 kbps CADUs     (KMA GK2A LRIT mission specification, Table 9.1)
//   HRIT  1695.4  MHz  QPSK    3 Msym/s   the same coding                  3 Mbps CADUs      (KMA GK2A HRIT mission specification, Table 9.1)
// Root raised cosine, roll-off 0.5, linear polarisation (east-west).
#pragma once
#include "dect2/goes/goes_tel.h"
#include <cstdio>
#include <string>

namespace dect2 {

constexpr double kGk2aLritMhz = 1692.14, kGk2aHritMhz = 1695.4;
constexpr double kGk2aLritSymbolRate = 128e3, kGk2aHritSymbolRate = 3e6;

struct Gk2aTelemetry : GoesTelemetry {
    int link = 0;                 // 0 LRIT, 1 HRIT
    int keysLoaded = 0;           // DES keys the receiver holds
    int segmentsLocked = 0;       // picture segments waiting for a key
    std::string hint;             // why nothing decodes yet, or empty
};

inline const char* gk2aLinkName(int link) { return link ? "GK-2A HRIT" : "GK-2A LRIT"; }
inline double gk2aSymbolRate(int link) { return link ? kGk2aHritSymbolRate : kGk2aLritSymbolRate; }
inline double gk2aMhz(int link) { return link ? kGk2aHritMhz : kGk2aLritMhz; }
// the slowest sample rate the chain works at (2.1 samples a symbol)
inline double gk2aMinRate(int link) { return 2.1 * gk2aSymbolRate(link); }

// One line that says what to do next. `running`: the receiver gets samples; `rate`: the sample rate they come at.
inline std::string gk2aHint(const Gk2aTelemetry& t, bool running, double rate) {
    char b[320];
    const int link = t.link;
    if (!running) return "";
    if (rate > 0 && rate < gk2aMinRate(link) - 1) {
        snprintf(b, sizeof b, "The sample rate (%.2f Msps) is too low: %s needs at least %.2f Msps (2.1 samples a symbol). Raise it in the radio settings.", rate / 1e6, gk2aLinkName(link), gk2aMinRate(link) / 1e6);
        return b;
    }
    if (t.state == 0) {
        if (t.levelDb > -5.f) return "The input is very strong (near full scale): lower the gain, a clipped signal does not decode.";
        snprintf(b, sizeof b, "No carrier found. GK-2A sits at 128.2 E: above the horizon from East Asia, Australia and the western Pacific, not from Europe. Point a dish with an LNA at it and tune %.2f MHz; a small antenna cannot hear it.", gk2aMhz(link));
        return b;
    }
    if (t.state == 1) {
        if (t.snrDb < 2.f) {
            snprintf(b, sizeof b, "Carrier found but weak (Es/N0 %.1f dB): decoding needs about %.0f dB. Re-aim the dish, add LNA gain or use a larger dish.", t.snrDb, link ? 2.0 : -1.0);
            return b;
        }
        return "Carrier locked; looking for the frame sync word. Wait a few seconds; if it never syncs the polarity or symbol rate is off (the receiver tries every polarity and I/Q order itself).";
    }
    if (t.state == 2) return "Frames are found but Reed-Solomon fails: the signal is too weak for clean frames. Improve the signal-to-noise ratio.";
    if (t.keysLoaded == 0 && t.segmentsLocked > 0)
        return "Pictures are DES-encrypted by KMA. Import your KMA decryption key file (Products tab, Import keys) to see them; text files and the raw .lrit files are saved anyway.";
    if (t.segmentsLocked > 0) return "Some pictures use a key that is not in the imported key file.";
    return "";
}

inline std::string gk2aSummary(const Gk2aTelemetry& t) {
    char b[240];
    const char* n = gk2aLinkName(t.link);
    if (t.state >= 2) {
        snprintf(b, sizeof b, "%s: %llu frames (%llu bad), %zu products, Es/N0 %.1f dB", n, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.products.size(), t.snrDb);
        return b;
    }
    if (t.state == 1) { snprintf(b, sizeof b, "%s: carrier locked (%.1f kHz), looking for frames", n, t.cfoHz / 1000); return b; }
    snprintf(b, sizeof b, "%s: searching for the %.0f ksym/s %s carrier", n, gk2aSymbolRate(t.link) / 1e3, t.link ? "QPSK" : "BPSK");
    return b;
}

} // namespace dect2
