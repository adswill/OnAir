// TETRA Direct Mode receiver. Mix and resample to 72 kHz (4 samples per symbol), root raised cosine filter, differential symbol at every sample. The DSB is found by the coherent
// differential correlation against its 38 bit training sequence, which needs no carrier recovery; the rotation of that correlator gives the frequency error. A found DSB is demodulated
// and its SCH/S and SCH/H decoded. With the slot clock of a good DSB, the slots are read as they come for normal bursts (22 bit training sequence).
#include "dect2/tetradmo/tetradmo_rx.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/tetra/tetra_voice.h"
#include "dect2/tetradmo/tetradmo_tel.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <mutex>
#include <vector>

namespace dect2 {

ModeTuning tetraDmoTuning() {
    ModeTuning t;
    t.stdMode = 323;
    t.id = "tetradmo";
    t.name = "TETRA DMO";
    t.minMhz = 30.0;
    t.maxMhz = 1000.0;
    t.defMhz = 427.0;
    t.sampleRate = 240000;
    t.basebandHz = 200000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 72000;
    t.tuneOffsetHz = 50000;
    return t;
}

namespace {
using namespace tetradmo;
using namespace tetra;
constexpr double kWork = kRate;                 // 72 kHz
constexpr int kBufSamples = 1 << 15;            // about 0.45 s
constexpr int kBurstLastSym = 235;              // the 470 bits
constexpr int kTrainLastSym = 126;              // the training ends at bit 252
constexpr int kAfterTrain = (kBurstLastSym - kTrainLastSym) * kSps + 8;
constexpr double kDsbThresh = 0.80, kDnbThresh = 0.72;
constexpr int kSyncHold = (int)(kWork * 1.5);    // the slot clock is kept this long after the last burst heard
constexpr double kLockSec = 5.0;
constexpr int kSlotS = kSlotSamples;            // 1020 samples
constexpr float kBurstMinLevel = 1e-9f;

inline double phaseOfDibit(int a, int b) { return dibitPhase(a, b) * M_PI / 4.0; }

struct Corr {
    std::vector<cf32> c;                        // the conjugates of the expected differential phases
    void init(const uint8_t* bits, int n) {
        c.resize((size_t)n / 2);
        for (int i = 0; i + 1 < n; i += 2) { const double p = phaseOfDibit(bits[i], bits[i + 1]); c[(size_t)i / 2] = cf32((float)std::cos(p), (float)-std::sin(p)); }
    }
};
}

struct TetraDmoReceiver::Impl {
    std::mutex mu;
    TetraDmoTelemetry tel;
    std::function<void(const std::string&)> log;
    SpeechFrameSink* sink = nullptr;
    double fs = 0, offHz = 0;
    bool rateSet = false;
    ExactResampler rs;
    std::vector<cf32> mix, rsb;
    double phRe = 1, phIm = 0, stepW = 0, stRe = 1, stIm = 0;
    std::vector<float> rrc;
    std::vector<cf32> hist;
    size_t hpos = 0;
    std::vector<cf32> y, d;                     // 72 kHz filtered, differential; ring buffers
    uint64_t n = 0;
    Corr cy, cn, cp;
    double lastDsbSec = -1e9;
    double lastPub = 0;
    double power = 0, burstPower = 0;

    // ---- slot clock
    bool timed = false;
    uint64_t nextTrain = 0;                     // the sample of the last training symbol of the next slot
    int slot = 1, frame = 1;
    int misses = 0;
    double slotEpsilon = 0;                     // fractional tracking, samples
    // current call
    bool dccValid = false;
    uint32_t dcc = 0;
    int commType = 0, aiState = 0;
    int masterSlot = 1;
    bool setupKnown = false, e2e = false, clearKnown = false;
    uint32_t curSrc = 0, curDest = 0, curMni = 0;
    bool curGroup = true;
    int callIdx = -1;

    double timeSec() const { return (double)n / kWork; }

