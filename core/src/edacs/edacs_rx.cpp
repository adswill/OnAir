// EDACS receiver: FM front end at 76.8 kHz (8 samples per bit), dotting search at every phase, then the 240 payload bits are sliced for
// the alignments the dotting leaves open (both polarities) and the one that matches the sync word 0x555557125555 and passes the BCH check is voted.
// On top: the message layer (edacs_msg.h) with the call and talkgroup tables, a power scan of the voice channels of a frequency table (one FFT
// of the capture), and the following of one analog voice channel through a second FM front end.
#include "dect2/edacs/edacs_rx.h"
#include "dect2/audio/audioout.h"
#include "dect2/codec/codec_registry.h"
#include "dect2/edacs/provoice.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/fm_front.h"
#include "dect2/edacs/edacs_msg.h"
#include "dect2/edacs/edacs_proto.h"
#include "dect2/edacs/edacs_tel.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>

namespace dect2 {

ModeTuning edacsTuning() {
    ModeTuning t;
    t.stdMode = 132;
    t.id = "edacs";
    t.name = "EDACS trunking";
    t.minMhz = 29.7;
    t.maxMhz = 960.0;
    t.defMhz = 856.0;
    t.sampleRate = 480000;
    t.basebandHz = 300000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 307200;
    t.tuneOffsetHz = 100000;
    return t;
}

namespace {
constexpr double kWork = 76800.0;
constexpr int kSps = 8;
constexpr int kRing = 4096, kMask = kRing - 1;
constexpr int kPayloadBits = 240;
constexpr int kMaxDisagree = 4;           // per message, of 40 bits
constexpr double kMinRangeHz = 400.0;
constexpr double kLockSec = 5.0;
constexpr double kVoiceRate = 48000.0;
constexpr double kCallHoldSec = 4.0;       // an announced call with no repeat and no carrier on its channel ends after this
constexpr double kActivityHoldSec = 1.5;   // a carrier-only call ends this long after the carrier went
constexpr int kPvRing = 16384, kPvMask = kPvRing - 1;   // ProVoice block buffer: 768 bits at 8 samples
constexpr double kChanHalfHz = 6250.0;     // half the width of a channel in the power scan
int pop(uint64_t x) { return __builtin_popcountll(x); }
}

struct EdacsReceiver::Impl {
    std::mutex mu;
    EdacsTelemetry tel;
    std::function<void(const std::string&)> log;
    FmFront fm;
    FmBiquad post;
    EyeEnvelope env;
    std::vector<float> disc;
    double fs = 0, offHz = 0, centreHz = 0;
    bool rateSet = false, inverted = false;
    uint64_t n = 0;
    float ring[kRing] = {};
    uint64_t reg[kSps] = {};               // last 48 bits per phase
    std::vector<uint64_t> hits;            // sample counts (n after increment) of dotting hits
    uint64_t firstHit = 0, evalAt = 0, centre = 0;
    bool pending = false, haveStart = false, predictedMode = false;
    uint64_t lastStart = 0;
    double lastGoodSec = -1e9, lastPub = 0;
    std::map<uint32_t, uint64_t> wordCount;

    // ---- message layer, voice channels
    edacs::Layout layout;
    std::vector<double> lcnFreq;                  // Hz, index lcn - 1
    bool follow = false, followAct = false;
    std::vector<int> followGroups;
    int followLcn = 0;
    std::unique_ptr<Fft> fft;
    std::vector<cf32> blk, fftBuf;
    std::vector<float> win;
    double winPow = 1;
    size_t blkN = 0;
    std::vector<double> chPow, chFloor;           // linear power per channel (smoothed), and its floor
    std::vector<char> chOn, floorInit;
    std::vector<int> chRun;                       // consecutive blocks above the threshold (a key click is not a carrier)
    std::vector<double> chLastOn;                 // signal time the channel was last above the threshold
    FmFront voice;
    FmBiquad vlp;
    bool voiceOk = false;
    double vdc = 0;
    std::vector<float> vdisc, aud;
    std::unique_ptr<AudioOut> audioOut;
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    std::function<void(int, const float*, size_t)> tap;
    // ---- ProVoice on the followed channel: the same FM front as the control channel
    FmFront pv;
    FmBiquad pvLp;
    EyeEnvelope pvEnv;
    std::vector<float> pvDisc;
    float pvRing[kPvRing] = {};
    uint64_t pvN = 0;
    uint32_t pvReg[kSps] = {};
    bool pvLocked = false, pvInv = false;
    uint64_t pvAt = 0;                        // the sample of the last sync bit of the block being read
    int pvMiss = 0;
    bool followDig = false;
    std::unique_ptr<codec::SpeechCodec> pvCodec;
    uint64_t pvBlocks = 0, pvFrames = 0, pvBad = 0;
    int pendingLid = -1;
    double pendingLidT = -1e9;

