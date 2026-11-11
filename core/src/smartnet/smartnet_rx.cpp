// Motorola Type II receiver: FM front end at 28.8 kHz (8 samples per bit). Search: at every sample phase the 84 most recent bits are checked for the sync word and the CRC; two good
// words one frame apart give the lock. Locked: each frame is read where the clock says it is, a sample either side, so a clock difference is followed. On top: the message layer, the
// trunk pool with one wide capture as its radio (CaptureVoice), the talkgroup, site and patch tables, and the sound of one call.
#include "dect2/smartnet/smartnet_rx.h"
#include "dect2/audio/audioout.h"
#include "dect2/dsp/fm_front.h"
#include "dect2/smartnet/smartnet_tel.h"
#include "dect2/trunkpool/capture_voice.h"
#include "dect2/trunkpool/trunkpool.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>

namespace dect2 {

ModeTuning smartnetTuning() {
    ModeTuning t;
    t.stdMode = 322;
    t.id = "smartnet";
    t.name = "Motorola Type II trunking";
    t.minMhz = 29.7;
    t.maxMhz = 960.0;
    t.defMhz = 851.0125;
    t.sampleRate = 960000;           // wide enough to take calls on neighbouring channels; more is better
    t.basebandHz = 800000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 48000;
    t.tuneOffsetHz = 100000;
    return t;
}

namespace {
using namespace smartnet;
constexpr double kWork = 28800.0;
constexpr int kSps = 8, kFrameSamples = kFrameBits * kSps;
constexpr int kRing = 4096, kMask = kRing - 1;
constexpr double kMinRangeHz = 250.0, kLockSec = 5.0, kPlayRate = 48000.0;
constexpr int kMissMax = 6, kSyncErrMax = 1;
int pop8(uint32_t x) { return __builtin_popcount(x & 0xFF); }
}

struct SmartnetReceiver::Impl {
    std::mutex mu;
    SmartnetTelemetry tel;
    std::function<void(const std::string&)> log;
    FmFront fm;
    FmBiquad post;
    EyeEnvelope env;
    std::vector<float> disc;
    double fs = 0, offHz = 0, centreHz = 0;
    bool rateSet = false;
    uint64_t n = 0;
    float ring[kRing] = {};
    uint64_t lo[kSps] = {}, hi[kSps] = {};
    struct Cand { uint64_t end; bool flip; Osw o; int fixed; };
    std::vector<Cand> cands;
    bool locked = false, flip = false;
    uint64_t nextEnd = 0;
    int misses = 0;
    double lastGoodSec = -1e9, lastPub = 0;
    double snrAcc = 0; int snrN = 0;

    Band band = Band::Auto, active = Band::Domestic800;
    CustomPlan custom;
    bool bandAuto = true;
    Parser parser;
    std::map<int, bool> tgDigital;
    std::vector<int> followGroups;
    bool follow = true, followPrivate = false;
    int listenTg = -1, listenSlice = -1;
    std::function<void(const Osw&)> oswTap;
    std::function<void(int, const float*, size_t, int)> audioTap;
    std::unique_ptr<AudioOut> audioOut;
    std::vector<float> stereo;
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    tpool::CaptureVoice voice{4};
    std::unique_ptr<tpool::TrunkPool> pool;
    std::string recordDir;
    std::map<int, SmartnetPatch> patches;

    double timeSec() const { return (double)n / kWork; }
    double tuned() const { return centreHz > 0 ? centreHz + offHz : 0; }
    void applyBand(Band b) { active = b; parser.setBand(b, custom); tel.band = bandName(b); }
    void setupPool() {
        tpool::PoolConfig c;
        c.hangSec = 2.0;
        c.maxSetupSec = 2.5;
        c.systemName = "typeii";
        c.recordDir = recordDir; c.record = !recordDir.empty(); c.recordMetaOnly = false;
        c.promoteControl = false;
        c.maxCalls = std::max(8, voice.slots());
        pool = std::make_unique<tpool::TrunkPool>(c);
        pool->wallBase = (int64_t)std::time(nullptr);
        pool->addDeviceRef(&voice);
        pool->setControl(0);
    }
    void chooseStartBand() {
        if (!bandAuto) { applyBand(band); return; }
        const Band b = bandFromControl(-1, tuned());
        applyBand(b == Band::Auto ? Band::Domestic800 : b);
    }

