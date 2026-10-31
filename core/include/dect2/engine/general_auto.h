// General / Auto: the state machine behind "tune around and OnAir finds the signal". A background scan (sigid::Capture::startAll, every few seconds,
// on the identifier's own thread) hands in the signals it found; this class turns them into labels that do not flicker (a label shows after it was
// seen twice, or at once when the identifier is very sure, and goes after several misses), keeps the signals it saw outside the current view for a
// while (tune back and the label is offered again), lets the user ignore a label, and runs the Decode / return-to-General rule:
//   - Decode (the button, or Auto-decode when the tuned frequency is close to a sure label) returns the Memory to open: the radio is tuned and set up
//     for the mode, the app stays in General;
//   - while a decoder runs, tuning more than releaseHz away from the signal releases it: the app goes back to General (no decoder, plain spectrum).
// Pure logic (no GUI, no radio, no clock of its own: every call carries "now"), so tests/genauto/test_genauto.cpp can drive it with synthetic
// scans. docs/GENERAL-AUTO.md says how it is used in the app.
#pragma once
#include "dect2/channels/memory_bank.h"
#include "dect2/engine/sig_ident.h"
#include <string>
#include <vector>

namespace dect2 {
namespace genauto {

// one signal as a scan found it
struct Detection {
    double rfHz = 0;               // centre, RF
    double bwHz = 0;               // occupied width
    double snrDb = 0;
    std::string name, modeId, subId, family;   // the best candidate: "DVB-T / T2", mode id, sub-mode id, group
    double confidence = 0;         // 0..1, the identifier's calibrated number
    int verify = 0;                // sigid::VerifyState of the candidate
    bool likely = false;           // sigid::VerdictLikely
    bool bursty = false;           // on the air only part of the time (labels of such signals are kept longer)
    Memory mem;                    // what Decode opens (sigid::toMemory)
};

// the best candidate of an identified signal as a Detection; false when the identifier does not know what it is, or nothing decodes it
bool fromResult(const sigid::Result& r, double rfHint, Detection& out);
// the same for what Capture::resultAll gives; rfOfCentreHz the RF of the samples' centre
bool fromFound(const sigid::Capture::Found& f, double rfOfCentreHz, Detection& out);

struct Config {
    double minConfidence = 0.45;       // below this a candidate is not worth a label
    int confirmScans = 2;              // scans a signal must be seen in before its label shows ...
    double instantConfidence = 0.90;   // ... unless the identifier is this sure (and the candidate verified)
    int dropScans = 3;                 // scans in a row without the signal (while its place was in view) before the label goes
    int dropScansBursty = 6;           // the same for signals that are only on part of the time
    double scanPeriodSec = 5;          // pause between the end of a scan and the next one
    double settleSec = 2.0;            // after a Decode or a retune: no release / no scan for this long (the radio is still moving)
    double forgetSec = 600;            // labels outside the view are kept this long
    size_t maxLabels = 32;
    bool autoDecode = false;           // decode by itself when the tuned frequency sits on a sure label (off by default)
    double autoConfidence = 0.80;
    double releaseHz = 0;              // keep decoding while the tuned frequency is within this of the signal; 0: automatic (releaseFor)
    double minReleaseHz = 25e3;
    double releaseWidthShare = 0.25;   // automatic release distance: this share of the signal's width, at least minReleaseHz
};

// the release distance for a signal of this width under this configuration
double releaseFor(const Config& c, double bwHz);

struct Label {
    int id = 0;
    Detection d;                   // the latest (name, width and confidence are the smoothed ones)
    double confidence = 0;         // smoothed
    int hits = 0, misses = 0;      // scans seen in / missed in a row
    bool shown = false;            // hysteresis passed: draw it
    bool inView = false;           // its place was in the last scan's span
    bool ignored = false;
    bool decoding = false;
    double firstSec = 0, lastSeenSec = 0;
    std::string pendName;          // a different name seen once; adopted when it repeats
    int pendCount = 0;
};

enum class State { General, Decoding };
enum class Action { None, Decode, ReturnToGeneral };
struct Step {
    Action act = Action::None;
    Memory mem;                    // Decode: what to open
    int labelId = 0;
    std::string note;              // for the status line: "Decoding DVB-T / T2", "Tuned away from DVB-T / T2: back to General"
};

class GeneralAuto {
public:
    GeneralAuto() = default;
    Config cfg;

    // ---- the scan loop
    // true when the app should start a scan now (General, settled, the period over, no scan running)
    bool wantScan(double nowSec) const;
    void scanStarted(double nowSec);
    void scanAborted() { scanning_ = false; }     // the result was dropped (stale, or the capture was taken by the Identify window)
    // a scan finished: its span (RF, from spanLoHz to spanHiHz) and the signals found
    void scanDone(double nowSec, double spanLoHz, double spanHiHz, const std::vector<Detection>& found);
    // the radio was retuned by the user (or the sample rate changed): scan again soon, forget which labels are in view
    void retuned(double nowSec);
    // a scan result for a different centre than the radio's now (late result): the caller drops it; this tells it
    bool resultStale(double scanCentreHz, double radioCentreHz, double tolHz = 1.0) const { return scanCentreHz - radioCentreHz > tolHz || radioCentreHz - scanCentreHz > tolHz; }

    // ---- every frame (cheap)
    // tunedHz: the frequency the user is on (the radio's, or the General receiver's listen cursor). Releases a decoder the user tuned away from
    // and, with autoDecode, starts one when the tuned frequency sits on a sure label.
    Step tick(double nowSec, double tunedHz);

    // ---- the user
    Step decode(double nowSec, int labelId);       // the Decode button (Action::Decode, or None with a note when it cannot)
    Step stop(double nowSec);                      // "Back to General" while decoding
    void abandon();                                // the user chose another mode by hand: the session is over, no action
    void ignore(int labelId);                      // ignore this label (and this place and name) for the session
    void clearIgnored();
    void reset();                                  // everything forgotten (the General session was switched off)

    // ---- what to draw
    State state() const { return state_; }
    const std::vector<Label>& labels() const { return labels_; }
    const Label* find(int id) const;
    const Label* active() const { return find(activeId_); }   // the label being decoded (nullptr in General)
    // labels that are shown and in view (what the spectrum labels), strongest first
    std::vector<const Label*> visible() const;
    size_t ignoredCount() const { return ignored_.size(); }
    double activeRfHz() const { return lockRfHz_; }
    double activeReleaseHz() const { return lockReleaseHz_; }

private:
    struct Ign { double rfHz, tolHz; std::string name; };
    bool isIgnored(const Detection& d) const;
    void expire(double nowSec);
    State state_ = State::General;
    std::vector<Label> labels_;
    std::vector<Ign> ignored_;
    int nextId_ = 1, activeId_ = 0;
    double lockRfHz_ = 0, lockBwHz_ = 0, lockReleaseHz_ = 0;
    std::string lockName_;
    double settleUntil_ = 0, nextScan_ = 0;
    bool scanning_ = false;
    bool haveSuppress_ = false; double suppressRf_ = 0, suppressTol_ = 0;   // after "back to General" on a label: no auto-decode until the user leaves it
};

} // namespace genauto
} // namespace dect2