    void clearState() {
        n = 0; hpos = 0; std::fill(hist.begin(), hist.end(), cf32(0, 0));
        std::fill(y.begin(), y.end(), cf32(0, 0)); std::fill(d.begin(), d.end(), cf32(0, 0));
        rs.reset(); phRe = 1; phIm = 0; timed = false; misses = 0; dccValid = false; setupKnown = false; clearKnown = false; e2e = false; callIdx = -1; lastDsbSec = -1e9; lastPub = 0; power = 0;
        const uint64_t s = tel.seq;
        tel = TetraDmoTelemetry();
        tel.seq = s + 1; tel.inputRate = fs; tel.state = rateSet ? 1 : 0;
    }

    void addRow(const std::string& burst, const DmMessage& m, int fr, int sl, bool enc, const std::string& text) {
        TetraDmoRow r;
        r.wallTime = (int64_t)std::time(nullptr); r.timeSec = timeSec(); r.frame = fr; r.slot = sl; r.msgType = m.messageType; r.dest = m.dest; r.src = m.src; r.mni = m.mni; r.encrypted = enc;
        r.burst = burst; r.text = text;
        if (tel.rows.size() >= kTetraDmoRowsMax) tel.rows.pop_back();
        tel.rows.insert(tel.rows.begin(), r);
    }

    TetraDmoCall* activeCall() { return callIdx >= 0 && (size_t)callIdx < tel.calls.size() ? &tel.calls[(size_t)callIdx] : nullptr; }

    void startCall(const DmMessage& m) {
        TetraDmoCall* c = activeCall();
        if (c && c->active && c->src == m.src && c->dest == m.dest && c->mni == m.mni) {
            c->durSec = timeSec() - c->startSec;
            c->encrypted = aiState != 0 || (m.setupKnown && m.e2eEncrypted); c->e2e = m.setupKnown && m.e2eEncrypted; c->group = m.setupKnown ? m.groupCall : c->group;
            return;
        }
        endCall();
        TetraDmoCall x;
        x.src = m.src; x.dest = m.dest; x.mni = m.mni; x.commType = commType; x.circuitMode = m.circuitMode; x.priority = m.priority;
        x.group = m.setupKnown ? m.groupCall : true;
        x.encrypted = aiState != 0 || (m.setupKnown && m.e2eEncrypted); x.e2e = m.setupKnown && m.e2eEncrypted;
        x.active = true; x.wallStart = (int64_t)std::time(nullptr); x.startSec = timeSec();
        tel.calls.insert(tel.calls.begin(), x);
        if (tel.calls.size() > kTetraDmoCallsMax) tel.calls.pop_back();
        callIdx = 0;
    }
    void endCall() {
        if (TetraDmoCall* c = activeCall()) { c->active = false; c->durSec = timeSec() - c->startSec; }
        callIdx = -1; dccValid = false; setupKnown = false; clearKnown = false; e2e = false;
    }

    void handleMessage(const char* burst, const DmMessage& m, int fr, int sl) {
        const int t = m.messageType;
        std::string txt = describeMessage(m);
        if (m.setupKnown) {
            char b[120];
            snprintf(b, sizeof b, ", %s, circuit %d, priority %d%s%s", m.groupCall ? "group" : "individual", m.circuitMode, m.priority, m.e2eEncrypted ? ", end-to-end encrypted" : "", m.twoFrequency ? ", two-frequency" : "");
            txt += b;
        }
        addRow(burst, m, fr, sl, aiState != 0 || (m.setupKnown && m.e2eEncrypted), txt);
        tel.lastSrc = m.src; tel.lastDest = m.dest; tel.lastMni = m.mni;
        const bool setup = t == (int)Dm::Setup || t == (int)Dm::SetupPres;
        if (setup || t == (int)Dm::ConnectAck || t == (int)Dm::Occupied) {
            dcc = dccInit(m.mni, m.src); dccValid = true;
            curSrc = m.src; curDest = m.dest; curMni = m.mni;
            if (setup) { setupKnown = true; e2e = m.e2eEncrypted; curGroup = m.groupCall; clearKnown = aiState == 0 && !m.e2eEncrypted; }
            else if (!setupKnown) clearKnown = false;         // an occupied message says the channel is in use, but not how: the speech stays undecoded until a set-up is heard
            startCall(m);
        } else if (t == (int)Dm::Release || t == (int)Dm::TxCeased) {
            endCall();
        }
    }