    void clearState() {
        fm.reset(); post.clear(); env.clear();
        n = 0; for (auto& r : ring) r = 0; for (auto& r : lo) r = 0; for (auto& r : hi) r = 0;
        cands.clear(); locked = false; misses = 0; lastGoodSec = -1e9; lastPub = 0; snrAcc = 0; snrN = 0;
        const uint64_t s = tel.seq;
        tel = SmartnetTelemetry();
        tel.seq = s + 1; tel.inputRate = fs; tel.centerHz = centreHz; tel.state = rateSet ? 1 : 0;
        tel.follow = follow; tel.followPrivate = followPrivate; tel.listenTg = listenTg; tel.bandAuto = bandAuto;
        parser.clear(); tgDigital.clear(); patches.clear(); listenSlice = -1;
        voice.configure(rateSet ? fs : 0); voice.setCentreHz(centreHz);
        setupPool();
        chooseStartBand();
    }

    // ---- messages
    void addRow(const Osw& o, const std::string& text) {
        if (text.empty()) return;
        SmartnetRow r;
        r.wallTime = (int64_t)std::time(nullptr); r.timeSec = timeSec(); r.addr = o.addr; r.cmd = o.cmd; r.group = o.group; r.text = text;
        if (tel.rows.size() >= kSmartnetRowsMax) tel.rows.pop_back();
        tel.rows.insert(tel.rows.begin(), r);
    }
    void touchTg(const Msg& m) {
        auto it = std::find_if(tel.talkgroups.begin(), tel.talkgroups.end(), [&](const SmartnetTalkgroup& t) { return t.tg == m.tg; });
        SmartnetTalkgroup t;
        if (it != tel.talkgroups.end()) { t = *it; tel.talkgroups.erase(it); } else t.tg = m.tg;
        if (m.kind == Kind::GroupGrant) t.calls++;
        if (m.src >= 0) t.lastSrc = m.src;
        if (m.freqHz > 0) t.lastFreqHz = m.freqHz;
        t.lastWall = (int64_t)std::time(nullptr);
        t.digital = tgDigital.count(m.tg) ? tgDigital[m.tg] : false;
        t.encrypted = m.encrypted; t.patched = m.patch || m.multiselect;
        tel.talkgroups.insert(tel.talkgroups.begin(), t);
        if (tel.talkgroups.size() > kSmartnetTgMax) tel.talkgroups.pop_back();
    }
    bool wanted(const Msg& m) const {
        if (!follow) return false;
        if (!m.group && !followPrivate) return false;
        if (!followGroups.empty() && m.group && std::find(followGroups.begin(), followGroups.end(), m.tg) == followGroups.end()) return false;
        return true;
    }
    void grant(const Msg& m) {
        if (m.kind == Kind::GroupGrant) tgDigital[m.tg] = m.digital;
        const bool dig = m.kind == Kind::GroupUpdate ? (tgDigital.count(m.tg) ? tgDigital[m.tg] : false) : m.digital;
        if (m.group) touchTg(m);
        const double now = timeSec();
        pool->onControlSeen(now);
        if (!wanted(m) || m.kind == Kind::InterconnectGrant) return;   // interconnect (telephone) calls are logged only
        tpool::GrantEvent g;
        g.proto = dig ? tpool::Proto::P25p1 : tpool::Proto::MotorolaTypeII;
        g.tg = m.group ? m.tg : m.dst; g.src = m.src; g.freqHz = m.freqHz; g.channel = m.chan;
        g.group = m.group; g.emergency = m.emergency; g.encrypted = m.encrypted;
        pool->onGrant(g, now);
    }
    void handle(const Msg& m, const Osw& o) {
        tel.messages++;
        if (m.kind != Kind::Unknown) tel.messagesKnown++;
        if (m.kind != Kind::Idle) addRow(o, describe(m));
        switch (m.kind) {
        case Kind::GroupGrant: case Kind::GroupUpdate: case Kind::PrivateGrant: case Kind::InterconnectGrant: grant(m); break;
        case Kind::ControlChannel:
            if (m.system >= 0) tel.systemId = m.system;
            if (bandAuto && m.chan >= 0 && tuned() > 0) {
                const Band b = bandFromControl(m.chan, tuned());
                if (b != Band::Auto && b != active) applyBand(b);
            }
            tel.controlHz = m.freqHz;
            break;
        case Kind::SystemBeacon: tel.systemId = m.system; break;
        case Kind::AltControl: case Kind::AdjacentControl: if (m.system >= 0) tel.systemId = m.system; break;
        case Kind::SiteInfo: break;
        case Kind::AdjacentSite: case Kind::AltSite: {
            tel.systemId = m.system;
            if (m.kind == Kind::AltSite) { tel.siteId = m.site; tel.features = m.features; if (isWideArea(m.features)) tel.smartZone = true; }
            else tel.smartZone = true;
            auto it = std::find_if(tel.sites.begin(), tel.sites.end(), [&](const SmartnetSiteRow& s) { return s.system == m.system && s.site == m.site; });
            SmartnetSiteRow s; s.system = m.system; s.site = m.site; s.band = m.band; s.features = m.features; s.controlHz = m.freqHz; s.adjacent = m.kind == Kind::AdjacentSite;
            if (it != tel.sites.end()) *it = s; else tel.sites.push_back(s);
            break;
        }
        case Kind::SystemStatus: if (m.opcode == 1) { tel.typeIIKnown = true; tel.typeII = m.typeII; } break;
        case Kind::Patch: { SmartnetPatch p; p.tg = m.tg; p.member = m.dst; p.lastSec = timeSec(); patches[(m.tg << 16) ^ m.dst] = p; break; }
        case Kind::PatchCancel: for (auto it = patches.begin(); it != patches.end();) { if (it->second.tg == m.tg) it = patches.erase(it); else ++it; } break;
        default: break;
        }
    }
    void goodOsw(const Osw& o, int fixed) {
        tel.oswOk++; tel.blocksOk++;
        tel.oswFixed += (uint64_t)fixed;
        tel.dataValid = true;
        lastGoodSec = timeSec();
        if (oswTap) oswTap(o);
        parser.feed(o);
        pump(o);
    }
    void badOsw() { tel.oswBad++; tel.blocksBad++; parser.bad(); Osw z; pump(z); }
    void pump(const Osw& o) {
        Msg m;
        while (parser.next(m)) handle(m, o);
    }

