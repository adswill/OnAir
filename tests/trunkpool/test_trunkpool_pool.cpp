// Trunk pool: synthetic multi-channel harness. A fake control channel emits grants, fake voice radios retune with a latency and can fail or be unplugged,
// and a simulated clock drives the pool. Covers allocation, retune latency, hang and terminator, priority preemption, access lists, encrypted calls,
// device loss and control promotion, the single-radio wide / narrow cases, the channel plan CSV and the recorder.
#include "dect2/trunkpool/trunkpool.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
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

static int countRole(const TrunkPool& p, Role r) { int n = 0; for (const DeviceInfo& d : p.devices()) n += d.role == r; return n; }

int main() {
    const double F = 851.0e6;
    // 1. one grant, one idle voice radio: retune, active, hang, release
    {
        Sim s;
        s.add("ctl", F, 0, 0);
        FakeRx* v1 = s.add("v1", 0, 0, 0, false);
        s.pool.setControlChannel(F);
        s.grant(1001, F + 1.0e6);
        CHECK(s.active() == 1 && s.last()->followed && v1->tunes == 1 && v1->freq == F + 1.0e6, "grant takes the voice radio");
        CHECK(s.last()->state == CallState::Tuning, "tuning first");
        s.run(0.4);
        CHECK(s.last()->state == CallState::Active && std::fabs(s.last()->setupSec - 0.25) < 0.1, "active after the latency: %.3f", s.last()->setupSec);
        CHECK(s.pool.stats().retunes == 1 && std::fabs(s.pool.stats().retuneAvgSec - 0.25) < 0.1, "retune stat");
        CHECK(s.pool.device(1).role == Role::Voice && s.pool.device(1).tg == 1001, "device table: voice on TG 1001");
        for (int i = 0; i < 20; i++) { s.run(0.1); s.grant(1001, F + 1.0e6); }   // repeats keep it
        CHECK(s.active() == 1 && s.pool.calls().size() == 1, "repeats keep one call, %zu rows", s.pool.calls().size());
        v1->voice = false;
        s.run(2.5);
        CHECK(s.active() == 0 && s.last()->endReason == "hang time over" && v1->releases == 1 && s.pool.device(1).role == Role::Idle, "hang time ends it: %s", s.last()->endReason.c_str());
        s.grant(1001, F + 1.0e6);   // a late repeat right after the end
        CHECK(s.active() == 0, "cooldown: no reopen");
    }
    // 2. terminator ends at once; two calls in parallel on two radios; a third is busy
    {
        Sim s;
        s.add("ctl", F);
        FakeRx* v1 = s.add("v1", 0, 0, 0, false);
        s.add("v2", 0, 0, 0, false);
        s.grant(1, F + 1e6); s.grant(2, F + 2e6); s.grant(3, F + 3e6);
        CHECK(s.active() == 2 && s.pool.stats().missedBusy == 1, "two follow, one busy (%d, %llu)", s.active(), (unsigned long long)s.pool.stats().missedBusy);
        CHECK(s.pool.calls()[2].missed && s.pool.calls()[2].note.find("busy") == 0, "busy note");
        s.run(0.5);
        v1->term = true;
        s.run(0.2);
        CHECK(s.active() == 1, "terminator frees one radio");
        s.grant(3, F + 3e6);   // the repeat of the busy call is picked up in the same row
        CHECK(s.active() == 2 && s.pool.calls().size() == 3 && s.pool.calls()[2].followed && !s.pool.calls()[2].missed, "busy call picked up by its repeat, rows %zu", s.pool.calls().size());
    }
    // 3. priority preemption and emergency
    {
        Sim s;
        s.add("ctl", F);
        s.add("v1", 0, 0, 0, false);
        AccessRule lo; lo.lo = lo.hi = 10; lo.priority = 8;
        AccessRule hi; hi.lo = hi.hi = 20; hi.priority = 2;
        s.pool.access.add(lo); s.pool.access.add(hi);
        s.grant(10, F + 1e6);
        s.run(0.4);
        s.grant(11, F + 2e6);   // unlisted, no priority: not above 8? rank 11 is worse than 8: busy
        CHECK(s.active() == 1 && s.pool.nowTalking()[0]->tg == 10, "worse priority does not preempt");
        s.grant(20, F + 3e6);
        CHECK(s.active() == 1 && s.pool.nowTalking()[0]->tg == 20 && s.pool.stats().preempted == 1, "priority talkgroup preempts");
        CHECK(s.pool.calls()[0].endReason == "preempted", "preempted reason");
        s.run(0.4);
        s.grant(30, F + 4e6, 1, -1, false, true);   // emergency beats priority 2
        CHECK(s.active() == 1 && s.pool.nowTalking()[0]->tg == 30 && s.pool.stats().preempted == 2, "emergency preempts");
        s.grant(21, F + 5e6);   // an unlisted call must not take the emergency
        CHECK(s.pool.nowTalking()[0]->tg == 30, "emergency stays");
    }
    // 4. access lists: deny, unit deny, whitelist, encrypted
    {
        Sim s;
        s.add("ctl", F);
        s.add("v1", 0, 0, 0, false);
        AccessRule d; d.lo = 500; d.hi = 599; d.act = AccessRule::Deny; d.label = "ops";
        s.pool.access.add(d);
        AccessRule u; u.kind = AccessRule::Unit; u.lo = u.hi = 777; u.act = AccessRule::Deny;
        s.pool.access.add(u);
        s.grant(505, F + 1e6);
        CHECK(s.active() == 0 && s.last()->note.find("denied") != std::string::npos && s.pool.stats().missedDenied == 1, "denied range");
        s.grant(505, F + 1e6);
        CHECK(s.pool.calls().size() == 1, "denied is logged once");
        s.grant(600, F + 2e6, 777);
        CHECK(s.active() == 0 && s.last()->note.find("unit denied") == 0, "unit deny");
        s.grant(601, F + 2e6, 100, -1, true);
        CHECK(s.active() == 0 && s.last()->encrypted && s.pool.stats().encrypted == 1 && s.pool.device(1).calls == 0, "encrypted never allocated");
        s.pool.access.allowByDefault = false;
        s.grant(602, F + 3e6);
        CHECK(s.active() == 0, "whitelist mode: unlisted not followed");
        AccessRule a; a.lo = a.hi = 602;
        s.pool.access.add(a);
        s.grant(602, F + 3e6);
        CHECK(s.active() == 1, "whitelisted followed");
        // encrypted heard on the voice channel
        s.run(0.4);
        s.rx[1]->enc = true;
        s.run(0.2);
        CHECK(s.active() == 0 && s.pool.calls().back().encrypted, "encrypted heard: left");
    }
    // 5. device loss: the call moves to the next grant repeat on another radio; control promotion
    {
        Sim s;
        s.add("ctl", F);
        FakeRx* v1 = s.add("v1", 0, 0, 0, false);
        FakeRx* v2 = s.add("v2", 0, 0, 0, false);
        s.pool.setControlChannel(F);
        s.grant(1, F + 1e6);
        s.run(0.5);
        const int onV1 = v1->tunes ? 1 : 2;
        FakeRx* used = onV1 == 1 ? v1 : v2;
        used->here = false;
        s.run(0.1);
        CHECK(s.active() == 0 && s.pool.stats().deviceLost == 1 && s.pool.device(onV1).role == Role::Lost, "device lost ends the call");
        s.grant(1, F + 1e6);
        CHECK(s.active() == 1 && s.pool.stats().calls == 2, "the next repeat is followed on the other radio, calls %llu", (unsigned long long)s.pool.stats().calls);
        s.run(0.5);
        s.rx[0]->here = false;   // the control radio goes
        s.run(0.1);
        CHECK(s.pool.controlDevice() < 0 || s.pool.device(s.pool.controlDevice()).role == Role::Control, "control state");
        s.pool.tick(s.t);
        CHECK(countRole(s.pool, Role::Control) <= 1, "one control radio at most");
        used->here = true;
        s.run(0.2);
        CHECK(s.pool.device(onV1).role != Role::Lost, "radio back");
    }
    // 5b. control radio lost, an idle radio takes over and is tuned to the control channel
    {
        Sim s;
        FakeRx* c = s.add("ctl", F);
        FakeRx* v = s.add("v1", 0, 0, 0, true);
        s.pool.setControlChannel(F);
        s.pool.onControlSeen(0);
        s.run(0.2);
        c->here = false;
        s.run(0.2);
        CHECK(s.pool.controlDevice() == 1 && v->freq == F && s.pool.device(1).role == Role::Control, "promoted and tuned");
    }
    // 6. single radio, wide capture: the grant inside the capture is a slice, outside is control only with a message
    {
        Sim s;
        FakeRx* c = s.add("ctl", F, 2.4e6, 2);
        s.pool.setControlChannel(F);
        s.grant(7, F + 0.5e6);
        CHECK(s.active() == 1 && s.last()->slice >= 0 && c->tunes == 0, "inside the capture: a slice, no retune");
        s.grant(8, F + 0.6e6);
        s.grant(9, F + 0.7e6);
        CHECK(s.active() == 2 && s.pool.stats().missedOutside + s.pool.stats().missedBusy >= 1, "slice limit");
        s.run(0.2);
        s.grant(70, F + 5e6);
        CHECK(s.last()->missed && s.last()->note.find("outside the capture") == 0, "outside: %s", s.last()->note.c_str());
        CHECK(s.pool.statusLine(s.t).find("Control channel only") == 0, "status says control only: %s", s.pool.statusLine(s.t).c_str());
        bool msg = false;
        for (auto& m : s.pool.messages) msg = msg || m.find("second radio") != std::string::npos;
        CHECK(msg, "notice mentions a second radio");
        const size_t n = s.pool.messages.size();
        s.grant(71, F + 5e6);
        CHECK(s.pool.messages.size() == n, "one notice per channel");
        c->voice = false;
        s.run(3.0);
        CHECK(s.active() == 0 && c->sliceUsed[0] == 0, "slices freed");
        // a narrow single radio
        Sim n2;
        n2.add("ctl", F, 0, 0);
        n2.grant(1, F + 100e3);
        CHECK(n2.last()->missed && n2.last()->note.find("outside") == 0, "narrow radio: control only");
        n2.add("v", 0, 0, 0, false);
        n2.run(0.5);
        n2.grant(1, F + 100e3);
        CHECK(n2.active() == 1 && n2.pool.statusLine(n2.t).find("Following 1 call") == 0, "after a radio is added the call is followed: %s", n2.pool.statusLine(n2.t).c_str());
    }
    // 7. no signal on the voice channel: dropped, back-off
    {
        Sim s;
        s.add("ctl", F);
        FakeRx* v = s.add("v1", 0, 0, 0, false);
        v->failTune = true;
        s.grant(5, F + 1e6);
        s.run(0.5);
        CHECK(s.active() == 0 && s.pool.stats().missedNoSignal == 1 && s.last()->note.find("no signal") == 0 && v->releases == 1, "failed tune dropped");
        s.grant(5, F + 1e6);
        CHECK(s.active() == 0 && v->tunes == 1, "back-off holds");
        s.run(7.0);
        v->failTune = false;
        s.grant(5, F + 1e6);
        CHECK(s.active() == 1, "after the back-off it is tried again");
        // a radio that refuses (out of range) is skipped for the next
        Sim r;
        r.add("ctl", F);
        FakeRx* a = r.add("a", 0, 0, 0, false);
        FakeRx* b = r.add("b", 0, 0, 0, false);
        a->refuse = true;
        r.grant(1, F + 1e6);
        CHECK(r.active() == 1 && b->tunes == 1 && a->tunes == 0, "refusing radio skipped");
    }
    // 8. a moved call, nearest radio first, call limit
    {
        Sim s;
        s.add("ctl", F);
        FakeRx* a = s.add("a", F + 10e6, 0, 0, false);
        FakeRx* b = s.add("b", F + 1e6, 0, 0, false);
        s.grant(1, F + 1.2e6);
        CHECK(b->tunes == 1 && a->tunes == 0, "nearest radio first");
        s.run(0.5);
        s.grant(1, F + 4e6);   // the same talkgroup on another channel
        CHECK(s.active() == 1 && s.last()->freqHz == F + 4e6 && s.pool.calls().size() == 2, "moved call follows");
        PoolConfig c; c.maxCalls = 1;
        Sim l(c);
        l.add("ctl", F); l.add("a", 0, 0, 0, false); l.add("b", 0, 0, 0, false);
        l.grant(1, F + 1e6); l.grant(2, F + 2e6);
        CHECK(l.active() == 1, "call limit");
    }
    // 9. auto-assign
    {
        Sim s;
        s.add("a", F, 0, 0);
        s.add("b", 0, 0, 0);
        s.add("c", 0, 0, 0);
        s.pool.autoAssign(1);
        CHECK(s.pool.controlDevice() == 1 && countRole(s.pool, Role::Control) == 1 && countRole(s.pool, Role::Idle) == 2, "auto-assign roles");
        s.pool.setEnabled(2, false);
        CHECK(s.pool.device(2).role == Role::Off, "switched off");
        s.grant(1, F + 1e6);
        CHECK(s.active() == 1 && s.pool.device(0).calls == 1 && s.pool.device(2).calls == 0, "voice goes to the enabled radio");
    }
    // 10. recording: one WAV + JSON per call, with the call's data
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "onair_trunkpool_test";
        { std::error_code ec; fs::remove_all(dir, ec); }
        PoolConfig c; c.recordDir = dir.string(); c.systemName = "NSW PSN";
        {   // scoped: the pool closes its files before the folder is removed (Windows cannot remove an open file)
        Sim s(c);
        s.pool.wallBase = 1700000000;
        s.add("ctl", F);
        s.add("v1", 0, 0, 0, false);
        s.grant(4242, F + 1e6, 987);
        s.run(2.0);
        s.rx[1]->voice = false;
        s.run(3.0);
        CHECK(s.active() == 0 && !s.last()->audioPath.empty() && s.last()->audioSec > 0.5, "recording: path '%s' audio %.2f", s.last()->audioPath.c_str(), s.last()->audioSec);
        std::ifstream w(fs::u8path(s.last()->audioPath), std::ios::binary);
        char h[44] = {0};
        w.read(h, 44);
        const uint32_t rate = (uint8_t)h[24] | ((uint8_t)h[25] << 8) | ((uint8_t)h[26] << 16);
        const uint32_t bytes = (uint8_t)h[40] | ((uint8_t)h[41] << 8) | ((uint8_t)h[42] << 16);
        CHECK(std::string(h, 4) == "RIFF" && std::string(h + 8, 4) == "WAVE" && rate == 8000 && bytes > 8000, "wav header rate %u bytes %u", rate, bytes);
        const std::string js = s.last()->audioPath.substr(0, s.last()->audioPath.size() - 4) + ".json";
        std::ifstream j(fs::u8path(js));
        std::stringstream ss; ss << j.rdbuf();
        const std::string t = ss.str();
        CHECK(t.find("\"talkgroup\": 4242") != std::string::npos && t.find("\"source\": 987") != std::string::npos && t.find("\"frequency_hz\": 852000000") != std::string::npos &&
              t.find("hang time over") != std::string::npos && t.find("NSW PSN") != std::string::npos, "sidecar: %s", t.c_str());
        CHECK(s.last()->audioPath.find("NSW_PSN_tg4242_") != std::string::npos, "file name");
        }
        // a call with no sound leaves no wav
        { std::error_code ec; fs::remove_all(dir, ec); }
        Sim q(c);
        q.add("ctl", F); q.add("v1", 0, 0, 0, false);
        q.rx[1]->voice = false;
        q.grant(1, F + 1e6);
        q.run(0.5);
        q.pool.clear();
        CHECK(!fs::exists(dir) || fs::is_empty(dir), "no sound, no files");
        { std::error_code ec; fs::remove_all(dir, ec); }
    }
    // 11. channel plan CSV
    {
        ChannelPlan p;
        std::string err; int skipped = 0;
        const std::string csv = "# plan\nsystem,NSW PSN,0xBEE00,0x2D1,0x293\niden,1,851006250,12500,-45000000,12500\niden_up,2,935.0125,12.5,-39,12.5,2\nchan,5,851.2\nbad,row\niden,99,1,2,3\n";
        const int n = p.importCsv(csv, &err, &skipped);
        CHECK(n == 4 && skipped == 2, "csv rows %d skipped %d (%s)", n, skipped, err.c_str());
        CHECK(p.wacn == 0xBEE00 && p.sysId == 0x2D1 && p.nac == 0x293 && p.name == "NSW PSN", "system row");
        CHECK(std::fabs(p.p25Freq((1 << 12) | 0x10) - (851006250 + 16 * 12500.0)) < 1, "p25 channel %.1f", p.p25Freq((1 << 12) | 0x10));
        CHECK(p.iden(2) && p.iden(2)->slots == 2 && std::fabs(p.iden(2)->txOffsetHz + 39e6) < 1 && std::fabs(p.iden(2)->spacingHz - 12500) < 1, "iden_up in MHz and kHz");
        CHECK(p.p25Freq((9 << 12) | 3) == 0 && std::fabs(p.channelHz(5) - 851.2e6) < 1, "unknown iden, lcn");
        ChannelPlan q;
        CHECK(q.importCsv(p.exportCsv()) == 4 && std::fabs(q.p25Freq((1 << 12) | 0x10) - p.p25Freq((1 << 12) | 0x10)) < 1, "export round trip");
        ChannelPlan t;
        CHECK(t.importCsv("Channel,Frequency\n1,851.0125\n2,851.0375\n3,xx\n", &err, &skipped) == 2 && std::fabs(t.channelHz(2) - 851.0375e6) < 1, "table with a header");
        ChannelPlan junk;
        junk.importCsv(std::string("\x01\xff,,\n\"unterminated,1,2\nchan,-1,5\nchan,3,nan\n"), nullptr, nullptr);
        CHECK(junk.channelCount() == 0, "junk rows taken as nothing");
    }
    // 12. access CSV
    {
        AccessList a;
        std::string err; int sk = 0;
        const int n = a.importCsv("Kind,From,To,Action,Priority,Record,Label\nTG,100,199,Allow,3,yes,Dispatch\nTG,150,150,Deny,0,no,Secret\nUnit,900,,Deny,,,bad radio\nTG,x,1,Allow\n", &err, &sk);
        CHECK(n == 3 && sk == 1, "access rows %d skipped %d", n, sk);
        CHECK(a.decide(120, 5).allow && a.decide(120, 5).priority == 3 && !a.decide(150, 5).allow && !a.decide(120, 900).allow && a.decide(300, 5).allow, "decisions");
        AccessList b;
        b.importCsv(a.exportCsv());
        CHECK(b.rules().size() == 3 && b.decide(120, 5).priority == 3, "round trip");
        AccessList rr;
        rr.importCsv("Decimal,Hex,Alpha Tag,Mode,Tag\n1001,3E9,Fire,D,Fire\n1002,3EA,Secure,DE,Law\n");
        CHECK(rr.decide(1001, 0).allow && !rr.decide(1002, 0).allow, "RadioReference columns: E is denied");
    }
    // 13. protocol adapters
    {
        GrantEvent g = grantMpt1327(12, 10, 150e6, 12500, 1234, 56);
        CHECK(std::fabs(g.freqHz - 150.025e6) < 1 && g.tg == 1234 && g.proto == Proto::Mpt1327, "mpt1327 channel");
        ChannelPlan p; p.setChannel(3, 462.5625e6);
        GrantEvent e = grantFromLcn(Proto::Edacs, p, 3, 77, 8);
        GrantEvent m = grantFromLcn(Proto::Ltr, p, 4, 77, 8);
        CHECK(e.freqHz == 462.5625e6 && m.freqHz == 0, "lcn lookup, unknown lcn has no frequency");
        GrantEvent t = grantTetra(380e6, 1000, 1, 2, 5, 9, false);
        CHECK(std::fabs(t.freqHz - (380e6 + 25e6 + 6250)) < 1 && t.slot == 2 && t.proto == Proto::Tetra, "tetra carrier");
        Sim s;
        s.add("ctl", F); s.add("v", 0, 0, 0, false);
        s.pool.onGrant(m, 1.0);
        CHECK(s.active() == 0 && s.pool.messages.size() == 1 && s.pool.messages[0].find("channel plan") != std::string::npos, "grant with no frequency is explained");
    }
    // 14. random grants and failures never leave the pool inconsistent
    {
        Sim s;
        s.add("ctl", F, 1.0e6, 1);
        for (int i = 0; i < 4; i++) s.add("v" + std::to_string(i), 0, 0, 0, false);
        srand(7);
        for (int step = 0; step < 4000; step++) {
            const int r = rand() % 20;
            if (r < 8) s.grant(1 + rand() % 12, F + (rand() % 40 - 20) * 100e3, rand() % 50, -1, rand() % 15 == 0, rand() % 25 == 0);
            else if (r == 8) s.rx[1 + rand() % 4]->here = rand() % 3 != 0;
            else if (r == 9) s.rx[1 + rand() % 4]->term = true;
            else if (r == 10) s.rx[1 + rand() % 4]->failTune = rand() % 2;
            else if (r == 11) s.rx[1 + rand() % 4]->voice = rand() % 2;
            s.run(0.1);
            int busy = 0;
            for (const DeviceInfo& d : s.pool.devices()) busy += d.call != 0 && d.role == Role::Voice;
            if (busy > 4 || s.active() > 8) { CHECK(false, "inconsistent at step %d: busy %d active %d", step, busy, s.active()); break; }
            if (countRole(s.pool, Role::Control) > 1) { CHECK(false, "two control radios at step %d", step); break; }
        }
        CHECK(true, "random run done");
        for (auto* r : s.rx) { r->voice = false; r->here = true; r->failTune = false; }
        s.run(10);
        CHECK(s.active() == 0, "everything ends");
        for (const DeviceInfo& d : s.pool.devices()) CHECK(d.call == 0 && d.slices == 0, "device %s left a call", d.name.c_str());
        CHECK(!s.pool.callLogCsv().empty() && s.pool.statusLine(s.t).size() > 0, "csv and status");
    }
    return testSummary("trunkpool pool");
}