    void initActivity() {
        int N = 256;
        while (N < 8192 && (double)N < fs / 500.0) N <<= 1;
        fft = std::make_unique<Fft>(N);
        blk.assign((size_t)N, cf32(0, 0)); fftBuf.assign((size_t)N, cf32(0, 0)); blkN = 0;
        win.resize((size_t)N);
        winPow = 0;
        for (int i = 0; i < N; i++) { win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * (i + 0.5) / N)); winPow += (double)win[(size_t)i] * win[(size_t)i]; }
        resizeChannels();
    }
    void resizeChannels() {
        chPow.assign(lcnFreq.size(), 0.0); chFloor.assign(lcnFreq.size(), 0.0); chOn.assign(lcnFreq.size(), 0); floorInit.assign(lcnFreq.size(), 0); chRun.assign(lcnFreq.size(), 0); chLastOn.assign(lcnFreq.size(), -1e9);
        tel.lcnTableSize = lcnFreq.size();
        tel.channels.clear();
        for (size_t i = 0; i < lcnFreq.size(); i++) {
            EdacsChannelActivity c;
            c.lcn = (int)i + 1; c.freqHz = lcnFreq[i]; c.inCapture = inCapture(lcnFreq[i]);
            tel.channels.push_back(c);
        }
    }
    bool inCapture(double f) const { return centreHz > 0 && fs > 0 && std::fabs(f - centreHz) < 0.42 * fs; }

    void activityBlock() {
        if (lcnFreq.empty() || !fft) return;
        const size_t N = blk.size();
        for (size_t i = 0; i < N; i++) fftBuf[i] = blk[i] * win[i];
        fft->forward(fftBuf.data());
        const double binHz = fs / (double)N;
        const int kw = std::max(1, (int)std::lround(kChanHalfHz / binHz));
        const double blockSec = (double)N / fs;
        for (size_t c = 0; c < lcnFreq.size(); c++) {
            if (!inCapture(lcnFreq[c])) { chOn[c] = 0; continue; }
            const double off = lcnFreq[c] - centreHz;
            const int k0 = (int)std::lround(off / binHz);
            double sum = 0;
            for (int k = -kw; k <= kw; k++) {
                int b = (k0 + k) % (int)N;
                if (b < 0) b += (int)N;
                sum += std::norm(fftBuf[(size_t)b]);
            }
            const double p = sum / ((double)N * winPow);
            chPow[c] = floorInit[c] ? 0.5 * chPow[c] + 0.5 * p : p;
            if (!floorInit[c]) { chFloor[c] = p; floorInit[c] = 1; }
            if (chPow[c] < chFloor[c]) chFloor[c] = chPow[c];
            else if (!chOn[c]) chFloor[c] *= std::pow(10.0, 0.1 * blockSec);   // a steady carrier is absorbed into the floor within ten seconds or so
            const bool strong = chPow[c] > chFloor[c] * 6.3 && chPow[c] > 1e-9;   // 8 dB above the floor
            const bool weak = chPow[c] < chFloor[c] * 2.5;                         // back to 4 dB: the carrier is gone
            if (strong) { if (++chRun[c] * blockSec >= 0.06) chOn[c] = 1; }
            else { chRun[c] = 0; if (weak) chOn[c] = 0; }
            if (chOn[c]) chLastOn[c] = timeSec();
        }
    }
    bool chActive(int lcn) const { return lcn >= 1 && (size_t)lcn <= chOn.size() && chOn[(size_t)lcn - 1]; }

    // ---- calls
    void touchTg(int gid, int lid, int lcn, double freq) {
        auto it = std::find_if(tel.talkgroups.begin(), tel.talkgroups.end(), [&](const EdacsTalkgroup& t) { return t.gid == gid; });
        EdacsTalkgroup t;
        if (it != tel.talkgroups.end()) { t = *it; tel.talkgroups.erase(it); }
        t.gid = gid; t.calls++;
        if (lid >= 0) t.lastLid = lid;
        t.lastLcn = lcn; if (freq > 0) t.lastFreqHz = freq;
        t.lastWall = (int64_t)std::time(nullptr); t.active = true;
        tel.talkgroups.insert(tel.talkgroups.begin(), t);
        if (tel.talkgroups.size() > kEdacsTgMax) tel.talkgroups.pop_back();
    }
    EdacsCall* activeCall(int lcn) {
        for (auto& c : tel.calls) if (c.active && c.lcn == lcn) return &c;
        return nullptr;
    }
    void endCall(EdacsCall& c) {
        c.active = false; c.durSec = c.lastSec - c.startSec;
        c.followed = false;
        if (log) { char b[120]; snprintf(b, sizeof b, "EDACS call ended: LCN %d, %s %d, %.1f s", c.lcn, c.group ? "group" : "unit", c.gid, c.durSec); log(b); }
    }
    void newCall(int lcn, int gid, int lid, bool group, bool emerg, bool dig, bool fromAct, double t) {
        EdacsCall c;
        c.lcn = lcn; c.gid = gid; c.lid = lid; c.group = group; c.emergency = emerg; c.digital = dig; c.fromActivity = fromAct; c.active = true;
        c.wallTime = (int64_t)std::time(nullptr); c.startSec = c.lastSec = t;
        c.freqHz = (lcn >= 1 && (size_t)lcn <= lcnFreq.size()) ? lcnFreq[(size_t)lcn - 1] : 0;
        tel.calls.insert(tel.calls.begin(), c);
        if (tel.calls.size() > kEdacsCallMax) tel.calls.pop_back();
        if (gid >= 0 && group) touchTg(gid, lid, lcn, c.freqHz);
        if (log) { char b[140]; snprintf(b, sizeof b, "EDACS call: LCN %d%s, %s %d", lcn, fromAct ? " (carrier only)" : "", group ? "group" : "unit", gid); log(b); }
    }
    void assignment(const edacs::Msg& m, double t) {
        const bool group = m.type == edacs::MsgType::GroupAssign;
        EdacsCall* c = activeCall(m.lcn);
        if (c && (c->fromActivity || (c->gid == m.gid && c->group == group))) {
            if (c->fromActivity) { c->fromActivity = false; c->gid = m.gid; c->group = group; if (group) touchTg(m.gid, c->lid, m.lcn, c->freqHz); }
            c->lastSec = t; c->emergency = c->emergency || m.emergency; c->digital = m.digital;
            return;
        }
        if (c) endCall(*c);
        newCall(m.lcn, m.gid, m.lid >= 0 ? m.lid : m.source, group, m.emergency, m.digital, false, t);
    }
    void handleFrame(uint32_t w1, uint32_t w2, double t) {
        const edacs::Msg m = edacs::parse(w1, w2, layout);
        tel.messages++;
        if (m.type != edacs::MsgType::Unknown) tel.messagesKnown++;
        if (m.lcn >= 1 && m.lcn <= 25 && m.lcn > tel.nChannels) tel.nChannels = m.lcn;   // the highest LCN seen so far
        switch (m.type) {
        case edacs::MsgType::SiteId:
            tel.siteValid = true; tel.siteId = m.site; tel.systemId = m.area;
            if (m.lcn >= 1 && m.lcn <= 25) tel.controlLcn = m.lcn;
            break;
        case edacs::MsgType::GroupAssign: case edacs::MsgType::IndividualAssign: assignment(m, t); break;
        case edacs::MsgType::CallEnd: { EdacsCall* c = activeCall(m.lcn); if (c && !c->fromActivity) { c->lastSec = t; endCall(*c); } break; }
        default: break;
        }
    }
    std::string frameText(uint32_t a, uint32_t b) {
        return edacs::describe(edacs::parse(a, b, layout));
    }

    // ---- housekeeping (every report): channel activity into calls, call timeouts, the choice of the followed channel
    void housekeeping() {
        const double t = timeSec();
        tel.note.clear();
        for (size_t c = 0; c < lcnFreq.size() && c < tel.channels.size(); c++) {
            EdacsChannelActivity& ch = tel.channels[c];
            ch.levelDb = chPow[c] > 0 ? (float)(10 * std::log10(chPow[c] + 1e-20)) : -200.f;
            ch.floorDb = chFloor[c] > 0 ? (float)(10 * std::log10(chFloor[c] + 1e-20)) : -200.f;
            ch.active = chOn[c] != 0;
            ch.inCapture = inCapture(lcnFreq[c]);
            const int lcn = (int)c + 1;
            if (ch.active && !activeCall(lcn) && lcn != tel.controlLcn) newCall(lcn, -1, -1, true, false, false, true, t);
        }
        for (auto& c : tel.calls) {
            if (!c.active) continue;
            const bool on = chActive(c.lcn);
            if (on) c.lastSec = std::max(c.lastSec, t);
            c.durSec = t - c.startSec;
            const double hold = (c.fromActivity || (c.freqHz > 0 && inCapture(c.freqHz))) ? kActivityHoldSec : kCallHoldSec;   // a channel we can see: its carrier says when the call is over
            if (!on && t - c.lastSec > hold) endCall(c);
        }
        for (auto& g : tel.talkgroups) g.active = false;
        for (auto& c : tel.calls) if (c.active && c.group && c.gid >= 0) for (auto& g : tel.talkgroups) if (g.gid == c.gid) g.active = true;
        // the followed channel
        if (followLcn) {
            EdacsCall* c = activeCall(followLcn);
            const bool carrier = chActive(followLcn) || t - chLastOn[(size_t)followLcn - 1] < 0.4 || (c && t - c->startSec < 1.0);   // the call is over when its carrier is
            if (!follow || !c || !carrier) { flushAudio(); followLcn = 0; followDig = false; voice.reset(); }
        }
        if (follow && !followLcn) {
            if (lcnFreq.empty()) tel.note = "no frequency table: enter the voice channel frequencies (LCN 1, 2, 3 ...) to follow calls";
            else if (centreHz <= 0) tel.note = "the frequency of the capture is not known (file input): the voice channels cannot be placed";
            for (int pass = 0; pass < 2 && !followLcn; pass++)   // first the calls with a carrier on the air, then the ones just announced
                for (auto& c : tel.calls) {   // newest first
                    if (!c.active || c.freqHz <= 0) continue;
                    if (c.digital && !codec::pluginLoaded("imbe")) { if (tel.note.empty()) tel.note = "ProVoice call: install the IMBE plug-in (onair-codec-imbe) for its sound"; continue; }
                    if (c.fromActivity && !followAct) continue;
                    if (!followGroups.empty() && std::find(followGroups.begin(), followGroups.end(), c.gid) == followGroups.end()) continue;
                    if (!inCapture(c.freqHz)) { if (tel.note.empty()) tel.note = "LCN " + std::to_string(c.lcn) + " is outside the captured span: use a wider capture"; continue; }
                    if (pass == 0 ? !chActive(c.lcn) : t - c.startSec > 1.0) continue;
                    followLcn = c.lcn;
                    followDig = c.digital;
                    voice.reset(); vdc = 0; vlp.clear(); aud.clear();
                    voice.setOffset(c.freqHz - centreHz);
                    if (followDig) startProvoice(c.freqHz);
                    break;
                }
        }
        for (auto& c : tel.calls) c.followed = c.active && c.lcn == followLcn && followLcn != 0;
        tel.follow = follow; tel.followActivity = followAct; tel.followLcn = followLcn;
        tel.audioOpen = followLcn != 0 && chActive(followLcn) && !muted.load();
    }

    // ---- ProVoice: the 768 bit blocks of the followed digital channel. Sync on the 32 bit word (four variants: standard or Extended Addressing, both polarities), then one block
    // of four IMBE 7100x4400 frames each 80 ms. The frames are corrected and rewritten as 7200x4400 code words for the user-installed plug-in.
    void startProvoice(double hz) {
        pv.reset(); pv.setOffset(hz - centreHz); pvEnv.clear(); pvLp.clear(); pvN = 0; for (auto& r : pvRing) r = 0; for (auto& r : pvReg) r = 0;
        pvLocked = false; pvMiss = 0;
        pvCodec = codec::pluginVocoder("imbe");
    }
    double pvSoft(uint64_t i) const { return (pvRing[i & kPvMask] + pvRing[(i - 1) & kPvMask] + pvRing[(i - 2) & kPvMask]) / 3.0; }
    void pvDecodeBlock(uint64_t lastSync) {
        const double thr = pvEnv.mid();
        uint8_t bits[736];
        for (int i = 0; i < 736; i++) {
            const uint64_t idx = lastSync + (uint64_t)(i + 1) * kSps;
            bits[i] = (uint8_t)((pvSoft(idx) > thr ? 0 : 1) ^ (pvInv ? 1 : 0));
        }
        provoice::Frame7100 fr[4];
        uint16_t lid = 0;
        provoice::splitBlock(bits, fr, &lid);
        pvBlocks++;
        for (int f = 0; f < 4; f++) {
            uint8_t d[provoice::kInfoBits];
            int errs = 0;
            pvFrames++;
            if (!provoice::decode7100(fr[f], d, &errs)) { pvBad++; continue; }
            if (!pvCodec) continue;
            provoice::convert7100to7200(d);
            provoice::Frame7200 out;
            provoice::encode7200(d, out);
            const std::vector<int16_t> pcm = pvCodec->decode(codec::packCodeWords(&out.b[0][0], 8, 23));
            // the plug-in gives 8 kHz; the follower plays at 48 kHz: each sample six times
            for (int16_t v : pcm) for (int k = 0; k < 6; k++) aud.push_back(chActive(followLcn) ? (float)v / 32768.f : 0.f);
            if (aud.size() >= 960) flushAudio();
        }
    }
    void pvSample(double s) {
        pvRing[pvN & kPvMask] = (float)s;
        pvEnv.push(s);
        const int ph = (int)(pvN % kSps);
        const double thr = pvEnv.mid();
        pvReg[ph] = (pvReg[ph] << 1) | (pvSoft(pvN) > thr ? 0u : 1u);   // a positive step is payload bit 0 (dsd_dibit.c)
        pvN++;
        if (pvLocked) {
            if (pvN >= pvAt + 736 * kSps + 2) {
                pvDecodeBlock(pvAt);
                pvAt += (uint64_t)provoice::kBlockBits * kSps;
                // is the next sync where it should be? a few samples each way, so clock drift is followed; otherwise hunt again
                int best = 99; uint64_t bestAt = 0;
                for (int d = -3; d <= 3; d++) {
                    const uint64_t last = pvAt + (uint64_t)(int64_t)d;
                    uint32_t v = 0;
                    for (int i = 0; i < 32; i++) v = (v << 1) | (pvSoft(last - (uint64_t)(31 - i) * kSps) > thr ? 0u : 1u);
                    const uint32_t tmpl = (pvInv ? ~pvTmpl : pvTmpl);
                    const int e = __builtin_popcount(v ^ tmpl);
                    if (e < best) { best = e; bestAt = last; }
                }
                if (best <= 5) { pvAt = bestAt; pvMiss = 0; }
                else if (++pvMiss >= 3) pvLocked = false;
            }
            return;
        }
        if (pvN < 512 || pvEnv.range() < 800) return;
        const uint32_t r = pvReg[ph];
        for (uint32_t t : {provoice::kSyncStdValue, provoice::kSyncEaValue}) {
            const int e0 = __builtin_popcount(r ^ t), e1 = __builtin_popcount(r ^ ~t);
            if (e0 <= 2 || e1 <= 2) {
                pvInv = e1 < e0; pvTmpl = t; pvAt = pvN - 1; pvLocked = true; pvMiss = 0;
                return;
            }
        }
    }
    uint32_t pvTmpl = 0;

    // ---- the followed voice channel
    void voiceSamples() {
        const bool gate = chActive(followLcn);
        for (float d : vdisc) {
            const double a = d / 5000.0;
            vdc += 0.0005 * (a - vdc);
            const double v = vlp.run(a - vdc);
            aud.push_back(gate ? (float)(0.9 * v) : 0.f);
        }
        if (aud.size() >= 960) flushAudio();
    }
    void flushAudio() {
        if (aud.empty()) return;
        if (tap) tap(followLcn, aud.data(), aud.size());
        if (!silent.load()) {
            std::vector<float> st(aud.size() * 2);
            for (size_t i = 0; i < aud.size(); i++) st[2 * i] = st[2 * i + 1] = aud[i];
            if (!audioOut) { audioOut = std::make_unique<AudioOut>(); audioOut->start(48000); audioOut->setStartThreshold(48000 / 5); }
            audioOut->setVolume(volume.load()); audioOut->setMuted(muted.load());
            if (audioOut->bufferedFrames() > 48000 * 3 / 2) audioOut->flush();
            audioOut->write(st.data(), (int)aud.size());
        }
        aud.clear();
    }

    double timeSec() const { return (double)n / kWork; }

    void clearState() {
        fm.reset(); post.clear(); env.clear();
        n = 0; for (auto& r : ring) r = 0; for (auto& r : reg) r = 0;
        hits.clear(); pending = false; haveStart = false; firstHit = evalAt = centre = 0; lastGoodSec = -1e9; lastPub = 0; wordCount.clear();
        const uint64_t s = tel.seq;
        tel = EdacsTelemetry();
        tel.seq = s + 1; tel.inputRate = fs; tel.centerHz = centreHz; tel.state = rateSet ? 1 : 0; tel.inverted = inverted;
        followLcn = 0; pendingLid = -1; pendingLidT = -1e9; vdisc.clear(); aud.clear(); vdc = 0; voice.reset(); vlp.clear();
        if (fft) { blkN = 0; resizeChannels(); }
        tel.follow = follow; tel.followActivity = followAct;
    }

    double soft(uint64_t idx) const { return (ring[idx & kMask] + ring[(idx - 1) & kMask] + ring[(idx - 2) & kMask]) / 3.0; }

    void evaluate() {
        // centre: sample index (1-based count) of the median dotting hit; the last sync bit was sampled at centre - 1
        const double thr = env.mid();
        // Frames follow each other every 288 bits, so after a good frame the next one is looked for where the clock says (predicted). The dotting alone
        // cannot tell neighbouring bit alignments apart (the three copies agree under a shift) and 
        const bool predicted = predictedMode;
        struct Cand { int off, sub, pol, score, ea, eb; bool okA, okB; uint32_t a, b; };
        Cand c[64];
        const int order[6] = {0, -1, 1, 2, -2, 3};   // the dotting looks the same two bits later or earlier, so these alignments are tried
        const int nOrd = predicted ? 3 : 6;
        int nc = 0;
        // after a good frame the bit clock is followed to the sample (a sender / radio clock difference moves the frame start a little every frame)
        for (int oi = 0; oi < nOrd * (predicted ? 5 : 1); oi++) {
            const int off = order[oi % nOrd];
            const int sub = predicted ? std::array<int, 5>{0, -1, 1, -2, 2}[(size_t)(oi / nOrd)] : 0;
            const int64_t last = (int64_t)centre - 1 + (int64_t)off * kSps + sub;
            uint8_t raw[kPayloadBits];
            for (int i = 0; i < kPayloadBits; i++) raw[i] = soft((uint64_t)(last + (int64_t)(i + 1) * kSps)) > thr ? 1 : 0;
            uint8_t pre[edacs::kSyncBits];
            for (int j = 0; j < edacs::kSyncBits; j++) pre[edacs::kSyncBits - 1 - j] = soft((uint64_t)(last - (int64_t)j * kSps)) > thr ? 1 : 0;
            for (int pol = 0; pol < 2; pol++) {   // the polarity is found from the sync pattern and the check bits
                uint8_t bits[kPayloadBits];
                for (int i = 0; i < kPayloadBits; i++) bits[i] = (uint8_t)(raw[i] ^ pol);
                // the 48 sync bits: dotting at both ends and the 16-bit Barker field 0x5712 in the middle; count the bits that differ from the pattern
                int mism = 0;
                for (int j = 0; j < edacs::kSyncBits; j++) mism += ((pre[j] ^ pol) != (int)((edacs::kSyncPattern >> (edacs::kSyncBits - 1 - j)) & 1));
                int ea, eb;
                const uint64_t va = edacs::vote(bits, ea), vb = edacs::vote(bits + 120, eb);
                const bool okA = edacs::bchOk(va), okB = edacs::bchOk(vb);
                c[nc++] = {off, sub, pol, mism + ea + eb + (okA ? 0 : 12) + (okB ? 0 : 12), ea, eb, okA, okB, (uint32_t)(va >> 12) & 0xFFFFFFF, (uint32_t)(vb >> 12) & 0xFFFFFFF};
            }
        }
        int best = 0;
        for (int i = 1; i < nc; i++) if (c[i].score < c[best].score) best = i;
                const uint32_t bestA = c[best].a, bestB = c[best].b;
        const int bestEa = c[best].ea, bestEb = c[best].eb;
        const bool okA = c[best].okA, okB = c[best].okB;
        const uint64_t chosenStart = centre + (uint64_t)((int64_t)c[best].off * kSps + c[best].sub);
        tel.syncs++;
        if (env.range() < kMinRangeHz) return;
        const bool good = okA && bestEa <= kMaxDisagree;   // the first message is the command: its check bits must verify
        if (!good) { tel.blocksBad++; haveStart = false; return; }
        inverted = c[best].pol != 0; tel.inverted = inverted;
        lastStart = chosenStart; haveStart = true;
        EdacsFrame f;
        f.wallTime = (int64_t)std::time(nullptr);
        f.timeSec = timeSec();
        f.msgA = bestA; f.msgB = okB ? bestB : 0; f.errA = bestEa; f.errB = bestEb; f.good = true;
        f.text = frameText(bestA, okB ? bestB : 0);
        handleFrame(bestA, okB ? bestB : 0, f.timeSec);
        if (tel.frames.size() >= kEdacsLogMax) tel.frames.pop_back();
        tel.frames.insert(tel.frames.begin(), f);
        wordCount[bestA]++; if (okB) wordCount[bestB]++;
        std::vector<EdacsWordCount> w;
        for (auto& kv : wordCount) w.push_back({kv.first, kv.second});
        std::sort(w.begin(), w.end(), [](const EdacsWordCount& a, const EdacsWordCount& b) { return a.count > b.count; });
        if (w.size() > 24) w.resize(24);
        tel.words = std::move(w);
        tel.blocksOk++;
        tel.dataValid = true;
        lastGoodSec = f.timeSec;
        if (log) { char b[80]; snprintf(b, sizeof b, "EDACS frame %07X %07X", bestA, bestB); log(b); }
    }

    static int alternationMismatch(uint64_t a16) { return 15 - pop((a16 ^ (a16 >> 1)) & 0x7FFF); }

    void sample(double s) {
        ring[n & kMask] = (float)s;
        env.push(s);
        const double thr = env.mid();
        const int ph = (int)(n % kSps);
        reg[ph] = (reg[ph] << 1) | (soft(n) > thr ? 1ULL : 0ULL);
        n++;
        if (pending) {
            if (n >= evalAt) {
                pending = false;
                evaluate();
                hits.clear();
                if (haveStart) {   // the next frame follows 288 bits after this one
                    centre = lastStart + (uint64_t)edacs::kFrameBits * kSps;
                    evalAt = centre + (uint64_t)(kPayloadBits + 2) * kSps;
                    predictedMode = true;
                    pending = evalAt > n;
                } else predictedMode = false;
            }
            return;
        }
        if (n > 1000 && env.range() >= kMinRangeHz) {
            const uint64_t r = reg[ph];
            if (alternationMismatch((r >> 32) & 0xFFFF) + alternationMismatch(r & 0xFFFF) <= 1) {
                if (hits.empty()) firstHit = n;
                hits.push_back(n);
            }
        }
        if (!hits.empty() && n >= firstHit + kSps / 2) {
            centre = hits[hits.size() / 2];
            hits.clear();
            pending = true;
            predictedMode = false;
            evalAt = centre + (uint64_t)(kPayloadBits + 5) * kSps;
        }
    }

    void publish() {
        tel.levelDb = (float)fm.levelDb();
        tel.deviationHz = (float)env.range();
        tel.timeSec = timeSec();
        tel.locked = timeSec() - lastGoodSec < kLockSec;
        tel.state = rateSet ? (tel.locked ? 2 : 1) : 0;
        housekeeping();
        tel.seq++;
    }
};

