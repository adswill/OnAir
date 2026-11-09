// Trunk pool: the same group call granted by two sites is one call (core/src/trunkpool/pool.cpp, PoolConfig::dedupSites). Synthetic harness as in
// test_trunkpool_pool: fake voice radios, a simulated clock. Covers the merge, the better-copy takeover, the late copy, another caller, and the
// capacity gain with N radios against the old behaviour.
#include "dect2/trunkpool/trunkpool.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
using namespace dect2::tpool;
#include "../common/test_util.h"

// A fake radio: tuning takes latencySec, then it locks (or fails). A voice channel is "active" when the harness says so.
struct FakeRx : VoiceReceiver {
    std::string nm;
    bool here = true, ctl = true, failTune = false, refuse = false;
    double centre = 0, span = 0, latency = 0.25, now = 0;
    int slices = 0;
    RxState st = RxState::Idle;
    double tuneAt = 0, freq = 0;
    bool voice = true, term = false, enc = false;
    int tunes = 0, releases = 0;
    std::vector<int> sliceUsed;
    double audioPerPull = 0.1;
    explicit FakeRx(const std::string& n, double c = 0, double s = 0, int sl = 0) : nm(n), centre(c), span(s), slices(sl), sliceUsed((size_t)sl, 0) {}
    std::string name() const override { return nm; }
    bool present() const override { return here; }
    bool canControl() const override { return ctl; }
    double captureCentreHz() const override { return centre; }
    double captureSpanHz() const override { return span; }
    int maxSlices() const override { return slices; }
    bool tune(const TuneRequest& r) override { if (refuse) return false; tunes++; freq = r.freqHz; centre = r.freqHz; st = RxState::Tuning; tuneAt = now; return true; }
    RxState state() const override { return st == RxState::Tuning && failTune ? (now - tuneAt > latency ? RxState::Failed : RxState::Tuning) : (st == RxState::Tuning && now - tuneAt >= latency ? RxState::Locked : st); }
    bool hearingVoice() const override { return state() == RxState::Locked && voice; }
    bool terminated() override { const bool t = term; term = false; return t; }
    bool encryptedHeard() const override { return enc && state() == RxState::Locked; }
    void release() override { releases++; st = RxState::Idle; }
    bool openSlice(const TuneRequest&, int* s) override { for (size_t i = 0; i < sliceUsed.size(); i++) if (!sliceUsed[i]) { sliceUsed[i] = 1; *s = (int)i; return true; } return false; }
    void closeSlice(int s) override { if (s >= 0 && (size_t)s < sliceUsed.size()) sliceUsed[(size_t)s] = 0; }
    bool sliceHearing(int s) const override { return s >= 0 && (size_t)s < sliceUsed.size() && sliceUsed[(size_t)s] && voice; }
    int pullAudio(int, float* out, int maxFrames, int* rate) override {
        if (!(st == RxState::Locked || (st == RxState::Tuning && state() == RxState::Locked)) && sliceUsed.empty()) return 0;
        if (!voice) return 0;
        static int phase = 0;
        const int n = std::min(maxFrames, (int)(8000 * audioPerPull));
        for (int i = 0; i < n; i++) out[i] = 0.3f * std::sin(0.2f * (float)(phase++));
        *rate = 8000;
        audioPerPull = audioPerPull > 0 ? audioPerPull : 0;
        return n;
    }
};

struct Sim {
    TrunkPool pool;
    std::vector<FakeRx*> rx;
    double t = 0;
    explicit Sim(const PoolConfig& c = PoolConfig()) : pool(c) {}
    FakeRx* add(const std::string& n, double centre = 0, double span = 0, int slices = 0, bool ctl = true) {
        auto* r = new FakeRx(n, centre, span, slices);
        r->ctl = ctl;
        rx.push_back(r);
        pool.addDevice(std::unique_ptr<VoiceReceiver>(r));
        return r;
    }
    void run(double sec, double dt = 0.05) {
        const double end = t + sec;
        while (t < end) { t += dt; for (auto* r : rx) r->now = t; pool.tick(t); }
    }
    void grant(int tg, double hz, int src = 100, int slot = -1, bool enc = false, bool emerg = false, Proto p = Proto::P25p1) {
        GrantEvent g; g.proto = p; g.tg = tg; g.src = src; g.freqHz = hz; g.slot = slot; g.encrypted = enc; g.emergency = emerg;
        for (auto* r : rx) r->now = t;
        pool.onGrant(g, t);
    }
    int active() const { return (int)pool.nowTalking().size(); }
    const CallRecord* last() const { return pool.calls().empty() ? nullptr : &pool.calls().back(); }
};


static void siteGrant(Sim& s, int site, double q, int tg, double hz, int src = 100) {
    GrantEvent g; g.proto = Proto::P25p1; g.tg = tg; g.src = src; g.freqHz = hz; g.site = site; g.qualityDb = q;
    for (auto* r : s.rx) r->now = s.t;
    s.pool.onGrant(g, s.t);
}

