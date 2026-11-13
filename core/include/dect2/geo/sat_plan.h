// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The planning logic of the satellite tracker (View > Passes, docs/SATTRACK-3.8.md), without any UI so it can be tested: which pass a user can
// hear (filters on the kind of satellite, peak elevation, the radio and the antenna), how old an orbit is, how an armed pass is found again after
// new elements arrive, and the run of one pass from the warm-up before AOS to the restore after LOS.
#pragma once
#include "dect2/geo/passes.h"
#include "dect2/geo/pass_sched.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

// ---- the kind of satellite (a satellite can be several: an amateur CubeSat is both)
enum SatTag : unsigned { TagWeather = 1, TagIss = 2, TagOrbcomm = 4, TagAmateur = 8, TagGlobalstar = 16, TagCubesat = 32, TagIridium = 64, TagAll = 127 };

// ---- bands an antenna covers
enum AntennaBand : unsigned { BandVhf = 1, BandUhf = 2, BandL = 4, BandOther = 8, BandAny = 15 };
unsigned bandOf(double hz);

struct RadioCaps { double minHz = 0, maxHz = 0, maxRateHz = 0; };   // 0 = not known (not held against a link)

// One downlink of a satellite and what OnAir does with it
struct SatLink {
    double hz = 0;
    int family = 0;              // the app's mode
    std::string mode;            // shown: "APT", "LRPT", "AX.25 9k6 ..."
    bool decoder = true;         // OnAir decodes it (else the pass gives the IQ recording only)
    double needRateHz = 0;       // sample rate the mode needs (0 = unknown)
    double retuneTolHz = 150;    // Doppler: the radio is retuned when the wanted frequency has moved this far
    bool radioFollowsDoppler = true;   // false: the decoder follows the shift itself, a retune would break a frame (LoRa)
    bool eventOnly = false;      // on the air only during announced events (ISS SSTV): chosen when asked for, never by default over a link that always runs
};

// A link is hearable with this radio and antenna: the frequency in both, and a sample rate the radio reaches
bool linkHearable(const SatLink& l, const RadioCaps& radio, unsigned antennaMask);
// The link used by default: hearable ones first, one with a decoder before one without, then the lowest frequency (the easiest antenna).
// -1 when the satellite has no links. hearable (optional) tells whether the chosen one is hearable.
int bestLink(const std::vector<SatLink>& links, const RadioCaps& radio, unsigned antennaMask, bool* hearable = nullptr);

struct PassFilter {
    unsigned tagMask = TagAll;
    double minPeakDeg = 0;
    bool onlyHearable = false;
    bool showRetired = false;
    RadioCaps radio;
    unsigned antennaMask = BandAny;
};
// Whether a pass of a satellite (its tags, links and whether it is retired) belongs in the list
bool passVisible(unsigned tags, bool retired, const std::vector<SatLink>& links, const Pass& p, const PassFilter& f);

// Passes from horizon to horizon (horizonDeg: 0, or a mask for hills), kept when they peak at or above minPeakDeg. A minimum elevation
// is a filter on the peak (as in SatDump, Gpredict): cutting the pass at the threshold loses the start and end of what could be received.
std::vector<Pass> findPassesPeak(const Sgp4& s, const Observer& o, double t0, double t1, double horizonDeg, double minPeakDeg);

// ---- how old an orbit is
enum class TleFresh { Fresh, Ageing, Stale, Expired };
double tleAgeDays(const Tle& t, double nowSec);                 // negative when the epoch is in the future
TleFresh tleFreshness(double ageDays);                          // Fresh < 3 days, Ageing < 14, Stale < 30, Expired from 30 (or an epoch far in the future)
double tleTimingErrorSec(double ageDays);                       // a rough figure for the AOS error of a low orbit, shown so an old orbit is not trusted
// Merges elements into the map: the newest epoch of each satellite wins (a stale download never replaces a newer built-in set, and vice versa)
void mergeNewestTle(std::map<int, Tle>& into, const std::vector<Tle>& add);

