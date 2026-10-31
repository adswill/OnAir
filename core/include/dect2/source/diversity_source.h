// Diversity pair: two radios tuned to the SAME frequency (two dongles, two antennas) whose streams are combined into one better stream for weak
// narrowband signals. To the engine the pair is one more radio in the list (DeviceInfo::Group, "Diversity: <name>"), so every narrowband mode
// (FM, AM, voice, paging, telemetry ...) simply hears a cleaner signal. Off by default: nothing changes until the user makes a pair.
//
//   members -> their own IqRing -> worker thread: DC removal, start-up alignment (dsp/diversity.h: diversityCoarseAlign, the radios deliver
//   their first samples tens of ms apart), then DiversityCombiner (delay / frequency offset / phase tracking, selection or maximal-ratio) -> the
//   engine's ring.
// A member that fails to start or stops delivering is left out: the pair carries on with the other radio unchanged (the status says so).
// The streams are combined for signals that both radios hear and that fill well under half of the sampled band (see dsp/diversity.h).
#pragma once
#include "dect2/dsp/diversity.h"
#include "dect2/source/synth_group.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dect2 {

struct DiversityMemberStatus {
    std::string name;
    bool running = false;
    double deliveredPct = 0;      // samples received against the nominal rate over the last seconds (USB losses show here)
    std::string error;
};
struct DiversitySourceStatus {
    bool running = false;
    std::string name;
    bool combining = false;       // both radios deliver and the combiner is locked on a common signal
    bool aligned = false;         // the start-up alignment found the delay between the radios
    std::string state;            // plain words: what it is doing now
    int mode = 1;                 // 1 = maximal ratio, 0 = selection
    double rateHz = 0;
    DiversityStatus dsp;
    std::vector<DiversityMemberStatus> members;
    std::string usbWarning;
};
bool diversityStatus(const std::string& name, DiversitySourceStatus& out);       // the running pair of that name
bool diversitySetMode(const std::string& name, int mode);                         // 1 = maximal ratio, 0 = selection, takes effect at once
bool diversityRealign(const std::string& name);                                   // measure the delay / offset again

class DiversitySource : public IqSource {
public:
    // members: exactly 2; factory: how a member is opened (makeSource when empty; tests pass their own)
    DiversitySource(SynthGroupDef def, std::vector<DeviceInfo> members, ChildFactory factory = nullptr);
    ~DiversitySource() override;
    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override;
    void stop() override;
    bool retune(const TuneSettings& s, std::string& err) override;
    double sampleRate() const override { return rate_; }
    bool realtimeHardware() const override { return true; }
    std::string statusNote() const override;

    DiversitySourceStatus status() const;
    void setMode(int mode);
    void realign();

private:
    struct Child {
        DeviceInfo dev;
        std::unique_ptr<IqSource> src;
        std::unique_ptr<IqRing> ring;
        bool running = false;
        std::string error;
        cf32 dc{0, 0};
        std::vector<cf32> q;             // read from the ring, not yet used
        int64_t lastNs = 0;
        uint64_t got = 0; int64_t rateT0 = 0; uint64_t rateN0 = 0; double deliveredPct = 100;
    };
    bool startChild(int i, std::string& err);
    void stopChild(int i);
    void worker();
    TuneSettings childTune() const;

    SynthGroupDef def_;
    ChildFactory factory_;
    std::vector<Child> ch_;
    TuneSettings tune_;
    IqRing* out_ = nullptr;
    double rate_ = 0;
    mutable std::mutex mu_;
    DiversityCombiner comb_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::atomic<int> mode_{1};
    std::atomic<bool> realign_{true};
    bool aligned_ = false, combining_ = false;
    std::string state_, usbWarn_;
};

std::unique_ptr<IqSource> makeDiversitySource(const SynthGroupDef& def, const std::vector<DeviceInfo>& members);

} // namespace dect2
