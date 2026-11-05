// FY-3 AHRPT / MPT test signal (see fy3_gen.h).
#include "dect2/fy3/fy3_gen.h"
#include "dect2/fy3/fy3_proto.h"
#include "dect2/fy3/fy3_rx.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/rrc.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/meteor/meteor_proto.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr int kSpan = 8, kTabRes = 256;
using dect2::dsp::rrc;
}

struct Fy3Generator::Impl {
    Fy3GenConfig c;
    double rs = 2.6e6, alpha = 0.5;
    int scid = 51, inst = fy3::Virr, vcid = 5;
    std::deque<uint8_t> stream;           // instrument bytes waiting for VCDUs
    uint32_t counter[64] = {};
    int64_t line = 0, segment = 0;
    double credit = 0;
    std::deque<float> symI, symQ;
    fy3::DiffEncoder denc;
    hrpt::CcsdsEncoder enc[2];
    float I[64], Q[64];
    int64_t symN = 0, outN = 0;
    std::vector<float> tab;
    double scale = 1, sigma = 0;
    std::mt19937 rng;
    dect2::normal_distribution<float> nd{0.f, 1.f};
    std::complex<double> rot{1, 0}, rstep{1, 0};
    fy3::BitWriter mersiBits;

    explicit Impl(const Fy3GenConfig& cfg) : c(cfg), rng(cfg.seed) {
        rs = c.symbolRate > 0 ? c.symbolRate : (c.product ? 18.7e6 : 2.6e6);
        alpha = c.rolloff > 0 ? c.rolloff : 0.5;
        int ns; const fy3::Sat* ss = fy3::sats(ns);
        scid = ss[std::clamp(c.sat, 0, ns - 1)].scid;
        if (c.inst) inst = c.inst;
        else if (scid >= 49 && scid <= 51) inst = c.product ? fy3::Mersi1 : fy3::Virr;   // FY-3A / B / C: VIRR on the AHRPT, MERSI on the MPT
        else inst = scid == 52 ? fy3::Mersi2 : scid == 56 ? fy3::MersiLL : scid == 64 ? fy3::MersiRM : fy3::Mersi3;
        vcid = inst == fy3::Virr ? 5 : inst == fy3::MersiRM ? 35 : inst == fy3::Mwri ? 10 : inst == fy3::Mwri2 ? 42 : 3;
        tab.resize((size_t)(2 * kSpan * kTabRes + 1));
        double e = 0;
        for (size_t i = 0; i < tab.size(); i++) {
            const double t = (double)i / kTabRes - kSpan;
            const double w = 0.5 + 0.5 * std::cos(kPi * t / kSpan);
            tab[i] = (float)(rrc(t, alpha) * w);
            e += (double)tab[i] * tab[i] / kTabRes;
        }
        scale = c.amp / std::sqrt(2 * e);
        if (c.snrDb < 99) { const double n0 = c.amp * c.amp / (std::pow(10.0, c.snrDb / 10) * rs); sigma = std::sqrt(n0 * c.rate / 2); }
        rot = std::polar(1.0, c.phaseDeg * kPi / 180);
        rstep = std::polar(1.0, 2 * kPi * c.cfoHz / c.rate);
        std::fill(I, I + 64, 0.f); std::fill(Q, Q + 64, 0.f);
    }

    // instrument data
    void addInstrumentData() {
        if (inst == fy3::Virr) {
            fy3::VirrLine l;
            l.days = 3000; l.ms = (uint32_t)(36000000 + std::llround((double)line * fy3::kVirrLineMs));
            for (int ch = 0; ch < fy3::kVirrChannels; ch++)
                for (int x = 0; x < fy3::kVirrWidth; x++) l.px[(size_t)ch * fy3::kVirrWidth + (size_t)x] = fy3::testPixel(ch, x, (int)line);
            std::vector<uint8_t> f(fy3::kVirrFrameBytes + 8);
            fy3::encodeVirrFrame(l, f.data());
            stream.insert(stream.end(), f.begin(), f.begin() + fy3::kVirrFrameBytes);
            line++;
        } else if (inst == fy3::Mwri || inst == fy3::Mwri2) {
            const fy3::MwriCfg& mc = fy3::mwriCfg(inst);
            fy3::MwriLine l;
            l.time = 1.7e9 + 36000.0 + (double)line * mc.periodSec;
            l.px.resize((size_t)mc.channels * (size_t)mc.width);
            for (int ch = 0; ch < mc.channels; ch++)
                for (int x = 0; x < mc.width; x++) l.px[(size_t)ch * (size_t)mc.width + (size_t)x] = (uint16_t)(fy3::testPixel(ch, x * 4, (int)line) << 6);
            std::vector<uint8_t> f;
            fy3::encodeMwriFrame(inst, l, f);
            stream.insert(stream.end(), f.begin(), f.end());
            line++;
        } else {
            fy3::BitWriter bw;
            const double t = 1.7e9 + 36000.0 + (double)segment * 1.5;
            const int cap = fy3::mersiCfg(inst).ch250;
            (void)cap;
            fy3::encodeMersiSegment(inst, (int)segment, t, [](int ch, int x, int y) { return (uint16_t)(fy3::testPixel(ch, x, y) << 2); }, bw);
            // carry the bits that did not fill a byte into the next segment: keep it simple, pad to a byte (the reader re-syncs on the next head frame)
            while (bw.nbit) bw.put(0, 1);
            stream.insert(stream.end(), bw.bytes.begin(), bw.bytes.end());
            segment++;
        }
    }

