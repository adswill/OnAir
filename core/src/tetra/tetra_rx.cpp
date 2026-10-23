// TETRA network information receiver (tetra_rx.h): blocks of input, 25 kHz carriers found in the spectrum, slot synchronised on the
// training sequences, the broadcast channels decoded.
#include "dect2/common/thread_name.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/results/results_io.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/tetra/tetra_tel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>
#include "dect2/common/timecompat.h"

namespace dect2 {

ModeTuning tetraTuning() {
    ModeTuning t;
    t.stdMode = 48; t.id = "tetra"; t.name = "TETRA";
    t.minMhz = 136; t.maxMhz = 960; t.defMhz = 392.5;   // the European emergency services downlink is 390 - 395 MHz
    t.sampleRate = 2000000;      // 2 MHz of the band (80 channels) in one capture
    t.basebandHz = 1750000;
    t.bandwidthMhz = 1.8;
    t.minSampleRate = 200000;    // a few channels
    return t;
}

namespace tetra {

// ---------------------------------------------------------------- channel filter

std::vector<cf32> channelize(const cf32* x, size_t n, double fs, double shift) {
    std::vector<cf32> m(n);
    {
        const double w = -2 * M_PI * shift / fs;
        for (size_t i0 = 0; i0 < n; i0 += 4096) {
            std::complex<double> o(std::cos(w * (double)i0), std::sin(w * (double)i0));
            const std::complex<double> st(std::cos(w), std::sin(w));
            const size_t e = std::min(n, i0 + 4096);
            for (size_t i = i0; i < e; i++) { m[i] = x[i] * cf32((float)o.real(), (float)o.imag()); o *= st; }
        }
    }
    const int d1 = std::max(1, (int)std::floor(fs / 288e3));
    const double fs1 = fs / d1;
    std::vector<cf32> s1;
    if (d1 == 1) s1.swap(m);
    else {
        const int M = 8 * d1 + 1, H = M / 2;
        const double fc = 0.4 / d1;
        std::vector<float> h((size_t)M);
        double sum = 0;
        for (int i = 0; i < M; i++) {
            const double t = i - H, a = 2 * fc * t;
            const double s = std::fabs(a) < 1e-9 ? 1.0 : std::sin(M_PI * a) / (M_PI * a);
            const double u = (double)(i - H) / (H + 1);
            h[(size_t)i] = (float)(s * (0.42 + 0.5 * std::cos(M_PI * u) + 0.08 * std::cos(2 * M_PI * u)));
            sum += h[(size_t)i];
        }
        for (auto& v : h) v = (float)(v / sum);
        const size_t no = n / (size_t)d1;
        s1.resize(no);
        for (size_t o = 0; o < no; o++) {
            const long c = (long)(o * (size_t)d1);
            cf32 acc(0, 0);
            const long lo = std::max(0L, c - H), hi = std::min((long)n - 1, c + H);
            for (long k = lo; k <= hi; k++) acc += m[(size_t)k] * h[(size_t)(k - c + H)];
            s1[o] = acc;
        }
    }
    const size_t nOut = (size_t)std::floor((double)s1.size() * kRate / fs1);
    if (nOut < 16) return {};
    return cell::resampleBlock(s1.data(), s1.size(), fs1, kRate, 0, 40e3, 0, nOut);
}

// ---------------------------------------------------------------- slot synchronisation

namespace {

struct Tpl { std::vector<int> k; std::vector<cf32> e; };   // symbol numbers in the slot (1-based; <= 0: the slot before) and their steps

Tpl makeTpl(const uint8_t* bits, int nBits, int firstSym) {
    Tpl t;
    for (int i = 0; i < nBits / 2; i++) {
        const int ph = dibitPhase(bits[2 * i], bits[2 * i + 1]);
        t.k.push_back(firstSym + i);
        t.e.push_back(cf32((float)std::cos(ph * M_PI / 4), (float)-std::sin(ph * M_PI / 4)));   // conjugated
    }
    return t;
}

const Tpl& tplQ() { static const Tpl t = makeTpl(kTrainQ, 22, -4); return t; }
const Tpl& tplN() { static const Tpl t = makeTpl(kTrainN, 22, 123); return t; }
const Tpl& tplP() { static const Tpl t = makeTpl(kTrainP, 22, 123); return t; }
const Tpl& tplY() { static const Tpl t = makeTpl(kTrainY, 38, 108); return t; }

struct Score { float v = 0; cf32 sum{0, 0}; float mag = 0; };

inline Score score(const std::vector<cf32>& dd, long s, const Tpl& t) {
    Score r;
    for (size_t i = 0; i < t.k.size(); i++) {
        const long p = s + 4L * (t.k[i] - 1);
        if (p < 0 || p >= (long)dd.size()) return Score();
        const cf32 v = dd[(size_t)p];
        r.sum += v * t.e[i];
        r.mag += std::abs(v);
    }
    r.v = r.mag > 0 ? std::abs(r.sum) / r.mag : 0.f;
    return r;
}

struct SlotFit { float total = 0; Burst type = Burst::None; float mid = 0; cf32 rot{1, 0}; };

SlotFit fitSlot(const std::vector<cf32>& dd, long s) {
    SlotFit f;
    const Score q = score(dd, s, tplQ()), n = score(dd, s, tplN()), p = score(dd, s, tplP()), y = score(dd, s, tplY());
    const Score* best = &n; f.type = Burst::NormalFull;
    if (p.v > best->v) { best = &p; f.type = Burst::NormalHalf; }
    if (y.v > best->v) { best = &y; f.type = Burst::Sync; }
    f.mid = best->v;
    f.total = q.v + best->v;
    const cf32 s2 = q.sum + best->sum;
    const float a = std::abs(s2);
    f.rot = a > 0 ? std::conj(s2) / a : cf32(1, 0);
    return f;
}

} // namespace

// ---------------------------------------------------------------- one carrier

CarrierDecode decodeCarrier(const cf32* x, size_t n, int kMcc, int kMnc, int kCc, SpeechFrameSink* sink, CallTracker* calls, double nowSec, int voiceTn,
                            VoiceTimeline* timeline) {
    CarrierDecode out;
    if (n < (size_t)kSlotSamples * 8) return out;
    // matched filter
    static const std::vector<float> h = rrcTaps(kSps, 8);
    const int H = (int)h.size() / 2;
    std::vector<cf32> y(n);
    for (size_t i = 0; i < n; i++) {
        cf32 acc(0, 0);
        const long lo = std::max(0L, (long)i - H), hi = std::min((long)n - 1, (long)i + H);
        for (long k = lo; k <= hi; k++) acc += x[(size_t)k] * h[(size_t)(k - (long)i + H)];
        y[i] = acc;
    }
    // frequency: the fourth power of the one-symbol differential is -1 times the error's rotation, at the best sampling phase
    double fTot = 0;
    for (int pass = 0; pass < 2; pass++) {
        std::complex<double> acc[kSps];
        for (size_t i = kSps; i < n; i++) {
            const cf32 d = y[i] * std::conj(y[i - kSps]);
            const std::complex<double> d2 = std::complex<double>(d) * std::complex<double>(d);
            acc[i % kSps] += d2 * d2 / (std::abs(d) + 1e-20);
        }
        int bp = 0;
        for (int p = 1; p < kSps; p++) if (std::abs(acc[p]) > std::abs(acc[bp])) bp = p;
        const double w = std::arg(-acc[bp]) / 4;   // radians per symbol
        const double f = w * kSymRate / (2 * M_PI);
        fTot += f;
        cell::mixDown(y.data(), n, f, kRate);
    }
    out.cfoHz = fTot;
    std::vector<cf32> dd(n, cf32(0, 0));
    for (size_t i = kSps; i < n; i++) dd[i] = y[i] * std::conj(y[i - kSps]);
    // slot grid: the scores folded over the slot length
    const long s0 = 32;
    const long avail = (long)n - s0 - kSlotSamples - 160 * kSps;
    if (avail < kSlotSamples) return out;
    const int M = (int)std::min<long>(24, avail / kSlotSamples);
    std::vector<float> fold((size_t)kSlotSamples, 0.f);
    for (int m = 0; m < M; m++)
        for (int ph = 0; ph < kSlotSamples; ph++) fold[(size_t)ph] += fitSlot(dd, s0 + (long)m * kSlotSamples + ph).total;
    const int phase = (int)(std::max_element(fold.begin(), fold.end()) - fold.begin());
    if (fold[(size_t)phase] / M < 1.0f) return out;   // random data scores about 0.6
    out.tetra = true;
    // track the slots (the clock error moves them by a fraction of a sample each)
    struct Slot { long s; SlotFit f; float mu; std::vector<cf32> d; };
    std::vector<Slot> slots;
    for (long s = s0 + phase; s + kSlotSamples + 8 < (long)n; s += kSlotSamples) {
        SlotFit best = fitSlot(dd, s);
        long bs = s;
        for (long d = -3; d <= 3; d++) { if (!d) continue; const SlotFit f = fitSlot(dd, s + d); if (f.total > best.total) { best = f; bs = s + d; } }
        if (best.total >= 1.1f) s = bs; else best = fitSlot(dd, s);
        slots.push_back({s, best, 0.f, {}});
    }
    out.slots = (int)slots.size();
    // the fraction of a sample between the best whole sample and the true symbol instant (parabola through the scores), and the
    // differential symbols there (linear interpolation of the 4 samples per symbol), symbols -4 .. 255 of the slot
    constexpr int kD0 = -4;
    auto at = [&](double p) -> cf32 {
        const long i = (long)std::floor(p);
        if (i < 0 || i + 1 >= (long)n) return cf32(0, 0);
        const float f = (float)(p - (double)i);
        return y[(size_t)i] * (1 - f) + y[(size_t)i + 1] * f;
    };
    for (auto& sl : slots) {
        const float a = fitSlot(dd, sl.s - 1).total, b = sl.f.total, c = fitSlot(dd, sl.s + 1).total;
        const float den = a - 2 * b + c;
        sl.mu = den < -1e-6f ? std::max(-0.5f, std::min(0.5f, 0.5f * (a - c) / den)) : 0.f;
        sl.d.resize((size_t)(kSlotSyms - kD0 + 1));
        cf32 prev = at((double)sl.s + 4.0 * (kD0 - 2) + sl.mu);
        for (int k = kD0; k <= kSlotSyms; k++) {
            const cf32 cur = at((double)sl.s + 4.0 * (k - 1) + sl.mu);
            sl.d[(size_t)(k - kD0)] = cur * std::conj(prev);
            prev = cur;
        }
        const Tpl* mid = sl.f.type == Burst::Sync ? &tplY() : sl.f.type == Burst::NormalHalf ? &tplP() : &tplN();
        cf32 sum(0, 0);
        for (const Tpl* t : {&tplQ(), mid}) for (size_t i = 0; i < t->k.size(); i++) sum += sl.d[(size_t)(t->k[i] - kD0)] * t->e[i];
        if (std::abs(sum) > 0) sl.f.rot = std::conj(sum) / std::abs(sum);
    }
    // soft bits of a slot
    double mAcc = 0; for (const auto& sl : slots) for (int k = 1; k <= kSlotSyms; k += 8) mAcc += std::abs(sl.d[(size_t)(k - kD0)]);
    const float g = (float)(mAcc > 0 ? 4.0 / (mAcc / ((double)slots.size() * 32)) : 1.0);
    auto softBits = [&](const Slot& sl, float* llr) {
        for (int k = 1; k <= kSlotSyms; k++) {
            const cf32 d = sl.d[(size_t)(k - kD0)] * sl.f.rot * g;
            llr[2 * k - 2] = -d.imag();
            llr[2 * k - 1] = -d.real();
        }
    };
    // SNR from the training symbols
    {
        double sm = 0, sv = 0; int cnt = 0;
        for (const auto& sl : slots) {
            if (sl.f.total < 1.4f) continue;
            const Tpl* mid = sl.f.type == Burst::Sync ? &tplY() : sl.f.type == Burst::NormalHalf ? &tplP() : &tplN();
            std::vector<cf32> v;
            for (const Tpl* t : {&tplQ(), mid}) for (size_t i = 0; i < t->k.size(); i++) v.push_back(sl.d[(size_t)(t->k[i] + 4)] * t->e[i] * sl.f.rot * g);
            cf32 mu(0, 0); for (auto& a : v) mu += a; mu /= (float)v.size();
            double var = 0; for (auto& a : v) var += std::norm(a - mu);
            sm += std::norm(mu); sv += var / (double)v.size(); cnt++;
        }
        if (cnt && sv > 0) out.snrDb = (float)(10 * std::log10(sm / sv) + 3.0);
    }
    // pass 1: the synchronisation bursts
    std::vector<float> llr((size_t)kSlotBits);
    long syncSlot = -1;
    for (size_t i = 0; i < slots.size(); i++) {
        if (slots[i].f.type != Burst::Sync || slots[i].f.mid < 0.5f) continue;
        softBits(slots[i], llr.data());
        Bits t1;
        if (!decodeChannel(llr.data() + kSbSb, kBsch, scramblerInit(0, 0, 0), t1)) { out.blocksBad++; continue; }
        Sync s;
        if (!parseSync(t1, s)) { out.blocksBad++; continue; }
        out.synced = true; out.sync = s; out.syncs++; out.blocksOk++;
        syncSlot = (long)i;
    }
    int mcc = kMcc, mnc = kMnc, cc = kCc;
    if (out.synced) { mcc = out.sync.mcc; mnc = out.sync.mnc; cc = out.sync.colourCode; }
    if (mcc < 0 || mnc < 0 || cc < 0) return out;
    const uint32_t scr = scramblerInit(mcc, mnc, cc);
    out.scrambler = scr;
    // pass 2: the access assignment and the signalling blocks of every slot
    auto frameOf = [&](size_t i, int& tn, int& fn, int& mn) {
        tn = 0; fn = 0; mn = 0;
        if (syncSlot < 0) return;
        const long a0 = ((long)(out.sync.mn - 1) * 18 + (out.sync.fn - 1)) * 4 + (out.sync.tn - 1);
        const long a = ((a0 + (long)i - syncSlot) % (60L * 72) + 60L * 72) % (60L * 72);
        tn = (int)(a % 4) + 1; fn = (int)((a / 4) % 18) + 1; mn = (int)(a / 72) + 1;
    };
    struct TrafficSlot { int tn; std::vector<float> t; };
    std::vector<TrafficSlot> traffic;
    // the TDMA frames of each timeslot in the capture, in order: the speech frame index (tetra_voice.h, VoiceTimeline) of frames 1 to 17 of the
    // multiframe (frame 18 carries no speech) and the traffic burst of the frame (index into traffic, -1 none)
    struct SlotFrame { long speech; int burst; };
    std::vector<SlotFrame> frames[5];
    for (size_t i = 0; i < slots.size(); i++) {
        const Slot& sl = slots[i];
        int tn, fn, mn;
        frameOf(i, tn, fn, mn);
        if (tn >= 1 && fn <= 17) frames[tn].push_back({(long)(mn - 1) * 17 + fn - 1, -1});
        if (sl.f.mid < 0.5f || sl.f.total < 1.0f) continue;
        softBits(sl, llr.data());
        float bb[30];
        if (sl.f.type == Burst::Sync) std::copy(llr.begin() + kSbBb, llr.begin() + kSbBb + 30, bb);
        else { std::copy(llr.begin() + kNbBb1, llr.begin() + kNbBb1 + 14, bb); std::copy(llr.begin() + kNbBb2, llr.begin() + kNbBb2 + 16, bb + 14); }
        uint16_t aa = 0;
        int usage = -1;
        if (decodeAach(bb, scr, aa)) {
            usage = aachDlUsage(aa, fn == 18);
            if (tn >= 1 && usage >= 0) out.tsUsage[tn - 1] = usage;
        }
        auto block = [&](const float* l, const ChanSpec& c) {
            Bits t1;
            if (!decodeChannel(l, c, scr, t1)) { out.blocksBad++; return; }
            out.blocksOk++;
            MacBlock mb;
            parseMacBlock(t1, mb);
            out.macs.push_back(std::move(mb));
        };
        if (sl.f.type == Burst::Sync) block(llr.data() + kSbBkn2, kSchHd);
        else if (sl.f.type == Burst::NormalHalf) { block(llr.data() + kNbBkn1, kSchHd); block(llr.data() + kNbBkn2, kSchHd); }
        else {
            if (usage >= 4) {   // a traffic channel: handled after the signalling of the capture is known (encryption check first)
                out.trafficSlots++;
                std::vector<float> t(432);
                std::copy(llr.begin() + kNbBkn1, llr.begin() + kNbBkn1 + 216, t.begin());
                std::copy(llr.begin() + kNbBkn2, llr.begin() + kNbBkn2 + 216, t.begin() + 216);
                if (tn >= 1 && fn <= 17) frames[tn].back().burst = (int)traffic.size();
                traffic.push_back({tn, std::move(t)});
                continue;
            }
            float f[432];
            std::copy(llr.begin() + kNbBkn1, llr.begin() + kNbBkn1 + 216, f);
            std::copy(llr.begin() + kNbBkn2, llr.begin() + kNbBkn2 + 216, f + 216);
            block(f, kSchF);
        }
    }
    // call set-up PDUs (D-SETUP / D-CONNECT / release) read from the clear signalling of this capture: the per-call encryption state
    if (calls) {
        for (const auto& m : out.macs) if (m.sysinfo) calls->setMainCarrier(m.si.mainCarrier);
        for (const auto& m : out.macs) for (const auto& e : m.calls) calls->onEvent(e, nowSec);
    }
    // The encryption gate of every traffic burst, taken before any burst is read. What the signalling of this capture says about the cell:
    // any encrypted PDU means encrypted; the air interface encryption service may be advertised. Whether the call is clear is known from
    // its set-up PDU alone (CallTracker, read from the clear MAC-RESOURCE signalling of this or an earlier capture): a capture taken during
    // a talk spurt often has no MAC-RESOURCE at all, only null PDUs, broadcasts and the traffic itself, and demanding one here refused whole
    // captures of a clear call. Unknown counts as encrypted.
    std::vector<CallEncryption> gate(traffic.size());
    if (!traffic.empty()) {
        CallEncryption cell;
        for (const auto& m : out.macs) {
            if (m.encrypted > 0) cell.encMode = 1;
            if (m.sysinfo && m.si.bsService >= 0 && (m.si.bsService & kSvcAirEncryption)) cell.cellOffersEncryption = true;
        }
        for (size_t k = 0; k < traffic.size(); k++) {
            // the per-call gate AND the cell-level gate: clear only when the call's set-up PDU said "no encryption" and no PDU of the capture is encrypted
            CallEncryption ce = calls ? calls->slotState(traffic[k].tn, nowSec) : CallEncryption{};
            ce.cellOffersEncryption = cell.cellOffersEncryption;
            if (cell.encMode != 0) ce.encMode = 1;
            gate[k] = ce;
            const VoiceVerdict v = voiceVerdict(ce);
            out.voiceStatus = voiceVerdictText(v);
            if (v == VoiceVerdict::Passed) out.voicePassed++; else out.voiceNotDecoded++;
        }
    }
    if (!sink) return out;
    if (voiceTn == 0) {   // every clear burst, in the order of the air, nothing else
        for (size_t k = 0; k < traffic.size(); k++)
            if (processTrafficBurst(traffic[k].t.data(), traffic[k].t.data() + 216, scr, gate[k], sink) == VoiceVerdict::Passed) out.voiceTn = traffic[k].tn;
        return out;
    }
    // One timeslot reaches the codec: two calls (or a call and its other slots) through one speech decoder would be heard in turn. The followed
    // timeslot (the one asked for, else the first with a clear burst) goes to the sink frame by frame: its clear traffic bursts, and one silence for
    // every frame without one (a pause, a stolen half slot, a burst too weak or refused, a slot lost where two captures meet), so that the sink
    // plays the air as it was, pauses included, and never runs dry in the middle of a call.
    int ftn = voiceTn > 0 ? voiceTn : 0;
    for (size_t k = 0; k < traffic.size() && !ftn; k++) if (voiceVerdict(gate[k]) == VoiceVerdict::Passed) ftn = traffic[k].tn;
    if (ftn < 1 || ftn > 4) return out;
    constexpr long kMaxLost = 41;   // frames of the slot between two captures filled with silence: one capture; after more the sound has run dry anyway
    if (timeline && timeline->tn != ftn) timeline->next = -1;
    for (const SlotFrame& f : frames[ftn]) {
        if (timeline && timeline->next >= 0) {
            const long lost = (f.speech - timeline->next + VoiceTimeline::kSpeechFrames) % VoiceTimeline::kSpeechFrames;
            if (lost <= kMaxLost) for (long k = 0; k < lost; k++) { sink->onMissingBurst(); out.voiceGaps++; }
        }
        if (f.burst >= 0 && processTrafficBurst(traffic[(size_t)f.burst].t.data(), traffic[(size_t)f.burst].t.data() + 216, scr, gate[(size_t)f.burst], sink) == VoiceVerdict::Passed) {
            out.voiceTn = ftn;
        } else { sink->onMissingBurst(); out.voiceGaps++; }
        if (timeline) { timeline->tn = ftn; timeline->next = (f.speech + 1) % VoiceTimeline::kSpeechFrames; }
    }
    return out;
}

} // namespace tetra

// ---------------------------------------------------------------- receiver

std::vector<tetra::CarrierPeak> tetra::findCarriers(const cf32* x, size_t n, double fs) {
    std::vector<CarrierPeak> out;
    const int nfft = cell::pow2At((int)(fs / 1000.0));
    if (nfft < 64 || n < (size_t)nfft * 8) return out;
    const std::vector<float> p0 = cell::welch(x, n, nfft, 300);
    std::vector<double> p((size_t)nfft);
    for (int i = 0; i < nfft; i++) p[(size_t)i] = (p0[(size_t)std::max(0, i - 1)] + p0[(size_t)i] + p0[(size_t)std::min(nfft - 1, i + 1)]) / 3.0;
    const double bin = fs / nfft;
    // the usable band: the middle kUsableBand of the span, where a radio's decimation filter is flat (the span the application steps by);
    // the roll-off outside it is neither searched nor taken into the floor
    const int lo = nfft / 2 - (int)(nfft * kUsableBand / 2), hi = nfft / 2 + (int)(nfft * kUsableBand / 2) - 1;
    // the local floor: the 20th percentile of the usable bins within kFloorWindowHz / 2 of each bin, taken on a grid of nfft / 128 bins and
    // held between the grid points (a carrier is 25 kHz wide and a row of them a few hundred kHz: the floor is the noise between the rows)
    const int half = std::max(8, (int)(kFloorWindowHz / 2 / bin)), step = std::max(1, nfft / 128);
    std::vector<double> floorP((size_t)nfft, 1e-30), tmp;
    for (int g = lo; g <= hi; g += step) {
        const int a = std::max(lo, g - half), b = std::min(hi, g + half);
        tmp.assign(p.begin() + a, p.begin() + b + 1);
        std::nth_element(tmp.begin(), tmp.begin() + (std::ptrdiff_t)(tmp.size() / 5), tmp.end());
        const double f = std::max(1e-30, tmp[tmp.size() / 5]);
        for (int k = g; k <= std::min(hi, g + step - 1); k++) floorP[(size_t)k] = f;
    }
    auto above = [&](int k) { return p[(size_t)k] >= floorP[(size_t)k] * 4.0; };   // 6 dB over the local floor
    int i = lo;
    while (i <= hi) {
        if (!above(i)) { i++; continue; }
        int j = i;
        while (j + 1 <= hi && (above(j + 1) || (j + 2 <= hi && above(j + 2)))) j++;
        const double w = (j - i + 1) * bin;
        if (w >= 9e3 && w <= 400e3) {
            const int nc = std::max(1, (int)std::lround(w / 25e3));
            for (int c = 0; c < nc; c++) {
                const int a = i + (int)std::floor((double)c * (j - i + 1) / nc), b = i + (int)std::floor((double)(c + 1) * (j - i + 1) / nc) - 1;
                double sp = 0, sf = 0, pk = 0, fl = 0;
                for (int k = a; k <= b; k++) { const double v = std::max(0.0, p[(size_t)k] - floorP[(size_t)k]); sp += v; sf += v * k; pk = std::max(pk, p[(size_t)k]); fl += floorP[(size_t)k]; }
                if (sp <= 0) continue;
                fl /= (double)(b - a + 1);
                CarrierPeak cr;
                cr.off = (sf / sp - nfft / 2.0) * bin;
                cr.levelDb = (float)(10 * std::log10(pk / fl));
                cr.powerDb = (float)(10 * std::log10((sp + fl * (b - a + 1)) / (0.375 * nfft) + 1e-30));
                out.push_back(cr);
            }
        }
        i = j + 1;
    }
    std::sort(out.begin(), out.end(), [](const CarrierPeak& a, const CarrierPeak& b) { return a.levelDb > b.levelDb; });
    if (out.size() > 24) out.resize(24);
    return out;
}

namespace {

int64_t wallNow() { return dect2::unixNow(); }

using Carrier = tetra::CarrierPeak;

} // namespace

struct TetraReceiver::Impl {
    mutable std::mutex mu;
    double rateSet = 0, sigOff = 0, centerHz = 0;
    std::function<void(const std::string&)> log;
    std::atomic<uint64_t> gen{0};
    std::atomic<bool> idle{true};
    std::atomic<bool> clearReq{false};
    std::atomic<SpeechFrameSink*> voiceSink{nullptr};
    // per-call encryption state of each carrier (analysis thread only), keyed by the carrier's 25 kHz channel
    std::map<long, tetra::CallTracker> callTrackers;
    tetra::VoiceStreamPicker voicePick;   // the one carrier and timeslot whose voice is played
    tetra::VoiceTimeline voiceTimeline;   // its frames across captures (the slots lost between two captures go to the sink as silence)
    static long callKey(double center, double off) { return std::lround(((center > 0 ? center : 0.0) + off) / 25e3); }
    // capture (analysis thread)
    std::vector<cf32> acc;
    size_t want = 0;
    uint64_t accGen = 0;
    double accCenter = 0;
    uint64_t total = 0;
    double levelAcc = 0; uint64_t levelN = 0;
    double lastPubSec = -1;
    // worker
    std::thread th;
    std::condition_variable cv;
    bool stop = false, jobReady = false;
    std::vector<cf32> job;
    double jobCenter = 0, jobRate = 0, jobSec = 0;
    uint64_t jobGen = 0;
    // results (under mu)
    TetraTelemetry tel, pub;