EdacsReceiver::EdacsReceiver() : p_(std::make_unique<Impl>()) {}
EdacsReceiver::~EdacsReceiver() = default;

void EdacsReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->fs = inputRateHz;
    p_->rateSet = inputRateHz >= edacsTuning().minSampleRate - 1 && p_->fm.configure(inputRateHz, kWork, 9000.0);
    p_->fm.setOffset(p_->offHz);
    p_->post.lowpass(6000.0, kWork, 0.7071);
    p_->voiceOk = p_->rateSet && p_->voice.configure(inputRateHz, kVoiceRate, 7500.0);
    p_->vlp.lowpass(3400.0, kVoiceRate, 0.7071);
    p_->pv.configure(inputRateHz, kWork, 9000.0);
    p_->pvLp.lowpass(6000.0, kWork, 0.7071);
    p_->initActivity();
    p_->clearState();
}
void EdacsReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offHz = hz; p_->fm.setOffset(hz); }
void EdacsReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->centreHz = hz; p_->tel.centerHz = hz; if (p_->fft) p_->resizeChannels(); }
void EdacsReceiver::setInverted(bool inv) { std::lock_guard<std::mutex> lk(p_->mu); p_->inverted = inv; p_->tel.inverted = inv; p_->tel.seq++; }
bool EdacsReceiver::ready() const { return p_->rateSet; }
void EdacsReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); p_->clearState(); }
void EdacsReceiver::clearMessages() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.frames.clear(); p_->tel.words.clear(); p_->wordCount.clear(); p_->tel.dataValid = false; p_->tel.calls.clear(); p_->tel.talkgroups.clear(); p_->tel.messages = p_->tel.messagesKnown = 0; p_->tel.seq++;
}
void EdacsReceiver::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rateSet) { p_->tel.state = 0; return; }
    p_->disc.clear();
    p_->fm.feed(x, n, p_->disc);
    for (float d : p_->disc) p_->sample(p_->post.run(d));
    if (!p_->lcnFreq.empty() && p_->fft) {   // the power scan of the voice channels
        const size_t N = p_->blk.size();
        for (size_t i = 0; i < n; i++) {
            p_->blk[p_->blkN++] = x[i];
            if (p_->blkN == N) { p_->activityBlock(); p_->blkN = 0; }
        }
    }
    if (p_->followLcn && p_->followDig && p_->pv.ready()) {
        p_->pvDisc.clear();
        p_->pv.feed(x, n, p_->pvDisc);
        for (float d : p_->pvDisc) p_->pvSample(p_->pvLp.run(d));
    } else if (p_->followLcn && p_->voiceOk) {
        p_->vdisc.clear();
        p_->voice.feed(x, n, p_->vdisc);
        p_->voiceSamples();
    }
    if (p_->timeSec() - p_->lastPub >= 0.25) { p_->lastPub = p_->timeSec(); p_->publish(); }
}
bool EdacsReceiver::telemetry(EdacsTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}
void EdacsReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void EdacsReceiver::setLcnTable(const std::vector<double>& f) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->lcnFreq = f;
    p_->followLcn = 0;
    if (p_->fft) p_->resizeChannels();
    p_->tel.seq++;
}
void EdacsReceiver::setLayout(const edacs::Layout& l) { std::lock_guard<std::mutex> lk(p_->mu); p_->layout = l; }
void EdacsReceiver::setFollow(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->follow = on; if (!on) p_->followLcn = 0; p_->tel.follow = on; p_->tel.seq++; }
void EdacsReceiver::setFollowActivity(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->followAct = on; p_->tel.followActivity = on; p_->tel.seq++; }
void EdacsReceiver::setFollowGroups(const std::vector<int>& g) { std::lock_guard<std::mutex> lk(p_->mu); p_->followGroups = g; }
void EdacsReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void EdacsReceiver::setMuted(bool m) { p_->muted = m; }
void EdacsReceiver::setSilent(bool s) { p_->silent = s; }
void EdacsReceiver::setAudioTap(std::function<void(int, const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->tap = std::move(cb); }

} // namespace dect2