// ---- an armed pass, found again after the elements change
// The index of the candidate with the closest culmination within tolSec of armed's, -1 when none: new elements move a pass by seconds, not by an orbit
int matchPass(const Pass& armed, const std::vector<Pass>& candidates, double tolSec = 300);
inline bool samePass(const Pass& a, const Pass& b, double tolSec = 300) { const double d = a.tca - b.tca; return d < tolSec && d > -tolSec; }

// Priority of a pass in the plan: a pass the user armed always beats one scheduled by itself; within each, the group priority
constexpr int kArmedPriorityBoost = 100;
inline int queuePriority(int groupPriority, bool armed) { return groupPriority + (armed ? kArmedPriorityBoost : 0); }

// ---- the run of one pass
struct RunJob {
    int norad = 0;
    std::string name, mode;
    double hz = 0;
    int family = 0;
    Pass pass;                   // the whole pass
    double start = 0, end = 0;   // the part taken (a trimmed slot starts late or ends early)
    bool startReceiver = true;   // start it (when stopped), set the mode
    bool record = true;
    bool takeOver = false;       // a receiver running another mode is switched for the pass and restored afterwards (the user armed this pass)
    bool radioFollowsDoppler = true;
    double retuneTolHz = 150;
};

// What the runner needs from the program
class PassHost {
public:
    virtual ~PassHost() = default;
    virtual bool running() const = 0;
    virtual bool transmitting() const = 0;       // a transmission under way is never interrupted
    virtual int family() const = 0;
    virtual double tunedHz() const = 0;
    virtual void setMode(int family) = 0;
    virtual void tune(double hz) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool canRecord() const = 0;
    virtual bool recording() const = 0;
    virtual bool startRecording(std::string& path, std::string& err) = 0;
    virtual void stopRecording() = 0;
    virtual double dopplerHz(const RunJob& j, double t) const = 0;   // added to the nominal frequency
    virtual void log(const std::string& s) = 0;
    virtual void onAos(const RunJob& j) = 0;     // gallery context, automations, naming the frames
    virtual void onLos(const RunJob& j, bool reached) = 0;
};

enum class PassOutcome { Completed, Cancelled, SkippedBusy, SkippedTx, Failed };
struct PassResult { RunJob job; PassOutcome outcome = PassOutcome::Completed; std::string iqPath, detail; double startedAt = 0, endedAt = 0; };

class PassRunner {
public:
    enum class Phase { Idle, Warmup, Receiving };
    // Once a frame. job: the slot that is due now (its warm-up included) or null. The result of a pass that ended, when one did, goes to finished.
    void tick(double now, const RunJob* job, PassHost& host, std::vector<PassResult>* finished = nullptr);
    void cancel(double now, PassHost& host, std::vector<PassResult>* finished);
    Phase phase() const { return phase_; }
    bool active() const { return phase_ != Phase::Idle; }
    bool following() const { return following_; }            // the receiver is on the pass (false: the user took it over, or the pass was tracked only)
    bool recordingIq() const { return weRecord_; }
    const RunJob& job() const { return cur_; }
    double lastTunedHz() const { return lastHz_; }
    int retunes() const { return retunes_; }
private:
    void begin(double now, const RunJob& j, PassHost& h, std::vector<PassResult>* finished);
    void finish(double now, PassOutcome why, PassHost& h, std::vector<PassResult>* finished, const std::string& detail = "");
    void doppler(double now, PassHost& h, bool force);
    Phase phase_ = Phase::Idle;
    RunJob cur_;
    bool following_ = false, owned_ = false, restoreMode_ = false, weRecord_ = false, recTried_ = false, aosDone_ = false;
    bool prevRunning_ = false; int prevFamily_ = 0; double prevHz_ = 0;
    double lastHz_ = 0, lastTune_ = -1e18, startedAt_ = 0; int retunes_ = 0;
    std::string iqPath_;
    bool haveDone_ = false; int doneNorad_ = 0; Pass donePass_;   // a pass already handled is not started again while it is still listed
};

}  // namespace dect2