    // ---- bits
    double soft(uint64_t i) const { return (ring[i & kMask] + ring[(i - 1) & kMask] + ring[(i - 2) & kMask]) / 3.0; }
    bool bitAt(uint64_t i, double thr) const { return soft(i) > thr; }
    // the 84 bits that end with sample e (the count of the sample after the last one)
    void readFrame(uint64_t e, uint8_t out[kFrameBits], double thr) const {
        for (int i = 0; i < kFrameBits; i++) out[i] = bitAt(e - 1 - (uint64_t)(kFrameBits - 1 - i) * kSps, thr) ? 1 : 0;
    }
    double eyeAt(uint64_t e, double thr) const {
        double sum = 0;
        for (int i = 0; i < kFrameBits; i++) sum += std::fabs(soft(e - 1 - (uint64_t)i * kSps) - thr);
        return sum;
    }
    void lockedFrame() {
        const double thr = env.mid();
        // every alignment that the CRC accepts is a candidate; the one with the widest eye sets the clock, so timing converges to the middle of the bit
        double bestEye = -1; Osw bestO; int bestFixed = 0; uint64_t bestE = 0;
        for (int d = -3; d <= 3; d++) {
            const uint64_t e = nextEnd + (uint64_t)(int64_t)d;
            uint8_t b[kFrameBits];
            readFrame(e, b, thr);
            if (flip) for (auto& v : b) v ^= 1;
            uint32_t s = 0;
            for (int j = 0; j < kSyncBits; j++) s = (s << 1) | b[j];
            if (pop8(s ^ kSync) > kSyncErrMax) continue;
            Osw o; int fixed = 0;
            if (!decodePayload(b + kSyncBits, o, &fixed)) continue;
            const double eye = eyeAt(e, thr);
            if (eye > bestEye) { bestEye = eye; bestO = o; bestFixed = fixed; bestE = e; }
        }
        if (bestEye >= 0) {
            nextEnd = bestE + kFrameSamples;
            misses = 0;
            eyeStats(bestE, thr);
            goodOsw(bestO, bestFixed);
            return;
        }
        nextEnd += kFrameSamples;
        if (++misses >= kMissMax) { locked = false; cands.clear(); parser.bad(); tel.oswBad += (uint64_t)misses; misses = 0; Osw z; pump(z); return; }
        badOsw();
    }
    void eyeStats(uint64_t e, double thr) {
        double hi2 = 0, lo2 = 0, varH = 0, varL = 0; int nH = 0, nL = 0;
        for (int i = 0; i < kFrameBits; i++) {
            const double v = soft(e - 1 - (uint64_t)i * kSps) - thr;
            if (v > 0) { hi2 += v; varH += v * v; nH++; } else { lo2 += v; varL += v * v; nL++; }
        }
        if (nH > 4 && nL > 4) {
            const double mh = hi2 / nH, ml = lo2 / nL;
            const double s2 = (varH / nH - mh * mh + varL / nL - ml * ml) * 0.5;
            if (s2 > 1e-3) { snrAcc += 10 * std::log10((mh - ml) * (mh - ml) * 0.25 / s2); snrN++; }
        }
    }
    void acquire(int ph) {
        if (env.range() < kMinRangeHz) return;
        const uint32_t s = (uint32_t)((hi[ph] >> 12) & 0xFF);
        const bool plain = s == kSync, inv = s == (~kSync & 0xFF);
        if (!plain && !inv) return;
        uint8_t b[kPayloadBits];
        for (int j = 0; j < kPayloadBits; j++) {
            const int idx = kPayloadBits - 1 - j;
            const uint64_t r = idx < 64 ? lo[ph] >> idx : hi[ph] >> (idx - 64);
            b[j] = (uint8_t)((r & 1) ^ (inv ? 1 : 0));
        }
        Osw o; int fixed = 0;
        if (!decodePayload(b, o, &fixed)) return;
        const uint64_t e = n;
        for (const Cand& c : cands) {
            const int64_t d = (int64_t)e - (int64_t)c.end - kFrameSamples;
            if (c.flip == inv && d >= -2 && d <= 2) {
                locked = true; flip = inv; tel.inverted = inv; nextEnd = e + kFrameSamples; misses = 0;
                goodOsw(c.o, c.fixed);
                goodOsw(o, fixed);
                cands.clear();
                return;
            }
        }
        while (cands.size() >= 32) cands.erase(cands.begin());
        cands.erase(std::remove_if(cands.begin(), cands.end(), [&](const Cand& c) { return e - c.end > (uint64_t)kFrameSamples + 6; }), cands.end());
        cands.push_back({e, inv, o, fixed});
    }
    void sample(double s) {
        ring[n & kMask] = (float)s;
        env.push(s);
        const int ph = (int)(n % kSps);
        const uint64_t b = soft(n) > env.mid() ? 1 : 0;
        hi[ph] = (hi[ph] << 1) | (lo[ph] >> 63);
        lo[ph] = (lo[ph] << 1) | b;
        n++;
        if (locked) { if (n >= nextEnd + 4) lockedFrame(); }
        else if (n > 1024) acquire(ph);
    }