    void nextFrame() {
        fy3::Cadu cd;
        cd.scid = scid;
        credit += c.duty;
        if (credit >= 1.0) {
            credit -= 1.0;
            while (stream.size() < 882) addInstrumentData();
            cd.vcid = vcid;
            for (int i = 0; i < 882; i++) { cd.data[i] = stream.front(); stream.pop_front(); }
        } else {
            cd.vcid = 63;
            for (int i = 0; i < 882; i++) cd.data[i] = (uint8_t)(i & 1 ? 0x55 : 0xAA);
        }
        cd.counter = counter[cd.vcid]++ & 0xFFFFFF;
        uint8_t cadu[meteor::kCaduBytes];
        fy3::encodeVcdu(cd, cadu);
        // data pairs -> rail bits -> codes
        for (int i = 0; i < meteor::kCaduBytes * 8; i += 2) {
            const int hi = cadu[i >> 3] >> (7 - (i & 7)) & 1, lo = cadu[(i + 1) >> 3] >> (7 - ((i + 1) & 7)) & 1;
            int x, y;
            denc.push(hi, lo, x, y);
            if (c.swapIq) std::swap(x, y);
            uint8_t a0, b0, a1, b1;
            enc[0].push(x, a0, b0);
            enc[1].push(y, a1, b1);
            symI.push_back(a0 ? 1.f : -1.f); symQ.push_back(a1 ? 1.f : -1.f);
            symI.push_back(b0 ? 1.f : -1.f); symQ.push_back(b1 ? 1.f : -1.f);
        }
    }

    void nextSymbol() {
        while (symI.empty()) nextFrame();
        I[(size_t)(symN & 63)] = symI.front(); symI.pop_front();
        Q[(size_t)(symN & 63)] = (c.conjugate ? -1.f : 1.f) * symQ.front(); symQ.pop_front();
        symN++;
    }

    float h(double t) const {
        const double u = (t + kSpan) * kTabRes;
        if (u <= 0 || u >= (double)tab.size() - 1) return 0;
        const size_t i = (size_t)u;
        const float f = (float)(u - (double)i);
        return tab[i] * (1 - f) + tab[i + 1] * f;
    }

    void generate(cf32* out, size_t n) {
        const double r = rs * (1 + c.clockPpm * 1e-6) / c.rate;
        for (size_t i = 0; i < n; i++) {
            const double t = (double)outN * r;
            outN++;
            const int64_t k0 = (int64_t)std::floor(t);
            while (symN <= k0 + kSpan + 1) nextSymbol();
            float re = 0, im = 0;
            for (int64_t k = k0 - kSpan; k <= k0 + kSpan + 1; k++) {
                if (k < 0 || k < symN - 64) continue;
                const float hh = h(t - (double)k);
                re += I[(size_t)(k & 63)] * hh;
                im += Q[(size_t)(k & 63)] * hh;
            }
            std::complex<double> s((double)re * scale, (double)im * scale);
            s *= rot;
            rot *= rstep;
            if ((outN & 4095) == 0) {
                rot /= std::abs(rot);
                if (c.driftHzPerS != 0) rstep = std::polar(1.0, 2 * kPi * (c.cfoHz + c.driftHzPerS * (double)outN / c.rate) / c.rate);
            }
            if (sigma > 0) s += std::complex<double>{sigma * nd(rng), sigma * nd(rng)};
            out[i] = cf32((float)s.real(), (float)s.imag());
        }
    }
};

Fy3Generator::Fy3Generator(const Fy3GenConfig& c) : p_(std::make_unique<Impl>(c)) {}
Fy3Generator::~Fy3Generator() = default;
void Fy3Generator::generate(cf32* out, size_t n) { p_->generate(out, n); }
double Fy3Generator::symbolRate() const { return p_->rs; }

namespace {
class Fy3Synth : public ModeSynth {
public:
    Fy3Synth(const SynthConfig& s, double rate, int product) : rate_(rate) {
        Fy3GenConfig c;
        c.rate = rate; c.product = product;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.sat = s.modeOpt[0] > 0 ? s.modeOpt[0] : 2;
        c.conjugate = (s.modeOpt[1] & 1) != 0; c.swapIq = (s.modeOpt[1] & 2) != 0;
        c.symbolRate = s.modeVal[0]; c.rolloff = s.modeVal[1];
        gen_ = std::make_unique<Fy3Generator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<Fy3Generator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeFy3Synth(const SynthConfig& cfg, double sampleRate, int product) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<Fy3Synth>(cfg, sampleRate, product);
}

} // namespace dect2