    Impl() { th = dect2::namedThread("onair-tetra", [this] { run(); }); }
    ~Impl() {
        { std::lock_guard<std::mutex> lk(mu); stop = true; }
        cv.notify_all();
        if (th.joinable()) th.join();
    }

    void event(const std::string& s, double sec) {
        TetraEvent e; e.wallTime = wallNow(); e.sec = sec; e.text = s;
        tel.events.insert(tel.events.begin(), e);
        if (tel.events.size() > kTetraLogMax) tel.events.resize(kTetraLogMax);
        if (log) log("tetra: " + s);
    }

    void publish() { tel.seq++; pub = tel; }

    void run() {
        for (;;) {
            std::vector<cf32> x;
            double center, rate, sec; uint64_t g;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return stop || jobReady; });
                if (stop) return;
                x.swap(job); jobReady = false;
                center = jobCenter; rate = jobRate; sec = jobSec; g = jobGen;
            }
            process(x, center, rate, sec, g);
            idle = true;
        }
    }

    // 25 kHz humps in the spectrum: runs of bins over the floor, split into 25 kHz carriers
    std::vector<Carrier> carriers(const std::vector<cf32>& x, double fs) { return tetra::findCarriers(x.data(), x.size(), fs); }

    void process(const std::vector<cf32>& x, double center, double fs, double sec, uint64_t g) {
        const std::vector<Carrier> car = carriers(x, fs);
        struct Found { Carrier c; tetra::CarrierDecode d; double freq; };
        std::vector<Found> found;
        std::vector<TetraCarrierSeen> seen;
        voicePick.newCapture();
        bool captureEnded = false;
        for (const auto& c : car) {
            if (gen.load() != g) return;   // retuned meanwhile: these samples belong to another frequency
            int kMcc = -1, kMnc = -1, kCc = -1;
            {
                std::lock_guard<std::mutex> lk(mu);
                const double f = (center > 0 ? center : 0.0) + c.off;
                for (const auto& t : tel.cells) if (std::fabs(t.freqHz - f) < 5e3 && t.mcc >= 0) { kMcc = t.mcc; kMnc = t.mnc; kCc = t.colourCode; break; }
            }
            const std::vector<cf32> s = tetra::channelize(x.data(), x.size(), fs, c.off);
            if (callTrackers.size() > 64) callTrackers.clear();
            const long key = callKey(center, c.off);
            int voiceTn = 0;
            const bool voice = voicePick.offer(key, sec, voiceTn);
            if (voice && voiceTimeline.key != key) { voiceTimeline = tetra::VoiceTimeline{}; voiceTimeline.key = key; }   // another carrier: a new stream
            tetra::CarrierDecode d = tetra::decodeCarrier(s.data(), s.size(), kMcc, kMnc, kCc, voice ? voiceSink.load() : nullptr, &callTrackers[key], (double)wallNow(), voiceTn,
                                                          voice ? &voiceTimeline : nullptr);
            if (voice) voicePick.heard(key, d.voiceTn, sec);
            if (voice && (d.voiceTn > 0 || d.voiceGaps > 0)) {   // the voice of this capture is all out: told at once, not after the other carriers (the sink may hold it)
                if (SpeechFrameSink* vs = voiceSink.load()) vs->onCaptureEnd();
                captureEnded = true;
            }
            const double freq = (center > 0 ? center : 0.0) + c.off + d.cfoHz;
            TetraCarrierSeen cs; cs.freqHz = d.tetra ? freq : (center > 0 ? center : 0.0) + c.off; cs.levelDb = c.levelDb; cs.synced = d.synced;
            seen.push_back(cs);
            if (d.tetra && (d.synced || d.scrambler)) found.push_back({c, std::move(d), freq});
        }
        if (!captureEnded) if (SpeechFrameSink* vs = voiceSink.load()) vs->onCaptureEnd();   // no carrier gave voice: the capture is over all the same
        std::lock_guard<std::mutex> lk(mu);
        if (gen.load() != g) return;
        if (clearReq.exchange(false)) { tel.cells.clear(); tel.events.clear(); tel.blocksOk = tel.blocksBad = 0; }
        tel.blocks++;
        tel.carriers = seen;
        for (auto& fr : found) merge(fr.c, fr.d, fr.freq, center, sec);
        std::stable_sort(tel.cells.begin(), tel.cells.end(), [](const TetraCell& a, const TetraCell& b) { return a.lastSec > b.lastSec; });
        if (tel.cells.size() > kTetraMax) tel.cells.resize(kTetraMax);
        refreshState(sec);
        publish();
    }

    void refreshState(double sec) {
        const TetraCell* best = nullptr;
        bool recent = false;
        for (const auto& c : tel.cells) {
            if (sec - c.lastSec < 30) recent = true;
            if (sec - c.lastSec < 30 && (!best || c.levelDb > best->levelDb)) best = &c;
        }
        tel.dataValid = recent;
        tel.state = (tel.timeSec > 0 || tel.blocks) ? (recent ? 2 : 1) : 0;
        if (best) { tel.cfoHz = best->cfoHz; tel.snrDb = best->snrDb; }
    }

    static std::string freqText(double hz) { char b[32]; snprintf(b, sizeof b, "%.4f MHz", hz / 1e6); return b; }

    void merge(const Carrier& cr, const tetra::CarrierDecode& d, double freq, double center, double sec) {
        const int mcc = d.synced ? d.sync.mcc : -1, mnc = d.synced ? d.sync.mnc : -1, cc = d.synced ? d.sync.colourCode : -1;
        TetraCell* t = nullptr;
        for (auto& c : tel.cells) {
            if (std::fabs(c.freqHz - freq) > 5e3) continue;
            if (d.synced && (c.mcc != mcc || c.mnc != mnc || c.colourCode != cc)) continue;
            t = &c; break;
        }
        if (!t && !d.synced) return;   // an identity is needed for a new entry
        char b[320];
        const bool isNew = !t;
        if (!t) {
            tel.cells.push_back(TetraCell());
            t = &tel.cells.back();
            t->firstWall = wallNow();
        }
        t->freqHz = freq;
        if (!t->hasSysinfo || !t->isMain) t->nominalHz = freq;
        t->levelDb = cr.powerDb; t->snrDb = d.snrDb;
        t->lastWall = wallNow(); t->lastSec = sec; t->seen++;
        t->bschOk += (uint64_t)d.syncs;
        t->blocksOk += (uint64_t)d.blocksOk; t->blocksBad += (uint64_t)d.blocksBad;
        tel.blocksOk += (uint64_t)d.blocksOk; tel.blocksBad += (uint64_t)d.blocksBad;
        for (int i = 0; i < 4; i++) if (d.tsUsage[i] >= 0) t->tsUsage[i] = d.tsUsage[i];
        if (d.synced) {
            const tetra::Sync& s = d.sync;
            t->mcc = s.mcc; t->mnc = s.mnc; t->colourCode = s.colourCode; t->systemCode = s.systemCode; t->sharingMode = s.sharingMode;
            t->tsReservedFrames = s.tsReservedFrames; t->uplaneDtx = s.uplaneDtx; t->frame18Ext = s.frame18Ext; t->lateEntry = s.lateEntry;
            t->nbrBroadcast = s.nbrBroadcast; t->cellLoad = s.cellLoad; t->tn = s.tn; t->fn = s.fn; t->mn = s.mn;
        }
        if (isNew) {
            snprintf(b, sizeof b, "%s: MCC %d MNC %d, colour code %d%s", freqText(freq).c_str(), t->mcc, t->mnc, t->colourCode, center > 0 ? "" : " (offset from the file's centre)");
            event(b, sec);
        }
        const bool encBefore = t->encryption();
        for (const auto& m : d.macs) {
            t->encryptedPdus += (uint64_t)m.encrypted;
            for (int k = 0; k < 8; k++) t->clearPdus += (uint64_t)m.byPd[k];
            if (m.sysinfo) {
                const tetra::Sysinfo& s = m.si;
                const bool first = !t->hasSysinfo || t->la != s.la || t->mainCarrier != s.mainCarrier;
                t->bnchOk++;
                t->hasSysinfo = true;
                t->mainCarrier = s.mainCarrier; t->band = s.band; t->offset = s.offset; t->duplex = s.duplex; t->reverse = s.reverse;
                t->numScch = s.numScch; t->msTxPwrMax = s.msTxPwrMax; t->rxLevAccessMin = s.rxLevAccessMin; t->accessParam = s.accessParam; t->dlTimeout = s.dlTimeout;
                t->cckFlag = s.cckFlag; t->hyperframeOrCck = s.hyperframeOrCck; t->optFlag = s.optFlag; t->optValue = s.optValue;
                t->la = s.la; t->subscriberClass = s.subscriberClass; t->bsService = s.bsService;
                t->aie = (s.bsService & tetra::kSvcAirEncryption) != 0;
                const tetra::Security sc = tetra::parseSecurity(s);
                if (sc.valid) { t->secValid = true; t->authRequired = sc.authRequired; t->secClass1 = sc.class1; t->secClass2 = sc.class2; t->secClass3 = sc.class3; }
                if (!t->aie) { t->secClass2 = false; t->secClass3 = false; }   // a class kept from an earlier broadcast: the service bit is the newest word
                t->mainHz = tetra::dlCarrierHz(s.band, s.mainCarrier, s.offset);
                t->uplinkHz = tetra::ulCarrierHz(s.band, s.mainCarrier, s.offset, s.duplex, s.reverse);
                // the main carrier: the announced frequency is this one, or near enough for a radio whose clock is off (up to about 150 ppm)
                // when no other carrier of the cell is nearer
                const double dm = std::fabs(t->mainHz - freq);
                bool nearer = false;
                for (const auto& o : tel.cells)
                    if (&o != t && o.mcc == t->mcc && o.mnc == t->mnc && o.colourCode == t->colourCode && std::fabs(o.freqHz - t->mainHz) < dm) nearer = true;
                const bool wasMain = t->isMain;
                t->isMain = center <= 0 || dm < 8e3 || (dm < 60e3 && !nearer);
                if (t->isMain) t->nominalHz = t->mainHz;
                t->cfoHz = center > 0 ? freq - t->nominalHz : d.cfoHz;
                if (t->isMain && !wasMain && center > 0 && dm >= 3e3) {
                    snprintf(b, sizeof b, "%s: heard %.1f kHz from the frequency the cell announces: the radio's frequency is about %.0f ppm off",
                             freqText(t->mainHz).c_str(), (freq - t->mainHz) / 1e3, (freq - t->mainHz) / t->mainHz * 1e6);
                    event(b, sec);
                }
                if (first) {
                    snprintf(b, sizeof b, "%s: LA %d, main carrier %d (%s), uplink %s, services: %s", freqText(freq).c_str(), s.la, s.mainCarrier,
                             freqText(t->mainHz).c_str(), t->uplinkHz > 0 ? freqText(t->uplinkHz).c_str() : "unknown", tetra::serviceText(s.bsService).c_str());
                    event(b, sec);
                }
            }
            for (const auto& e : m.calls) {
                if (e.kind != 1 || e.gssi < 0) continue;   // only the set-up of a group call names its group
                auto g = std::find_if(t->groups.begin(), t->groups.end(), [&](const TetraGroupInfo& x) { return x.gssi == e.gssi; });
                if (g == t->groups.end()) {
                    TetraGroupInfo n; n.gssi = e.gssi; n.firstWall = wallNow();
                    t->groups.push_back(n);
                    g = t->groups.end() - 1;
                    snprintf(b, sizeof b, "%s: group call to GSSI %d (%s)", freqText(freq).c_str(), e.gssi, e.enc == 1 ? "end-to-end encrypted" : e.enc == 0 ? "clear" : "encryption not stated");
                    event(b, sec);
                }
                if (g->lastCallId != e.callId) { g->calls++; g->lastCallId = e.callId; }   // a set-up is repeated while the call starts
                if (e.enc >= 0) g->enc = e.enc;
                g->lastWall = wallNow();
                std::stable_sort(t->groups.begin(), t->groups.end(), [](const TetraGroupInfo& x, const TetraGroupInfo& y) { return x.calls > y.calls; });
            }
            for (const auto& nw : m.nwrk) {
                const bool first = !t->hasNwrk || t->neighbours.size() != nw.nbrs.size();
                t->hasNwrk = true;
                t->reselect = nw.reselect;
                if (nw.hasTime) t->netTime = tetra::networkTimeText(nw.time48);
                std::vector<TetraNeighbourInfo> v;
                for (const auto& nb : nw.nbrs) {
                    TetraNeighbourInfo ni;
                    ni.cellId = nb.cellId; ni.carrier = nb.carrier; ni.la = nb.la; ni.cellLoad = nb.cellLoad; ni.synced = nb.synced; ni.mcc = nb.mcc; ni.mnc = nb.mnc;
                    if (nb.hasExt) ni.freqHz = tetra::dlCarrierHz(nb.band, nb.carrier, nb.offset);
                    else if (t->hasSysinfo) ni.freqHz = tetra::dlCarrierHz(t->band, nb.carrier, t->offset);
                    v.push_back(ni);
                }
                // the cell may send its neighbours over several broadcasts: keep every one heard
                for (const auto& ni : v) {
                    bool have = false;
                    for (auto& o : t->neighbours) if (o.cellId == ni.cellId && o.carrier == ni.carrier) { o = ni; have = true; }
                    if (!have) t->neighbours.push_back(ni);
                }
                if (first && !v.empty()) {
                    std::string l;
                    for (const auto& ni : v) { char e[64]; snprintf(e, sizeof e, "%s%s (LA %d)", l.empty() ? "" : ", ", ni.freqHz > 0 ? freqText(ni.freqHz).c_str() : "?", ni.la); l += e; }
                    snprintf(b, sizeof b, "%s: neighbour cells %s", freqText(freq).c_str(), l.c_str());
                    event(b, sec);
                }
            }
        }
        if (!t->hasSysinfo) t->cfoHz = 0;
        if (!encBefore && t->encryption()) {
            static const char* const kind[] = {"", "", "class 2", "class 3", "service available"};
            snprintf(b, sizeof b, "%s: air interface encryption %s%s%s (nothing is decrypted)", freqText(freq).c_str(), t->airEnc() >= 2 ? "advertised: " : "", t->airEnc() >= 2 ? kind[t->airEnc()] : "not advertised",
                     t->encryptedPdus ? ", some signalling PDUs carry an encryption mode" : "");
            event(b, sec);
        }
    }
};

