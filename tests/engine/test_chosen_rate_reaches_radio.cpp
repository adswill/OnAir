// The sample rate a user chooses (Advanced > Custom, or the RATE box) is the rate the radio is asked for: a fake HackRF records what its
// driver is given (3.5, bug "set 10 Msps, the radio runs 5"). Many modes, several rates, with and without the offset tuning of the DC spike;
// the rate the radio then runs at is the one the UI shows ("runs at ..."), which is the engine's radioRate().
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/source/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

namespace {
struct Seen { std::mutex mu; std::vector<double> starts, retunes; };
Seen gSeen;

// a radio that takes any rate exactly (a HackRF does) unless `cap` is set: then it runs at the cap, like a driver that rounds
class FakeRadio : public IqSource {
public:
    explicit FakeRadio(double cap) : cap_(cap) {}
    bool start(const TuneSettings& s, IqRing&, std::string&) override {
        { std::lock_guard<std::mutex> lk(gSeen.mu); gSeen.starts.push_back(s.sampleRate); }
        rate_ = cap_ > 0 ? std::min(cap_, s.sampleRate) : s.sampleRate;
        return true;
    }
    void stop() override {}
    bool retune(const TuneSettings& s, std::string&) override { std::lock_guard<std::mutex> lk(gSeen.mu); gSeen.retunes.push_back(s.sampleRate); return true; }
    double sampleRate() const override { return rate_; }
    bool realtimeHardware() const override { return true; }
private:
    double cap_, rate_ = 0;
};

// what the app's applyBandwidth() leaves in TuneSettings for a rate the user chose
TuneSettings planFor(const ModeTuning& mt, double chosen) {
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz; t.sampleRate = mt.sampleRate; t.basebandFilterHz = mt.basebandHz;
    if (chosen > 0) { t.sampleRate = chosen; t.basebandFilterHz = basebandFilterFor(mt.basebandHz, mt.sampleRate, chosen); }
    t.rateChosen = chosen > 0;
    return t;
}
}

int main() {
    bool capped = false;
    double cap = 0;
    setTestSourceFactory([&](const DeviceInfo&) { return std::unique_ptr<IqSource>(new FakeRadio(capped ? cap : 0)); });
    DeviceInfo hackrf; hackrf.kind = DeviceInfo::HackRF; hackrf.board = "HackRF Pro"; hackrf.name = "HackRF Pro (fake)";
    // P25, DMR, ADS-B, FM, TETRA, pagers, ISM, airband, NXDN, HFDL, DVB-T (0), ATSC-like (6 MHz at 8 Msps)
    const int kStd[] = {52, 10, 13, 6, 48, 25, 29, 27, 39, 36, 0, 14};
    const double kRates[] = {2e6, 4e6, 5e6, 8e6, 10e6, 16e6, 20e6};
    for (int autoOffset = 0; autoOffset < 2; autoOffset++)
    for (int std : kStd) {
        const ModeTuning* mt = std == 0 ? nullptr : modeTuning(std);
        ModeTuning dvb; if (!mt) { dvb.stdMode = 0; dvb.id = "dvb"; dvb.defMhz = 522; dvb.bandwidthMhz = 8; dvb.sampleRate = 10e6; mt = &dvb; }
        for (double hz : kRates) {
            { std::lock_guard<std::mutex> lk(gSeen.mu); gSeen.starts.clear(); gSeen.retunes.clear(); }
            Engine e;
            e.setAutoOffset(autoOffset);
            e.setStandard(std);
            FileOptions fo;
            const TuneSettings t = planFor(*mt, hz);
            const bool ok = e.start(hackrf, t, fo);
            CHECK(ok, "%s @ %.1f Msps: did not start", mt->id, hz / 1e6);
            if (!ok) continue;
            double asked = 0;
            { std::lock_guard<std::mutex> lk(gSeen.mu); asked = gSeen.starts.empty() ? 0 : gSeen.starts.back(); }
            if (autoOffset && asked > hz * 1.0001) {
                // offset tuning may run the radio faster than the choice (never slower): the receiver still gets the chosen rate
                CHECK(std::fabs(e.sampleRate() - hz) < 1.0, "%s @ %.1f Msps (offset): receiver rate %.3f", mt->id, hz / 1e6, e.sampleRate() / 1e6);
            } else {
                CHECK(std::fabs(asked - hz) < 1.0, "%s @ %.1f Msps (auto offset %d): the radio was asked for %.3f Msps", mt->id, hz / 1e6, autoOffset, asked / 1e6);
            }
            CHECK(asked + 1 >= hz, "%s @ %.1f Msps: the radio was asked for LESS (%.3f)", mt->id, hz / 1e6, asked / 1e6);
            CHECK(e.radioRate() + 1 >= hz, "%s @ %.1f Msps: radioRate %.3f", mt->id, hz / 1e6, e.radioRate() / 1e6);
            // a live retune keeps the rate too
            TuneSettings t2 = t; t2.centerHz += 25e3;
            e.retune(t2);
            { std::lock_guard<std::mutex> lk(gSeen.mu); for (double r : gSeen.retunes) CHECK(r + 1 >= hz, "%s @ %.1f Msps: a retune asked for %.3f Msps", mt->id, hz / 1e6, r / 1e6); }
            e.stop();
        }
    }
    // a driver that cannot run the rate: the engine reports what the radio really runs at (the UI's "runs at"), not the asked-for rate
    capped = true; cap = 5e6;
    {
        Engine e; e.setStandard(52);
        FileOptions fo;
        const ModeTuning* mt = modeTuning(52);
        const bool ok = e.start(hackrf, planFor(*mt, 10e6), fo);
        CHECK(ok && std::fabs(e.radioRate() - 5e6) < 1.0, "a capped driver: radioRate %.3f Msps (should show the real 5)", e.radioRate() / 1e6);
        e.stop();
    }
    return testSummary("chosen rate reaches the radio");
}