    // ---- sound
    void chooseListen() {
        listenSlice = -1;
        const tpool::CallRecord* best = nullptr;
        for (const tpool::CallRecord* r : pool->nowTalking()) {
            if (r->slice < 0) continue;
            if (listenTg >= 0 && r->tg != listenTg) continue;
            if (!best || r->startSec > best->startSec) best = r;
        }
        if (best) listenSlice = best->slice;
    }
    int tgOfSlice(int s) const {
        for (const tpool::CallRecord* r : pool->nowTalking()) if (r->slice == s) return r->tg;
        return -1;
    }
    void audio(int slice, const float* x, size_t cnt, int rate) {
        if (audioTap) audioTap(tgOfSlice(slice), x, cnt, rate);
        if (slice != listenSlice || silent.load()) return;
        const int up = rate > 0 ? (int)std::lround(kPlayRate / rate) : 1;
        stereo.resize(cnt * (size_t)up * 2);
        for (size_t i = 0; i < cnt; i++)
            for (int k = 0; k < up; k++) stereo[(i * (size_t)up + (size_t)k) * 2] = stereo[(i * (size_t)up + (size_t)k) * 2 + 1] = x[i];
        if (!audioOut) { audioOut = std::make_unique<AudioOut>(); audioOut->start(48000); audioOut->setStartThreshold(48000 / 5); }
        audioOut->setVolume(volume.load()); audioOut->setMuted(muted.load());
        if (audioOut->bufferedFrames() > 48000 * 3 / 2) audioOut->flush();
        audioOut->write(stereo.data(), (int)(cnt * (size_t)up));
    }