    // ---- bursts
    cf32 dAt(uint64_t i) const { return d[i % d.size()]; }
    cf32 yAt(uint64_t i) const { return y[i % y.size()]; }
    void llrBits(uint64_t lastTrainSample, double rot, int sym0, int count, float* out) {
        const cf32 de(std::cos(rot), -std::sin(rot));
        // the symbol energy for the scale
        for (int k = 0; k < count; k++) {
            const uint64_t i = lastTrainSample + (uint64_t)(int64_t)((sym0 + k - kTrainLastSym) * kSps);
            const cf32 v = dAt(i) * de;
            out[2 * k] = -v.imag();        // bit 1: the negative phase steps
            out[2 * k + 1] = -v.real();    // bit 2: the steps beyond +-pi/2
        }
    }
    void scaleLlr(float* x, int n2) {
        double e = 0;
        for (int i = 0; i < n2; i++) e += (double)x[i] * x[i];
        const float g = e > 0 ? (float)(2.0 / std::sqrt(e / n2)) : 1.f;
        for (int i = 0; i < n2; i++) x[i] *= g;
    }

    double corrAt(const Corr& c, uint64_t last, double* phase) const {
        cf32 sum(0, 0); double e = 0;
        const int m = (int)c.c.size();
        for (int k = 0; k < m; k++) {
            const cf32 v = dAt(last - (uint64_t)((m - 1 - k) * kSps));
            sum += v * c.c[(size_t)k];
            e += (double)std::abs(v);
        }
        if (phase) *phase = std::arg(sum);
        return e > 0 ? (double)std::abs(sum) / e : 0;
    }

    double bodyPower(uint64_t trainLast) const {
        double e = 0;
        for (int k = 0; k < 18; k++) e += (double)std::norm(yAt(trainLast - (uint64_t)(k * kSps)));
        return e / 18;
    }
    void decodeDsb(uint64_t trainLast, double rot) {
        float sb[120], bk[216];
        llrBits(trainLast, rot, kSbAt / 2 + 1, 60, sb);
        llrBits(trainLast, rot, kBkn2At / 2 + 1, 108, bk);
        scaleLlr(sb, 120); scaleLlr(bk, 216);
        Bits s60, h124;
        const uint32_t z = zeroInit();
        if (!decodeChannel(sb, kBsch, z, s60)) { tel.blocksBad++; return; }
        burstPower = bodyPower(trainLast);
        SyncS ss;
        if (!parseSyncS(s60, ss)) { tel.blocksBad++; return; }
        // the slot clock from this burst
        tel.dsb++; tel.blocksOk++; tel.dataValid = true; lastDsbSec = timeSec();
        commType = ss.commType; tel.commType = ss.commType; aiState = ss.aiEncState; tel.encState = ss.aiEncState; tel.slot = ss.slot; tel.frame = ss.frame;
        timed = true; misses = 0;
        nextTrain = trainLast + (uint64_t)kSlotS; slot = ss.slot + 1; frame = ss.frame;
        if (slot > 4) { slot = 1; frame = ss.frame >= 18 ? 1 : ss.frame + 1; }
        DmMessage m;
        if (decodeChannel(bk, kSchHd, z, h124) && parseSyncH(h124, ss.commType, m)) {
            tel.schF++;
            handleMessage("DSB", m, ss.frame, ss.slot);
        } else tel.blocksBad++;
    }

