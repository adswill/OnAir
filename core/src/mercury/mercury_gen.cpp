// Mercury test signal (see mercury_gen.h). Nothing is transmitted.
#include "dect2/mercury/mercury_gen.h"
#include "dect2/mercury/mercury_mfsk.h"
#include "dect2/mercury/mercury_proto.h"
#include "dect2/mercury/mercury_raptorq.h"
#include "dect2/mercury/mercury_rx.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/dsp/portable_rng.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {
namespace mercury {

namespace {

using dect2::math::kPi;
constexpr double kFs = 8000.0;

const char* kCaller = "LY2ONA";
const char* kCallee = "LY1HRM";
constexpr int kSid = 0x21;
constexpr uint16_t kNonce = 0xBEEF;
constexpr int kStart = 3;                 // the caller's rounds start on DATAC3
constexpr double kCalleeCfo = 3.5;        // the callee's radio sits 3.5 Hz off the caller's

std::vector<uint8_t> bytesOf(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// a carousel block: the application's bytes and the keyed CRC-32, cut into pieces of P bytes (the last zero padded)
struct TxBlock {
    int id, K, pad;
    std::vector<std::vector<uint8_t>> pieces;   // data pieces, then repair pieces on demand
};
TxBlock makeBlock(int id, const std::vector<uint8_t>& app, uint16_t seed, int P) {
    std::vector<uint8_t> b = app;
    const uint32_t c = blockCheck(seed, id & 0xFF, (int)((app.size() + 4 + (size_t)P - 1) / (size_t)P), app.data(), app.size());
    for (int i = 0; i < 4; i++) b.push_back((uint8_t)(c >> (8 * i)));
    TxBlock t;
    t.id = id;
    t.K = (int)((b.size() + (size_t)P - 1) / (size_t)P);
    t.pad = t.K * P - (int)b.size();
    b.resize((size_t)(t.K * P), 0);
    for (int i = 0; i < t.K; i++) t.pieces.emplace_back(b.begin() + i * P, b.begin() + (i + 1) * P);
    return t;
}
// piece idx of the block (data or repair)
std::vector<uint8_t> piece(const TxBlock& t, int idx, int P) {
    if (idx < t.K) return t.pieces[(size_t)idx];
    std::vector<uint8_t> r;
    rsRepair(t.K, P, t.pieces, idx - t.K, r);
    return r;
}
// a DATA frame of the block's pieces idx[] (consecutive indices only: the wire carries the first index and a count)
std::vector<uint8_t> dataFrame(int mode, const TxBlock& t, int first, int count, int left, int pollId, int P) {
    std::vector<uint8_t> pcs;
    for (int i = 0; i < count; i++) { const std::vector<uint8_t> p = piece(t, (first + i) % 256, P); pcs.insert(pcs.end(), p.begin(), p.end()); }
    return buildCarData((size_t)modePayloadBytes(mode), left, pollId, t.id & 0x0F, t.id, t.K, first, count, t.pad, pcs.data(), P);
}

CarCtl poll(int pollId, int level, int frames, float snr, int base, bool hasData) {
    CarCtl c;
    c.type = 0; c.pollId = pollId; c.level = level; c.frames = frames; c.snrDb = snr; c.base = base; c.hasData = hasData;
    for (int& v : c.need) v = -1;
    c.need[0] = 0;
    c.highBlock = base;
    return c;
}

GenBurst burst(int mode, const std::vector<uint8_t>& payload, double gap, uint16_t seed = 0, double cfo = 0) {
    GenBurst b;
    b.mode = mode;
    b.frame = mode >= kNumOfdmModes ? makeMfskFrame(payload.data(), payload.size(), seed) : makeFrame(mode, payload.data(), payload.size(), seed);
    b.gapSec = gap;
    b.cfoHz = cfo;
    return b;
}

void addFile(std::vector<GenBurst>& v) {
    // the bundle (uint32 LE length, name '\n', data), RaptorQ with T = 41 (Mercury's fixed symbol size), 12 symbols per DATAC1 frame
    const std::string name = demoFileName();
    const std::vector<uint8_t> data = demoFileData();
    std::vector<uint8_t> obj;
    const uint32_t total = (uint32_t)(name.size() + 1 + data.size());
    for (int i = 0; i < 4; i++) obj.push_back((uint8_t)(total >> (8 * i)));
    obj.insert(obj.end(), name.begin(), name.end());
    obj.push_back('\n');
    obj.insert(obj.end(), data.begin(), data.end());
    const uint32_t T = 41, F = (uint32_t)obj.size();
    const uint32_t K = (F + T - 1) / T;
    std::vector<uint8_t> src((size_t)K * T, 0);
    std::memcpy(src.data(), obj.data(), obj.size());
    RqBlockEncoder enc(K, T, src.data());
    const Geometry& g = geometry(kDatac1);
    const int per = (g.payloadBytes - 12) / (int)T;
    // ESIs: the first frame of source symbols, then a frame is skipped (lost), then the rest and repair symbols
    std::vector<uint32_t> starts;
    for (uint32_t e = 0; e < K; e += (uint32_t)per) if (e != (uint32_t)per) starts.push_back(e);
    starts.push_back(K + 5);
    for (uint32_t e0 : starts) {
        std::vector<uint8_t> f((size_t)g.payloadBytes, 0);
        f[0] = framerByte(kPtBcastControl, 7);   // session 7
        f[1] = (uint8_t)F; f[2] = (uint8_t)(F >> 8); f[3] = (uint8_t)(F >> 16);
        f[4] = (uint8_t)(T - 1); f[5] = (uint8_t)((T - 1) >> 8);
        f[6] = 0;            // Z - 1
        f[7] = 0; f[8] = 0;  // N - 1
        f[9] = 0;            // SBN
        f[10] = (uint8_t)e0; f[11] = (uint8_t)(e0 >> 8);
        for (int i = 0; i < per; i++) {
            const std::vector<uint8_t> s = enc.symbol(e0 + (uint32_t)i);
            std::memcpy(f.data() + 12 + (size_t)i * T, s.data(), T);
        }
        v.push_back(burst(kDatac1, f, 0.4));
    }
}

} // namespace

std::string demoCallerText() {
    return "This is LY2ONA calling LY1HRM over Mercury. OnAir monitor test: a connected session, both directions reassembled by a "
           "station that only listens.\n";
}
std::string demoCalleeText() { return "73 de LY1HRM, all received, QSL.\n"; }
std::string demoFloorText() { return "LY2ONA at the floor: 32-FSK, 13 seconds a frame, four pieces of 23 bytes each.\n"; }
std::string demoFileName() { return "onair-mercury-test.txt"; }
std::vector<uint8_t> demoFileData() {
    std::string s = "OnAir Mercury broadcast test file.\n";
    for (int i = 1; i <= 24; i++) s += "Line " + std::to_string(i) + ": the RaptorQ carousel of hermes-broadcast, symbol size 41 bytes.\n";
    return bytesOf(s);
}

std::vector<GenBurst> demoScript(int which, int singleMode) {
    std::vector<GenBurst> v;
    if (which == 2) {
        const int m = std::max(0, std::min(kNumModes - 1, singleMode));
        const std::string txt = std::string("OnAir test frame on ") + modeName(m);
        v.push_back(burst(m, buildBcastMessage((const uint8_t*)txt.data(), txt.size(), (size_t)modePayloadBytes(m)), 1.0));
        return v;
    }
    if (which == 3) {
        for (int m = 0; m < kNumModes; m++) {
            const std::string txt = std::string("OnAir ") + modeName(m);
            v.push_back(burst(m, buildBcastMessage((const uint8_t*)txt.data(), txt.size(), (size_t)modePayloadBytes(m)), 1.0));
        }
        return v;
    }
    if (which == 4) {
        // a link at the floor: the CALL and ACCEPT on MFSK, the callee hears the caller below the control mode (so its control goes on
        // MFSK behind the mark 0x07), pieces of 23 bytes, four to a frame
        const int sid = 0x22, P = pieceSize(0, true);
        const uint16_t seed = sessionSeed(sid, 0x0F0F, 0, kCaller, kCallee);
        std::vector<uint8_t> f = buildCall(sid, kCaller, kCallee, 2300);
        v.push_back(burst(kMfsk, f, 1.0));
        f = buildAccept(sid, kCallee, kCaller, 2300, 0, true, -11.f, 0x0F0F);
        v.push_back(burst(kMfsk, f, 1.0, 0, kCalleeCfo));
        const TxBlock b0 = makeBlock(0, bytesOf(demoFloorText()), seed, P);
        const int per = (modePayloadBytes(kMfsk) - 6) / P;
        for (int first = 0, k = 0; first < b0.K; first += per, k++)
            v.push_back(burst(kMfsk, dataFrame(kMfsk, b0, first, std::min(per, b0.K - first), first + per < b0.K ? 1 : 0, 0, P), first + per < b0.K ? 0.2 : 1.0, seed));
        std::vector<uint8_t> c = {0x07};
        const std::vector<uint8_t> ctl = buildCarCtl(poll(1, 0, 2, -11.f, 1, false));
        c.insert(c.end(), ctl.begin(), ctl.end());
        v.push_back(burst(kMfsk, c, 1.0, seed, kCalleeCfo));
        v.push_back(burst(kMfsk, buildDisconnect(sid, -12.f), 1.0));
        return v;
    }
    if (which == 0) {
        const uint16_t seed = sessionSeed(kSid, kNonce, kStart, kCaller, kCallee);
        const int P = pieceSize(kStart);
        v.push_back(burst(kDatac16, buildCq(kCallee, 2300), 2.0, 0, kCalleeCfo));
        v.push_back(burst(kDatac16, buildCall(kSid, kCaller, kCallee, 2300), 1.0));
        v.push_back(burst(kDatac16, buildAccept(kSid, kCallee, kCaller, 2300, kStart, false, 12.f, kNonce), 1.0, 0, kCalleeCfo));
        // the caller's first round on DATAC3: block 0, data piece 5 lost, the first repair piece instead
        const TxBlock b0 = makeBlock(0, bytesOf(demoCallerText()), seed, P);
        const int per3 = (geometry(kDatac3).payloadBytes - 6) / P;
        v.push_back(burst(kDatac3, dataFrame(kDatac3, b0, 0, per3, 1, 0, P), 0.1, seed));
        v.push_back(burst(kDatac3, dataFrame(kDatac3, b0, per3 + 1, b0.K - per3, 0, 0, P), 1.0, seed));
        // the callee's handover poll and its round on DATAC15 (one piece a frame): its data piece 1 lost, repair piece 0 instead
        CarCtl h = poll(1, 6, 1, 11.f, 1, true);
        h.hLevel = 1; h.hFrames = 2; h.hId = 2;
        v.push_back(burst(kDatac16, buildCarCtl(h), 0.3, seed, kCalleeCfo));
        const TxBlock c0 = makeBlock(0, bytesOf(demoCalleeText()), seed, P);
        v.push_back(burst(kDatac15, dataFrame(kDatac15, c0, 0, 1, 1, 2, P), 0.1, seed, kCalleeCfo));
        v.push_back(burst(kDatac15, dataFrame(kDatac15, c0, c0.K, 1, 0, 2, P), 1.0, seed, kCalleeCfo));
        // the caller acknowledges, then its round on QAM16C2 (block 1)
        v.push_back(burst(kDatac16, buildCarCtl(poll(2, 1, 1, 9.f, 1, true)), 0.3, seed));
        std::string longText;
        for (int i = 0; i < 12; i++) longText += "QAM16C2 block, line " + std::to_string(i) + ": fast rung of the ladder. ";
        longText += "\n";
        const TxBlock b1 = makeBlock(1, bytesOf(longText), seed, P);
        v.push_back(burst(kQam16c2, dataFrame(kQam16c2, b1, 0, b1.K, 0, 2, P), 1.0, seed));
        v.push_back(burst(kDatac16, buildCarCtl(poll(3, 5, 1, 14.f, 2, false)), 1.0, seed, kCalleeCfo));
        // and on DATAC17 (block 2)
        std::string t17;
        for (int i = 0; i < 8; i++) t17 += "DATAC17 block, line " + std::to_string(i) + ". ";
        t17 += "\n";
        const TxBlock b2 = makeBlock(2, bytesOf(t17), seed, P);
        v.push_back(burst(kDatac17, dataFrame(kDatac17, b2, 0, b2.K, 0, 3, P), 1.0, seed));
        v.push_back(burst(kDatac16, buildCarCtl(poll(4, 5, 1, 14.f, 3, false)), 1.0, seed, kCalleeCfo));
        v.push_back(burst(kDatac16, buildDisconnect(kSid, 13.f), 1.5));
    }
    const std::string msg = "LY2ONA: broadcast test from the OnAir generator";
    v.push_back(burst(kDatac3, buildBcastMessage((const uint8_t*)msg.data(), msg.size(), (size_t)geometry(kDatac3).payloadBytes), 1.0));
    addFile(v);
    v.back().gapSec = 2.0;
    return v;
}

const char* fadingName(int f) {
    static const char* n[kNumFadings] = {"none", "CCIR good", "CCIR moderate", "CCIR poor", "ITU-R low latitude moderate", "flutter"};
    return n[std::max(0, std::min(kNumFadings - 1, f))];
}

void applyChannel(std::vector<cf>& x, const ChannelConfig& ch) {
    // sample clock: the sender's samples are 1 / (8000 (1 + ppm)) apart; windowed sinc interpolation
    if (std::fabs(ch.sroPpm) > 1e-6) {
        const double r = 1.0 + ch.sroPpm * 1e-6;
        const size_t n = (size_t)std::floor((double)x.size() / r);
        std::vector<cf> y(n);
        constexpr int H = 8;
        for (size_t i = 0; i < n; i++) {
            const double u = (double)i * r;
            const int64_t i0 = (int64_t)std::floor(u);
            const double fr = u - (double)i0;
            std::complex<double> acc(0, 0);
            for (int k = -H + 1; k <= H; k++) {
                const int64_t j = i0 + k;
                if (j < 0 || j >= (int64_t)x.size()) continue;
                const double t = k - fr;
                const double s = std::fabs(t) < 1e-9 ? 1.0 : std::sin(kPi * t) / (kPi * t);
                const double w = 0.5 + 0.5 * std::cos(kPi * t / H);
                acc += std::complex<double>(x[(size_t)j].real(), x[(size_t)j].imag()) * (s * w);
            }
            y[i] = cf((float)acc.real(), (float)acc.imag());
        }
        x.swap(y);
    }
    // fading: two equal paths, Rayleigh, Gaussian Doppler spectrum with the spread (2 sigma) of the model
    if (ch.fading > kFadeNone && ch.fading < kNumFadings) {
        static const double kDelayMs[kNumFadings] = {0, 0.5, 1.0, 2.0, 2.0, 0.5};
        static const double kSpreadHz[kNumFadings] = {0, 0.1, 0.5, 1.0, 1.5, 10.0};
        const double fd = kSpreadHz[ch.fading];
        const int delay = (int)std::lround(kDelayMs[ch.fading] * 1e-3 * kFs);
        const double fr = std::max(10.0, 20.0 * fd);                 // rate of the tap processes
        const double sigK = fr / (2 * kPi * (fd / 2));               // the Gaussian filter in samples of fr
        const int half = (int)std::ceil(3.5 * sigK);
        std::vector<double> h((size_t)(2 * half + 1));
        double hs = 0;
        for (int k = -half; k <= half; k++) { h[(size_t)(k + half)] = std::exp(-0.5 * k * k / (sigK * sigK)); hs += h[(size_t)(k + half)] * h[(size_t)(k + half)]; }
        for (double& v : h) v /= std::sqrt(hs);
        const size_t nTap = (size_t)std::ceil((double)x.size() / kFs * fr) + 2;
        PortableRng rng(ch.seed * 7919 + 13);
        std::vector<std::complex<double>> g[2];
        for (int p = 0; p < 2; p++) {
            std::vector<std::complex<double>> w(nTap + h.size());
            for (auto& z : w) z = std::complex<double>(rng.gauss(), rng.gauss()) * std::sqrt(0.25);   // each path power 1/2
            g[p].assign(nTap, 0);
            for (size_t i = 0; i < nTap; i++) for (size_t k = 0; k < h.size(); k++) g[p][i] += h[k] * w[i + k];
        }
        std::vector<cf> y(x.size());
        for (size_t i = 0; i < x.size(); i++) {
            const double u = (double)i / kFs * fr;
            const size_t i0 = std::min(nTap - 2, (size_t)u);
            const double a = u - (double)i0;
            const std::complex<double> g0 = g[0][i0] * (1 - a) + g[0][i0 + 1] * a, g1 = g[1][i0] * (1 - a) + g[1][i0 + 1] * a;
            std::complex<double> v = g0 * std::complex<double>(x[i].real(), x[i].imag());
            if (i >= (size_t)delay) v += g1 * std::complex<double>(x[i - (size_t)delay].real(), x[i - (size_t)delay].imag());
            y[i] = cf((float)v.real(), (float)v.imag());
        }
        x.swap(y);
    }
    if (std::fabs(ch.cfoHz) > 1e-9) {
        for (size_t i = 0; i < x.size(); i++) x[i] *= std::polar(1.f, (float)std::fmod(2 * kPi * ch.cfoHz * (double)i / kFs, 2 * kPi));
    }
    if (ch.snrDb < 99) {
        double ps = ch.refPower;
        if (ps <= 0) {
            double s = 0;
            size_t n = 0;
            for (const cf& v : x) if (std::norm(v) > 0) { s += std::norm(v); n++; }
            ps = n ? s / (double)n : 1.0;
        }
        const double n0 = ps / (std::pow(10.0, ch.snrDb / 10) * 3000.0);
        const double sigma = std::sqrt(n0 * kFs / 2);
        PortableRng rng(ch.seed * 104729 + 1);
        for (cf& v : x) v += cf((float)(sigma * rng.gauss()), (float)(sigma * rng.gauss()));
    }
}

std::vector<cf> renderScript(const std::vector<GenBurst>& script, const ChannelConfig& ch, double leadSec) {
    std::vector<cf> x((size_t)std::llround(leadSec * kFs), cf(0, 0));
    double ps = 0;
    size_t pn = 0;
    for (const GenBurst& b : script) {
        std::vector<cf> s = b.mode >= kNumOfdmModes ? modulateMfskBurst(b.mode, b.frame) : modulateBurst(b.mode, b.frame);
        for (size_t i = 0; i < s.size(); i++) {
            if (b.cfoHz != 0) s[i] *= std::polar(1.f, (float)std::fmod(2 * kPi * b.cfoHz * (double)i / kFs, 2 * kPi));
            ps += std::norm(s[i]);
        }
        pn += s.size();
        x.insert(x.end(), s.begin(), s.end());
        x.insert(x.end(), (size_t)std::llround(b.gapSec * kFs), cf(0, 0));
    }
    ChannelConfig c = ch;
    if (c.refPower <= 0 && pn) c.refPower = ps / (double)pn;   // the SNR of the bursts, before fading (the fading has unit mean power)
    applyChannel(x, c);
    return x;
}

} // namespace mercury

// ------------------------------------------------------------------------------------------- the test source

namespace {
using dect2::math::kPi;
using mercury::cf;

class MercurySynth : public ModeSynth {
public:
    MercurySynth(const SynthConfig& s, double rate) : rate_(rate), noise_(11) {
        using namespace mercury;
        const int which = std::max(0, std::min(4, s.modeOpt[0]));
        ChannelConfig ch;
        ch.fading = std::max(0, std::min(kNumFadings - 1, s.modeOpt[2]));
        ch.sroPpm = s.sroPpm;
        ch.seed = 5;
        const std::vector<cf> a = renderScript(demoScript(which, s.modeOpt[1]), ch, 0.6);
        // 8 kHz -> 48 kHz (windowed sinc, 6 phases); the radio rate is reached by linear interpolation from there
        constexpr int U = 6, H = 12;
        up_.assign(a.size() * U, cf(0, 0));
        double ps = 0;
        size_t pn = 0;
        for (size_t i = 0; i < up_.size(); i++) {
            const double u = (double)i / U;
            const int64_t i0 = (int64_t)std::floor(u);
            const double fr = u - (double)i0;
            std::complex<double> acc(0, 0);
            for (int k = -H + 1; k <= H; k++) {
                const int64_t j = i0 + k;
                if (j < 0 || j >= (int64_t)a.size()) continue;
                const double t = k - fr;
                const double sn = std::fabs(t) < 1e-9 ? 1.0 : std::sin(kPi * t) / (kPi * t);
                const double w = 0.5 + 0.5 * std::cos(kPi * t / H);
                acc += std::complex<double>(a[(size_t)j].real(), a[(size_t)j].imag()) * (sn * w);
            }
            up_[i] = cf((float)acc.real(), (float)acc.imag());
            if (std::norm(up_[i]) > 1e-12f) { ps += std::norm(up_[i]); pn++; }
        }
        ps = pn ? ps / (double)pn : 1.0;
        // level: the noise is 0.05 per component at the radio rate; the signal gets the SNR in 3 kHz, or 0.3 rms with no noise
        double amp;
        if (s.snrDb >= 99) { sigma_ = 0; amp = 0.3 / std::sqrt(ps); }
        else {
            sigma_ = 0.05;
            const double n0 = 2 * sigma_ * sigma_ / rate;
            amp = std::sqrt(std::pow(10.0, s.snrDb / 10) * n0 * 3000.0 / ps);
        }
        for (cf& v : up_) v *= (float)amp;
        step_ = 48000.0 / rate;
        rot_ = std::polar(1.0, 2 * kPi * (-mercuryTuning().tuneOffsetHz + s.cfoHz) / rate);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const double len = (double)up_.size();
        for (size_t i = 0; i < n; i++) {
            const size_t i0 = (size_t)pos_;
            const double a = pos_ - (double)i0;
            const cf v0 = up_[i0], v1 = up_[(i0 + 1) % up_.size()];
            const std::complex<double> z = std::complex<double>(v0.real() + (v1.real() - v0.real()) * a, v0.imag() + (v1.imag() - v0.imag()) * a) * car_;
            out[i] = cf32((float)z.real(), (float)z.imag());
            car_ *= rot_;
            if ((++cnt_ & 1023) == 0) car_ /= std::abs(car_);
            pos_ += step_;
            if (pos_ >= len) pos_ -= len;
        }
        if (sigma_ > 0) noise_.add(out, n, (float)sigma_);
    }
private:
    double rate_;
    std::vector<cf> up_;
    double pos_ = 0, step_ = 1, sigma_ = 0;
    std::complex<double> car_{1, 0}, rot_{1, 0};
    uint32_t cnt_ = 0;
    genutil::NoiseSource noise_;
};
} // namespace

std::unique_ptr<ModeSynth> makeMercurySynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<MercurySynth>(cfg, sampleRate);
}

} // namespace dect2