int main() {
    const double F = 851.0e6;
    // 1. two sites grant the same call: merged, one radio, one row
    {
        PoolConfig c; c.dedupSites = true;
        Sim s(c);
        s.add("ctl", F);
        s.add("v1", 0, 0, 0, false);
        s.add("v2", 0, 0, 0, false);
        siteGrant(s, 1, 20, 500, F + 1e6);
        s.run(0.1);
        siteGrant(s, 2, 18, 500, F + 3e6);
        CHECK(s.active() == 1 && s.pool.calls().size() == 1, "merged: %d active, %zu rows", s.active(), s.pool.calls().size());
        CHECK(s.pool.calls()[0].duplicates == 1 && s.pool.stats().duplicates == 1 && s.pool.calls()[0].altSites.size() == 1, "duplicate counted");
        siteGrant(s, 2, 18, 500, F + 3e6);   // repeats from the second site do not count again
        CHECK(s.pool.calls()[0].duplicates == 1, "counted once per site");
        siteGrant(s, 1, 20, 600, F + 4e6);   // another talkgroup uses the free radio
        CHECK(s.active() == 2 && s.pool.stats().missedBusy == 0, "the second radio is free for another call");
        // 1b. the old behaviour (switch off): two radios used for one call, two rows
        PoolConfig o; o.dedupSites = false;
        Sim t(o);
        t.add("ctl", F); t.add("v1", 0, 0, 0, false); t.add("v2", 0, 0, 0, false);
        siteGrant(t, 1, 20, 500, F + 1e6); t.run(0.1); siteGrant(t, 2, 18, 500, F + 3e6);
        CHECK(t.pool.stats().duplicates == 0 && t.pool.stats().followed >= 1, "off: not merged");
    }
    // 2. a clearly better copy within the first seconds takes the call over, and no flip-flop afterwards
    {
        Sim s;
        s.add("ctl", F); s.add("v1", 0, 0, 0, false); s.add("v2", 0, 0, 0, false);
        siteGrant(s, 1, 10, 500, F + 1e6);
        s.run(0.5);
        siteGrant(s, 2, 22, 500, F + 3e6);
        s.run(0.4);
        const CallRecord* a = s.pool.nowTalking().empty() ? nullptr : s.pool.nowTalking()[0];
        CHECK(a && a->site == 2 && std::fabs(a->freqHz - (F + 3e6)) < 1, "moved to the better site");
        const size_t rows = s.pool.calls().size();
        for (int i = 0; i < 10; i++) { s.run(0.2); siteGrant(s, 1, 10, 500, F + 1e6); siteGrant(s, 2, 22, 500, F + 3e6); }
        CHECK(s.pool.calls().size() == rows && s.active() == 1, "no flip-flop: %zu rows", s.pool.calls().size());
    }
    // 3. a better copy late in the call does not interrupt it; a different caller is a different call
    {
        Sim s;
        s.add("ctl", F); s.add("v1", 0, 0, 0, false); s.add("v2", 0, 0, 0, false);
        siteGrant(s, 1, 10, 500, F + 1e6);
        s.run(4.0);
        siteGrant(s, 2, 30, 500, F + 3e6);
        const CallRecord* a = s.pool.nowTalking().empty() ? nullptr : s.pool.nowTalking()[0];
        CHECK(a && a->site == 1 && s.active() == 1, "late better copy: the call stays");
        siteGrant(s, 2, 30, 500, F + 3e6, 777);   // another unit talks on the same talkgroup at the other site
        CHECK(s.active() == 2, "another caller: its own call (%d)", s.active());
    }
    // 4. capacity: 3 radios, 40 calls on 20 talkgroup bursts, each granted by 2 sites; compare the calls missed for lack of a radio
    {
        long missedOn = 0, missedOff = 0, totalOn = 0, totalOff = 0;
        for (int mode = 0; mode < 2; mode++) {
            PoolConfig c; c.dedupSites = mode == 0;
            Sim s(c);
            s.add("ctl", F); s.add("v1", 0, 0, 0, false); s.add("v2", 0, 0, 0, false); s.add("v3", 0, 0, 0, false);
            std::mt19937 g(11);
            for (int i = 0; i < 40; i++) {
                const int tg = 1000 + (int)(g() % 30);
                const double hz = F + 1e6 + 25e3 * (double)(g() % 40);
                siteGrant(s, 1, 15, tg, hz, 100 + tg);
                s.run(0.05);
                siteGrant(s, 2, 12, tg, hz + 7e6, 100 + tg);   // the copy at the second site on another channel
                s.run(1.0 + (double)(g() % 20) / 10.0);
            }
            s.run(5);
            (mode == 0 ? missedOn : missedOff) = (long)s.pool.stats().missedBusy;
            (mode == 0 ? totalOn : totalOff) = (long)s.pool.stats().calls;
        }
        printf("capacity (3 radios, 40 calls, each granted by 2 sites): calls logged %ld -> %ld, missed for lack of a radio %ld -> %ld\n", totalOff, totalOn, missedOff, missedOn);
        CHECK(totalOn < totalOff, "fewer rows");
        CHECK(missedOn <= missedOff, "no more missed calls with the merge");
    }
    return testSummary("trunkpool dedup");
}