    void publish() {
        const double now = timeSec();
        pool->tick(now);
        chooseListen();
        tel.levelDb = (float)fm.levelDb();
        tel.deviationHz = (float)env.range();
        tel.timeSec = now;
        tel.locked = now - lastGoodSec < kLockSec;
        tel.state = rateSet ? (tel.locked ? 2 : 1) : 0;
        if (snrN > 0) { tel.snrDb = (float)(snrAcc / snrN); snrAcc = 0; snrN = 0; }
        tel.band = bandName(active); tel.bandAuto = bandAuto;
        tel.follow = follow; tel.followPrivate = followPrivate; tel.listenTg = listenTg;
        const auto& st = pool->stats();
        tel.grants = st.grants; tel.followed = st.followed; tel.missedOutside = st.missedOutside; tel.missedBusy = st.missedBusy; tel.encrypted = st.encrypted;
        tel.poolStatus = pool->statusLine(now);
        tel.voiceSlots = voice.slots();
        for (auto& t : tel.talkgroups) t.active = false;
        tel.calls.clear();
        tel.audioOpen = false;
        const auto& calls = pool->calls();
        for (auto it = calls.rbegin(); it != calls.rend() && tel.calls.size() < kSmartnetCallsMax; ++it) {
            SmartnetCall c;
            c.id = it->id; c.tg = it->tg; c.src = it->src; c.freqHz = it->freqHz; c.group = it->group; c.digital = it->proto == tpool::Proto::P25p1;
            c.emergency = it->emergency; c.encrypted = it->encrypted; c.followed = it->followed; c.active = it->active(); c.missed = it->missed;
            c.wallStart = it->wallStart; c.durationSec = c.active ? now - it->startSec : it->durationSec(); c.audioSec = it->audioSec;
            c.state = tpool::callStateName(it->state); c.note = it->note;
            c.listening = c.active && it->slice >= 0 && it->slice == listenSlice;
            c.voiceSlot = c.active && it->followed ? it->slice : -1;
            if (c.listening && !muted.load()) tel.audioOpen = true;
            if (c.active && c.group) for (auto& t : tel.talkgroups) if (t.tg == c.tg) t.active = true;
            tel.calls.push_back(std::move(c));
        }
        std::stable_sort(tel.calls.begin(), tel.calls.end(), [](const SmartnetCall& a, const SmartnetCall& b) { return a.active > b.active; });
        tel.patches.clear();
        for (const auto& kv : patches) tel.patches.push_back(kv.second);
        tel.note.clear();
        if (active == Band::Domestic800 && bandAuto && tuned() > 0 && !(tuned() >= 851e6 && tuned() <= 870e6)) tel.note = "The frequency is outside the 800 and 900 MHz plans: enter the channel plan of this site under Advanced to place the calls.";
        else if (centreHz <= 0) tel.note = "The frequency of the capture is not known (file input): the calls cannot be placed.";
        tel.seq++;
    }
};

SmartnetReceiver::SmartnetReceiver() : p_(std::make_unique<Impl>()) { p_->setupPool(); p_->applyBand(Band::Domestic800); }
SmartnetReceiver::~SmartnetReceiver() = default;

void SmartnetReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->fs = inputRateHz;
    p_->rateSet = inputRateHz >= smartnetTuning().minSampleRate - 1 && p_->fm.configure(inputRateHz, kWork, 7000.0);
    p_->fm.setOffset(p_->offHz);
    p_->post.lowpass(2700.0, kWork, 0.7071);
    p_->voice.setAudioSink([this](int s, const float* x, size_t n, int r) { p_->audio(s, x, n, r); });
    p_->clearState();
}
void SmartnetReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offHz = hz; p_->fm.setOffset(hz); p_->chooseStartBand(); }
void SmartnetReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->centreHz = hz; p_->tel.centerHz = hz; p_->voice.setCentreHz(hz); p_->chooseStartBand(); }
bool SmartnetReceiver::ready() const { return p_->rateSet; }
void SmartnetReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); p_->clearState(); }
void SmartnetReceiver::clearMessages() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.rows.clear(); p_->tel.talkgroups.clear(); p_->tel.sites.clear(); p_->patches.clear(); p_->tel.calls.clear(); p_->pool->clear(); p_->tel.messages = p_->tel.messagesKnown = 0; p_->tel.seq++;
}
void SmartnetReceiver::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rateSet) { p_->tel.state = 0; return; }
    p_->disc.clear();
    p_->fm.feed(x, n, p_->disc);
    for (float d : p_->disc) p_->sample(p_->post.run(d));
    p_->voice.feed(x, n);
    if (p_->timeSec() - p_->lastPub >= 0.1) { p_->lastPub = p_->timeSec(); p_->publish(); }
}
bool SmartnetReceiver::telemetry(SmartnetTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}
void SmartnetReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void SmartnetReceiver::setBand(smartnet::Band b, const smartnet::CustomPlan& c) { std::lock_guard<std::mutex> lk(p_->mu); p_->band = b; p_->custom = c; p_->bandAuto = b == smartnet::Band::Auto; p_->chooseStartBand(); }
void SmartnetReceiver::setFollow(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->follow = on; p_->tel.follow = on; }
void SmartnetReceiver::setFollowPrivate(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->followPrivate = on; p_->tel.followPrivate = on; }
void SmartnetReceiver::setFollowGroups(const std::vector<int>& g) { std::lock_guard<std::mutex> lk(p_->mu); p_->followGroups = g; }
void SmartnetReceiver::setVoiceSlots(int n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->voice.setSlots(n);
    tpool::PoolConfig c = p_->pool->config(); c.maxCalls = std::max(8, p_->voice.slots()); p_->pool->setConfig(c);
    p_->tel.voiceSlots = p_->voice.slots();
}
void SmartnetReceiver::setListenTg(int tg) { std::lock_guard<std::mutex> lk(p_->mu); p_->listenTg = tg; p_->tel.listenTg = tg; }
void SmartnetReceiver::setRecordDir(const std::string& d) { std::lock_guard<std::mutex> lk(p_->mu); p_->recordDir = d; if (p_->pool) { auto c = p_->pool->config(); c.recordDir = d; c.record = !d.empty(); p_->pool->setConfig(c); } }
void SmartnetReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void SmartnetReceiver::setMuted(bool m) { p_->muted = m; }
void SmartnetReceiver::setSilent(bool s) { p_->silent = s; }
void SmartnetReceiver::setOswTap(std::function<void(const smartnet::Osw&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->oswTap = std::move(cb); }
void SmartnetReceiver::setAudioTap(std::function<void(int, const float*, size_t, int)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->audioTap = std::move(cb); }

} // namespace dect2
