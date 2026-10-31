// Synthetic wideband radio groups (docs/SYNTH-WIDEBAND.md): 2-5 ordinary radios (RTL-SDRs or any mix running at one sample rate) tuned to
// adjacent, overlapping slices and stitched (dect2/dsp/synth_wide.h) into ONE wide stream. To the engine a group is one more radio in the list
// (DeviceInfo::Group): SyntheticWideSource opens the members, feeds the stitcher on its own thread and writes the wide stream into the engine's
// ring, so the spectrum, waterfall, wide bank, channelizer, scanner and every other mode just see a wide radio.
//
//   group definition (saved by the app per member serials) -> planSlices(): slice centres, overlap, achievable span, output rate
//   members -> their own IqRing (bounded queue, ~0.4 s) -> worker thread: DC removal, SynthWideband::push / pull -> the engine's ring
// A member that fails to start, stops delivering or is switched off is left out (the stitcher fades its neighbours over the gap); the status
// says which. The phase across the seams of free-running radios wanders: see synth_wide.h and the study numbers in the doc.
#pragma once
#include "dect2/dsp/synth_wide.h"
#include "dect2/source/source.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {

struct SynthGroupMember {
    std::string board, serial, name;   // identity: board + serial when the radio has a serial, else its list name
};

struct SynthGroupDef {
    std::string name;                        // what the radio list shows ("Group: <name>")
    std::vector<SynthGroupMember> members;   // in frequency order: member 0 takes the lowest slice
    double centerHz = 100e6;                 // the frequency the dialog proposes (the receiver's own frequency is what is tuned)
    double spanHz = 8e6;                     // total span wanted
    double childRateHz = 0;                  // the members' common sample rate; 0 = automatic (2.4 Msps for RTL-SDRs, else what they all reach)
    bool diversity = false;                  // a diversity pair (diversity_source.h): exactly 2 radios tuned to the SAME frequency and combined for weak signals,
                                             // not stitched side by side; span and centre are not used
    int diversityMode = 1;                   // 1 = maximal-ratio combining, 0 = selection (dsp/diversity.h)
};

// How the span is cut: centre of each slice relative to the group centre, overlap, the span reached and the stitched output rate.
struct SlicePlan {
    bool ok = false;
    std::string why;                         // when !ok, or a note (span limited by too few radios / too slow a rate)
    synthwide::SynthCfg cfg;
    std::vector<double> sliceOffsetHz;       // per member
    double overlapHz = 0, spanHz = 0, maxSpanHz = 0, outRateHz = 0;
};
// radios: 2..5; childRateHz: their sample rate; wantSpanHz: total span (clamped to what they reach with a safe overlap)
SlicePlan planSlices(int radios, double childRateHz, double wantSpanHz, double usable = 0.8);

// the app's saved groups (one line per group; the fields are percent-escaped)
std::string synthGroupsToString(const std::vector<SynthGroupDef>& v);
std::vector<SynthGroupDef> synthGroupsFromString(const std::string& s);
void setSynthGroups(const std::vector<SynthGroupDef>& v);
std::vector<SynthGroupDef> synthGroups();

bool memberMatches(const SynthGroupMember& m, const DeviceInfo& d);
SynthGroupMember memberOf(const DeviceInfo& d);
// the sample rate a group of these radios runs each member at (0 when they have no common rate)
double groupChildRate(const std::vector<DeviceInfo>& members, double wanted = 0);
// one list entry per saved group with at least 2 of its members among `found` (the members found are remembered for makeSource)
std::vector<DeviceInfo> synthGroupDevices(const std::vector<DeviceInfo>& found);
// the members a group entry stands for (as found at listing time)
std::vector<DeviceInfo> synthGroupMembers(const DeviceInfo& group);

// USB load: bytes per second a radio sends at `rate`, and a warning when the members together need more than one USB 2 hub carries, or
// several dongles hang on bus power ("" = nothing to say)
double usbBytesPerSec(const DeviceInfo& d, double rate);
std::string usbLoadWarning(const std::vector<DeviceInfo>& members, double rate);

// ---- the running group: status and commands (the app reaches the source the engine owns through these, by group name) ----
struct SynthMemberStatus {
    std::string name;
    double centerHz = 0;          // where the member is tuned
    bool enabled = true;          // the user did not switch it off
    bool running = false;         // its driver is open and delivering
    bool alive = false;           // part of the stitch now
    bool locked = false;
    double offsetHz = 0, driftPpm = 0;
    double deliveredPct = 0;      // samples received against its nominal rate over the last seconds (USB losses show here)
    long long dropouts = 0;       // times it fell out of the stitch
    uint64_t queueDropped = 0;    // samples thrown away because the stitcher could not keep up
    std::string error;            // why it is not running
};
struct SynthGroupStatus {
    bool running = false;
    std::string name;
    double outRateHz = 0, spanHz = 0, overlapHz = 0, childRateHz = 0;
    std::vector<SynthMemberStatus> members;
    std::vector<synthwide::PairStatus> seams;   // between member i and i+1
    std::string usbWarning;
};
bool synthGroupStatus(const std::string& name, SynthGroupStatus& out);
bool synthGroupRecalibrate(const std::string& name);
bool synthGroupSetMember(const std::string& name, int member, bool enabled);   // hot remove / add back

using ChildFactory = std::function<std::unique_ptr<IqSource>(const DeviceInfo&)>;

class SyntheticWideSource : public IqSource {
public:
    // members in frequency order; factory: how a member is opened (makeSource when empty; tests pass their own)
    SyntheticWideSource(SynthGroupDef def, std::vector<DeviceInfo> members, ChildFactory factory = nullptr);
    ~SyntheticWideSource() override;
    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override;
    void stop() override;
    bool retune(const TuneSettings& s, std::string& err) override;
    double sampleRate() const override { return plan_.outRateHz; }
    bool realtimeHardware() const override { return true; }

    SynthGroupStatus status() const;
    void recalibrate();
    bool setMemberEnabled(int i, bool enabled, std::string* err = nullptr);
    const SlicePlan& plan() const { return plan_; }

private:
    struct Child {
        DeviceInfo dev;
        std::unique_ptr<IqSource> src;
        std::unique_ptr<IqRing> ring;
        bool enabled = true, running = false;
        std::string error;
        cf32 dc{0, 0};
        int64_t lastNs = 0;               // when it last delivered
        uint64_t got = 0; int64_t rateT0 = 0; uint64_t rateN0 = 0; double deliveredPct = 0;
        uint64_t dropped = 0;
    };
    bool startChild(int i, std::string& err);
    void stopChild(int i);
    void worker();
    TuneSettings childTune(int i) const;

    SynthGroupDef def_;
    ChildFactory factory_;
    SlicePlan plan_;
    std::vector<Child> ch_;
    TuneSettings tune_;
    IqRing* out_ = nullptr;
    mutable std::mutex mu_;               // the stitcher, the children's state, tune_
    synthwide::SynthWideband sw_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::string usbWarn_;
};

std::unique_ptr<IqSource> makeSynthGroupSource(const DeviceInfo& d);

} // namespace dect2
