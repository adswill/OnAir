// Radio groups (core/include/dect2/source/synth_group.h): the slice plan, the saved-group format, the list entries, the USB warning, and
// SyntheticWideSource driving fake member radios: start with one member failing, rate mismatch, a member that stops delivering, hot remove /
// add back, retune, and too few members. No hardware; the alignment itself is tested in tests/dsp/test_synth_wide.cpp.
#include "dect2/source/synth_group.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

namespace {
struct Ctl { std::atomic<bool> failStart{false}, mute{false}; double rateScale = 1.0; std::atomic<double> center{0}; std::atomic<int> retunes{0}; };

class Fake : public IqSource {
public:
    explicit Fake(std::shared_ptr<Ctl> c) : c_(std::move(c)) {}
    ~Fake() override { stop(); }
    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override {
        if (c_->failStart) { err = "fake failure"; return false; }
        rate_ = s.sampleRate * c_->rateScale; c_->center = s.centerHz; ring_ = &ring; run_ = true;
        th_ = std::thread([this] {
            std::mt19937 rng(7); std::normal_distribution<float> nd(0.f, 0.1f);
            std::vector<cf32> b(4096);
            while (run_) {
                if (!c_->mute) { for (auto& v : b) v = cf32(nd(rng), nd(rng)); ring_->write(b.data(), b.size()); }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        return true;
    }
    void stop() override { run_ = false; if (th_.joinable()) th_.join(); }
    bool retune(const TuneSettings& s, std::string&) override { c_->center = s.centerHz; c_->retunes++; return true; }
    double sampleRate() const override { return rate_; }
private:
    std::shared_ptr<Ctl> c_;
    double rate_ = 0;
    IqRing* ring_ = nullptr;
    std::atomic<bool> run_{false};
    std::thread th_;
};

DeviceInfo rtl(const std::string& serial) {
    DeviceInfo d; d.kind = DeviceInfo::Native; d.board = "rtlsdr"; d.serial = serial; d.name = "RTL-SDR " + serial; d.maxRateHz = 3.2e6; d.minRateHz = 0.25e6;
    return d;
}
void waitMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
}

static void testPlan() {
    const SlicePlan p = planSlices(5, 2.4e6, 8e6);
    CHECK(p.ok, "plan 5 x 2.4 Msps / 8 MHz: %s", p.why.c_str());
    CHECK(p.cfg.n == 1024, "FFT size %d", p.cfg.n);
    CHECK(p.overlapHz >= 64 * 2.4e6 / 1024 - 1, "overlap %.0f Hz", p.overlapHz);
    CHECK(std::fabs(p.outRateHz - 9.6e6) < 1, "out rate %.0f", p.outRateHz);
    CHECK(p.spanHz > 7.9e6 && p.spanHz < 8.1e6, "span %.0f", p.spanHz);
    CHECK(p.sliceOffsetHz.size() == 5 && std::fabs(p.sliceOffsetHz[2]) < 1 && p.sliceOffsetHz[0] < -2.5e6 && p.sliceOffsetHz[4] > 2.5e6, "slice centres");
    printf("plan: n %d step %d overlap %.0f kHz span %.3f MHz (max %.3f) rate %.3f Msps\n", p.cfg.n, p.cfg.stepBins, p.overlapHz / 1e3, p.spanHz / 1e6, p.maxSpanHz / 1e6, p.outRateHz / 1e6);
    const SlicePlan w = planSlices(5, 2.4e6, 20e6);
    CHECK(w.ok && !w.why.empty() && std::fabs(w.spanHz - w.maxSpanHz) < 1, "too wide a span is clamped with a note (%.0f vs %.0f)", w.spanHz, w.maxSpanHz);
    CHECK(!planSlices(1, 2.4e6, 8e6).ok && !planSlices(6, 2.4e6, 8e6).ok && !planSlices(3, 0, 8e6).ok, "bad plans accepted");
}

static void testStrings() {
    SynthGroupDef g;
    g.name = "5x RTL; roof|mast"; g.centerHz = 98.5e6; g.spanHz = 8e6; g.childRateHz = 2.4e6;
    g.members = {{"rtlsdr", "0001", "RTL-SDR 1"}, {"rtlsdr", "", "Generic, 100%"}};
    const auto v = synthGroupsFromString(synthGroupsToString({g}));
    CHECK(v.size() == 1, "round trip size %zu", v.size());
    if (v.size() == 1) {
        CHECK(v[0].name == g.name && v[0].centerHz == g.centerHz && v[0].spanHz == g.spanHz && v[0].childRateHz == g.childRateHz, "fields");
        CHECK(v[0].members.size() == 2 && v[0].members[1].name == "Generic, 100%" && v[0].members[0].serial == "0001", "members");
    }
    CHECK(synthGroupsFromString("junk\n;;;\n").empty(), "junk parsed");
}

static void testDevices() {
    SynthGroupDef g; g.name = "trio"; g.spanHz = 5e6;
    g.members = {memberOf(rtl("A")), memberOf(rtl("B")), memberOf(rtl("C"))};
    setSynthGroups({g});
    auto found = std::vector<DeviceInfo>{rtl("A"), rtl("C"), rtl("Z")};
    auto d = synthGroupDevices(found);
    CHECK(d.size() == 1 && d[0].kind == DeviceInfo::Group && d[0].isRadio(), "one group entry");
    if (!d.empty()) {
        CHECK(d[0].name.find("2 of 3") != std::string::npos, "name %s", d[0].name.c_str());
        CHECK(synthGroupMembers(d[0]).size() == 2, "members found");
        CHECK(d[0].maxRateHz > 4e6 && d[0].maxRateHz == d[0].minRateHz, "rate %.0f", d[0].maxRateHz);
    }
    CHECK(synthGroupDevices({rtl("A")}).empty(), "a group with one radio found is listed");
    setSynthGroups({});
    std::vector<DeviceInfo> five{rtl("1"), rtl("2"), rtl("3"), rtl("4"), rtl("5")};
    CHECK(!usbLoadWarning(five, 2.4e6).empty(), "no warning for 5 dongles");
    CHECK(usbLoadWarning({rtl("1"), rtl("2")}, 2.4e6).empty(), "warning for 2 dongles");
}

static void testSource() {
    std::vector<std::shared_ptr<Ctl>> ctl;
    std::vector<DeviceInfo> mem;
    for (int i = 0; i < 4; i++) { ctl.push_back(std::make_shared<Ctl>()); mem.push_back(rtl(std::to_string(i))); }
    std::map<std::string, std::shared_ptr<Ctl>> bySerial;
    for (int i = 0; i < 4; i++) bySerial[mem[(size_t)i].serial] = ctl[(size_t)i];
    auto factory = [&](const DeviceInfo& d) -> std::unique_ptr<IqSource> { return std::make_unique<Fake>(bySerial[d.serial]); };
    SynthGroupDef def; def.name = "test-group"; def.spanHz = 6e6;
    for (auto& d : mem) def.members.push_back(memberOf(d));
    ctl[2]->failStart = true;
    {
        SyntheticWideSource src(def, mem, factory);
        CHECK(src.plan().ok, "plan: %s", src.plan().why.c_str());
        IqRing ring(1u << 22);
        TuneSettings t; t.centerHz = 100e6;
        std::string err;
        CHECK(src.start(t, ring, err), "start with one member failing: %s", err.c_str());
        CHECK(err.find("fake failure") != std::string::npos, "the failure is said: %s", err.c_str());
        CHECK(std::fabs(ctl[0]->center - (100e6 + src.plan().sliceOffsetHz[0])) < 1, "member 0 tuned to its slice");
        waitMs(400);
        SynthGroupStatus st;
        CHECK(synthGroupStatus("test-group", st), "status by name");
        CHECK(st.members.size() == 4 && !st.members[2].alive && !st.members[2].error.empty() && st.members[0].running, "status of the failed member");
        const size_t got1 = ring.available();
        CHECK(got1 > 100000, "wide samples flowing: %zu", got1);
        // hot remove and add back
        CHECK(synthGroupSetMember("test-group", 1, false), "switch member 1 off");
        waitMs(200);
        synthGroupStatus("test-group", st);
        CHECK(!st.members[1].enabled && !st.members[1].alive, "member 1 off");
        ring.clear();
        waitMs(200);
        CHECK(ring.available() > 50000, "output goes on with member 1 off: %zu", ring.available());
        CHECK(synthGroupSetMember("test-group", 1, true), "member 1 back");
        waitMs(300);
        synthGroupStatus("test-group", st);
        CHECK(st.members[1].enabled && st.members[1].running, "member 1 running again");
        // a member stops delivering
        ctl[3]->mute = true;
        waitMs(1300);
        synthGroupStatus("test-group", st);
        CHECK(!st.members[3].running && st.members[3].error.find("no samples") != std::string::npos, "silent member: '%s'", st.members[3].error.c_str());
        ring.clear();
        waitMs(200);
        CHECK(ring.available() > 50000, "output goes on with a silent member: %zu", ring.available());
        ctl[3]->mute = false;
        // retune
        TuneSettings t2 = t; t2.centerHz = 200e6;
        CHECK(src.retune(t2, err), "retune");
        CHECK(std::fabs(ctl[0]->center - (200e6 + src.plan().sliceOffsetHz[0])) < 1 && ctl[0]->retunes >= 1, "member 0 retuned");
        CHECK(synthGroupRecalibrate("test-group"), "recalibrate by name");
        src.stop();
        CHECK(!synthGroupStatus("test-group", st), "status after stop");
    }
    // too few members start; a member at the wrong rate is left out
    ctl[2]->failStart = false;
    ctl[1]->failStart = true; ctl[3]->failStart = true; ctl[2]->rateScale = 1.01;
    {
        SyntheticWideSource src(def, mem, factory);
        IqRing ring(1u << 20);
        TuneSettings t; t.centerHz = 100e6;
        std::string err;
        CHECK(!src.start(t, ring, err), "started with one usable member");
        CHECK(err.find("instead of") != std::string::npos, "rate mismatch said: %s", err.c_str());
    }
}

int main() {
    testPlan();
    testStrings();
    testDevices();
    testSource();
    return testSummary("synth_group");
}