TetraReceiver::TetraReceiver() : p_(new Impl) {}
TetraReceiver::~TetraReceiver() = default;

void TetraReceiver::configure(double rate) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rateSet = rate;
    p_->tel.inputRate = rate;
    p_->want = (size_t)(rate * TetraReceiver::kCaptureSec);
    p_->gen++;
}

void TetraReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->sigOff = hz; p_->gen++; }
void TetraReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (std::fabs(hz - p_->centerHz) > 1) p_->gen++;
    p_->centerHz = hz; p_->tel.centerHz = hz;
}
bool TetraReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= tetraTuning().minSampleRate - 1; }
void TetraReceiver::setVoiceSink(SpeechFrameSink* s) { p_->voiceSink.store(s); }
void TetraReceiver::reset() { p_->gen++; }
void TetraReceiver::clearCells() { std::lock_guard<std::mutex> lk(p_->mu); p_->tel.cells.clear(); p_->tel.events.clear(); p_->tel.blocksOk = p_->tel.blocksBad = 0; p_->publish(); }
void TetraReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

bool TetraReceiver::busy() const { std::lock_guard<std::mutex> lk(p_->mu); return !p_->idle.load() || p_->jobReady; }

// the cells and the event log already stay through reset() and configure() (the sample clock runs on): only the user's Clear empties them
void TetraReceiver::clearResults() { clearCells(); }
std::string TetraReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->tel.timeSec, p_->tel.cells, p_->tel.events);
}
bool TetraReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    TetraTelemetry l;
    if (!results::unpack(blob, t0, l.cells, l.events)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    const double dt = p_->tel.timeSec - t0;    // onto this run's clock
    for (auto& c : l.cells) c.lastSec += dt;
    for (auto& e : l.events) e.sec += dt;
    p_->tel.cells = std::move(l.cells); p_->tel.events = std::move(l.events);
    p_->publish();
    return true;
}
bool TetraReceiver::telemetry(TetraTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void TetraReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    double rate, center;
    { std::lock_guard<std::mutex> lk(m.mu); rate = m.rateSet; center = m.centerHz; }
    if (rate < tetraTuning().minSampleRate - 1 || m.want == 0) return;
    for (size_t i = 0; i < n; i++) { const cf32 v = x[i]; if (std::isfinite(v.real()) && std::isfinite(v.imag())) m.levelAcc += std::norm(v); }
    m.levelN += n;
    m.total += n;
    const uint64_t g = m.gen.load();
    if (g != m.accGen) { m.acc.clear(); m.accGen = g; }
    // The next capture is gathered while the last one is decoded, so the captures follow each other without a gap (voice is heard
    // whole). Samples are dropped only when a capture is complete and the decoder is still busy, i.e. when it is slower than real time.
    for (size_t i = 0; i < n;) {
        if (m.acc.empty()) { m.accCenter = center; m.acc.reserve(m.want); }
        const size_t take = std::min(n - i, m.want - m.acc.size());
        for (size_t k = 0; k < take; k++) {
            cf32 v = x[i + k];
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            m.acc.push_back(v);
        }
        i += take;
        if (m.acc.size() < m.want || !m.idle.load()) break;
        std::lock_guard<std::mutex> lk(m.mu);
        m.job.swap(m.acc); m.acc.clear();
        m.jobCenter = m.accCenter; m.jobRate = rate; m.jobSec = (double)(m.total - n + i) / rate; m.jobGen = g;
        m.jobReady = true; m.idle = false;
        m.cv.notify_one();
    }
    const double now = (double)m.total / rate;
    if (now - m.lastPubSec >= 0.5) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.tel.timeSec = now;
        m.tel.levelDb = m.levelN ? (float)(10 * std::log10(m.levelAcc / (double)m.levelN + 1e-20)) : -200.f;
        m.levelAcc = 0; m.levelN = 0;
        m.lastPubSec = now;
        m.refreshState(now);
        if (m.tel.state == 0) m.tel.state = 1;
        m.publish();
    }
}

} // namespace dect2
