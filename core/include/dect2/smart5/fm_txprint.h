// FM transmitter fingerprint (3.9, docs/SMART5-broadcast-3.9.md item 3). Networks reuse one PI on many transmitters; on a DX
// frequency, or when two of them reach you on one channel, the PI alone cannot tell them apart. Each transmitter keeps its own
// carrier offset (a few Hz to some hundred Hz off nominal), pilot injection and processing (multiplex power). observe() learns
// one signature per (PI, frequency) and says when the current one does not match the transmitter heard before.
// The carrier offset is only compared after the radio's own frequency error is removed (cfoHz from a calibrated radio, or the
// same radio on the same day); the tolerance is set by the caller.
#pragma once
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {
namespace smart5 {

struct FmPrintSample { double cfoHz = 0; double pilotPct = 0; double mpxDbr = -99; };   // mpxDbr -99: not measured

struct FmTxSignature {
    int id = 0;               // 1, 2, ... per (PI, frequency)
    int n = 0;                // reports averaged in
    double cfoHz = 0, pilotPct = 0, mpxDbr = -99;
    double sdCfo = 0, sdPilot = 0;
    std::string label;        // user name ("Vilnius TV tower"), empty until set
};

struct FmPrintParams { double cfoTolHz = 8; double pilotTolPct = 0.6; double mpxTolDb = 1.5; int learnReports = 30; };

class FmTxPrint {
public:
    explicit FmTxPrint(FmPrintParams p = {}) : p_(p) {}
    // one report of a locked station; returns the signature id it matched (0 while still learning the first one)
    int observe(int pi, double freqMhz, const FmPrintSample& s);
    // the transmitter matched by the latest reports of (pi, freq); -1 = none yet; changed() is true when it differs from the
    // one heard first, so the UI can say "a different transmitter of this network"
    int current(int pi, double freqMhz) const;
    bool changed(int pi, double freqMhz) const;
    const std::vector<FmTxSignature>* signatures(int pi, double freqMhz) const;
    void setLabel(int pi, double freqMhz, int id, const std::string& name);
    std::string save() const;            // text lines, one per signature
    void load(const std::string& text);
private:
    struct Key { int pi; int f10; bool operator<(const Key& o) const { return pi != o.pi ? pi < o.pi : f10 < o.f10; } };
    struct Slot {
        std::vector<FmTxSignature> sigs;
        std::vector<FmPrintSample> pend;  // reports not yet settled into a signature
        int cur = -1;
    };
    static Key key(int pi, double f) { return {pi, (int)std::lround(f * 10)}; }
    bool matches(const FmTxSignature& g, const FmPrintSample& s) const;
    FmPrintParams p_;
    std::map<Key, Slot> slots_;
};

} // namespace smart5
} // namespace dect2