    void decodeDnb(uint64_t trainLast, double rot, bool two, int fr, int sl) {
        float b1[216], b2[216];
        llrBits(trainLast, rot, kBkn1At / 2 + 1, 108, b1);
        llrBits(trainLast, rot, kBkn2At / 2 + 1, 108, b2);
        tel.dnb++;
        if (!dccValid) { tel.blocksBad++; return; }
        scaleLlr(b1, 216); scaleLlr(b2, 216);
        if (!two) {
            // SCH/F (CRC) or a whole-slot traffic burst
            float f[432];
            std::copy(b1, b1 + 216, f); std::copy(b2, b2 + 216, f + 216);
            Bits t;
            if (decodeChannel(f, kSchF, dcc, t)) { tel.schF++; tel.blocksOk++; dmacData(t, fr, sl); return; }
            traffic(b1, b2);
        } else {
            Bits t;
            if (decodeChannel(b1, kSchHd, dcc, t)) { tel.schF++; tel.blocksOk++; dmacData(t, fr, sl); }
            // the second half is a TCH half-slot or another STCH
            if (decodeChannel(b2, kSchHd, dcc, t)) { tel.schF++; tel.blocksOk++; dmacData(t, fr, sl); }
            else traffic(b1, b2);
        }
    }

    void dmacData(const Bits& t, int fr, int sl) {
        DmacData d2;
        if (!parseDmacData(t, commType, d2) || d2.nullPdu) return;
        if (d2.msg.messageType == (int)Dm::Setup || d2.msg.messageType == (int)Dm::SetupPres) return;   // a set-up is read on a DSB, where its dependent elements are decoded
        handleMessage("DNB", d2.msg, fr, sl);
    }

    void traffic(const float* b1, const float* b2) {
        CallEncryption e;
        e.known = setupKnown && clearKnown;
        e.encMode = aiState != 0 || e2e ? 1 : 0;
        const VoiceVerdict v = processTrafficBurst(b1, b2, dcc, e, sink);
        tel.traffic++;
        if (v != VoiceVerdict::Passed) tel.trafficRefused++;
        if (TetraDmoCall* c = activeCall()) { if (v == VoiceVerdict::Passed) { c->voice = true; c->voiceBursts++; } }
    }

    void slotTick() {
        // the slot at nextTrain is complete; search +-2 samples of the training for a DNB and take the DSB case through the correlator hunt below
        double bestM = 0, bestRot = 0; bool two = false; uint64_t best = 0;
        for (int di = -2; di <= 2; di++) {
            const uint64_t t = nextTrain + (uint64_t)(int64_t)di;
            double ph1 = 0, ph2 = 0;
            const double a = corrAt(cn, t, &ph1), b = corrAt(cp, t, &ph2);
            if (a > bestM) { bestM = a; bestRot = ph1; two = false; best = t; }
            if (b > bestM) { bestM = b; bestRot = ph2; two = true; best = t; }
        }
        if (bestM >= kDnbThresh && bodyPower(best) > 0.3 * burstPower) { misses = 0; decodeDnb(best, bestRot, two, frame, slot); nextTrain = best + (uint64_t)kSlotS; }
        else { nextTrain += (uint64_t)kSlotS; if (++misses > kSyncHold / kSlotS) { timed = false; endCall(); } }
        if (++slot > 4) { slot = 1; if (++frame > 18) frame = 1; }
    }

    void sampleOut(cf32 v) {
        // RRC
        hist[hpos] = v; hpos = (hpos + 1) % hist.size();
        cf32 s(0, 0);
        const size_t L = rrc.size();
        for (size_t i = 0; i < L; i++) s += hist[(hpos + i) % L] * rrc[i];
        y[n % y.size()] = s;
        const cf32 dv = s * std::conj(y[(n + y.size() - kSps) % y.size()]);
        d[n % d.size()] = dv;
        power += (std::norm(s) - power) * 0.002;
        // the slot clock first
        if (timed && n + 1 >= nextTrain + kAfterTrain + 4) { n++; slotTick(); return; }
        // the DSB hunt: at every sample, on the 19 symbols of y, and take the maximum of the next few samples
        if (n >= 8192 && power > kBurstMinLevel) {
            const uint64_t t = n;
            double ph = 0;
            const double m = corrAt(cy, t, &ph);
            if (m > kDsbThresh) {
                if (!huntActive || m > huntBest) { huntBest = m; huntAt = t; huntPh = ph; }
                if (!huntActive) { huntActive = true; huntEnd = t + 8; }
            }
        }
        if (huntActive && n >= huntEnd && n >= huntAt + kAfterTrain + 4) {
            huntActive = false;
            if (huntAt + (uint64_t)kBurstLastSym * kSps > kBurstLastSym) decodeDsb(huntAt, huntPh);
        }
        n++;
    }
    bool huntActive = false;
    double huntBest = 0, huntPh = 0;
    uint64_t huntAt = 0, huntEnd = 0;

    void publish() {
        tel.timeSec = timeSec();
        tel.levelDb = power > 0 ? (float)(10 * std::log10(power + 1e-30)) : -200.f;
        tel.locked = timeSec() - lastDsbSec < kLockSec;
        tel.state = rateSet ? (tel.locked ? 2 : 1) : 0;
        tel.seq++;
        if (TetraDmoCall* c = activeCall()) c->durSec = timeSec() - c->startSec;
    }
};

TetraDmoReceiver::TetraDmoReceiver() : p_(std::make_unique<Impl>()) {
    p_->cy.init(kTrainY, 38); p_->cn.init(kTrainN, 22); p_->cp.init(kTrainP, 22);
    p_->rrc = rrcTaps(kSps, 6);
    p_->hist.assign(p_->rrc.size(), cf32(0, 0));
    p_->y.assign((size_t)kBufSamples, cf32(0, 0)); p_->d.assign((size_t)kBufSamples, cf32(0, 0));
}
TetraDmoReceiver::~TetraDmoReceiver() = default;

void TetraDmoReceiver::configure(double r) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->fs = r;
    p_->rateSet = r >= tetraDmoTuning().minSampleRate - 1 && p_->rs.configure(r, kWork, 8);
    p_->clearState();
}
void TetraDmoReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offHz = hz; }
bool TetraDmoReceiver::ready() const { return p_->rateSet; }
void TetraDmoReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); p_->clearState(); }
void TetraDmoReceiver::clearMessages() { std::lock_guard<std::mutex> lk(p_->mu); p_->tel.rows.clear(); p_->tel.calls.clear(); p_->callIdx = -1; p_->tel.seq++; }
void TetraDmoReceiver::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rateSet) { p_->tel.state = 0; return; }
    Impl& m = *p_;
    const double step = -2 * M_PI * m.offHz / m.fs;
    if (step != m.stepW) { m.stepW = step; m.stRe = std::cos(step); m.stIm = std::sin(step); }
    m.mix.resize(n);
    double pr = m.phRe, pi = m.phIm;
    for (size_t i = 0; i < n; i++) {
        const float lr = (float)pr, li = (float)pi, xr = x[i].real(), xi = x[i].imag();
        m.mix[i] = cf32(xr * lr - xi * li, xr * li + xi * lr);
        const double nr = pr * m.stRe - pi * m.stIm;
        pi = pr * m.stIm + pi * m.stRe; pr = nr;
        if ((i & 1023) == 1023) { const double g = 1.0 / std::sqrt(pr * pr + pi * pi); pr *= g; pi *= g; }
    }
    const double g = 1.0 / std::sqrt(pr * pr + pi * pi); m.phRe = pr * g; m.phIm = pi * g;
    m.rsb.clear();
    m.rs.process(m.mix.data(), n, m.rsb);
    for (const cf32& v : m.rsb) m.sampleOut(v);
    if (m.timeSec() - m.lastPub >= 0.25) { m.lastPub = m.timeSec(); m.publish(); }
}
bool TetraDmoReceiver::telemetry(TetraDmoTelemetry& out, uint64_t lastSeq) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->tel.seq <= lastSeq) return false; out = p_->tel; return true; }
void TetraDmoReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void TetraDmoReceiver::setVoiceSink(tetra::SpeechFrameSink* s) { std::lock_guard<std::mutex> lk(p_->mu); p_->sink = s; }

} // namespace dect2
