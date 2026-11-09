// See wxsat_gen.h. Formats from SatDump (GPL-3.0, Copyright Aang23 and contributors); code written for OnAir.
#include "dect2/wxsat/wxsat_gen.h"
#include "dect2/wxsat/wxsat_proto.h"
#include "dect2/common/mathconst.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/rrc.h"
#include "dect2/elektro/elektro_ggak.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/meteor/meteor_proto.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

namespace dect2 {

namespace {
using math::kPi;
constexpr int kSpan = 8, kTabRes = 256;
}

struct WxsatGenerator::Impl {
    WxsatGenConfig c;
    int P;
    WxProfileInfo info;
    double rs = 1;
    int syms = 1;                 // symbols summed (1: real / phase, 2: I, Q)
    std::vector<float> tab;
    std::vector<float> I, Q;
    int64_t symN = 0;
    double outT = 0, sTime = 0, tstep = 0;
    double phase = 0, freq = 0, amp = 1;
    double sigma = 0, scale = 1;
    std::deque<uint8_t> bits;
    std::mt19937 rng;
    dect2::normal_distribution<float> nd{0.f, 1.f};
    hrpt::CcsdsEncoder enc;
    std::unique_ptr<GgakGenerator> ggak;
    std::unique_ptr<ModeSynth> dvb;      // HimawariCast, GeoNetCast: the DVB-S2 chain with the test carousel in its transport stream
    int msgLine = 0, msgSeq = 0;
    int line = 0;
    uint32_t counter = 0, vcCounter = 0;
    int nrz = 0, seq = 0, minor = 0, hirsEl = 0;
    std::vector<uint8_t> meteorStream, rdasIr, rdasVis, awsStream, awsStarts;
    int rdasSeg = 0;
    // APT
    double audioPhase = 0, audioT = 0, fmPhase = 0, audioRate = 0;

    explicit Impl(const WxsatGenConfig& cfg) : c(cfg), P(cfg.profile), info(wxProfile(cfg.profile)), rng(cfg.seed) {
        rs = info.symbolRate;
        if (P == kWxArktikaGgak) {
            GgakGenConfig g;
            g.rate = c.rate; g.snrDb = c.snrDb; g.cfoHz = c.cfoHz; g.amplitude = c.amp;
            ggak = std::make_unique<GgakGenerator>(g);
            return;
        }
        if (P == kWxHimawariCast || P == kWxGeoNetCast) {
            SynthConfig sc;
            sc.modeOpt[0] = 0; sc.modeOpt[3] = 2; sc.modeOpt[4] = 0; sc.modeOpt[5] = 1;     // DVB-S2, roll-off 0.20, normal frames, pilots: the pipelines' settings
            sc.modeOpt[1] = P == kWxHimawariCast ? 0 : 1;                                   // QPSK 2/3 (MODCOD 5), 8PSK 5/6 (MODCOD 15)
            sc.modeOpt[2] = P == kWxHimawariCast ? 6 : 9;
            sc.modeVal[0] = c.fileSymbolRate > 0 ? c.fileSymbolRate : info.symbolRate;
            sc.snrDb = c.snrDb; sc.cfoHz = c.cfoHz; sc.sroPpm = c.clockPpm;
            dvb = makeDvbsSynth(sc, c.rate, wxsat::filecastTestTs(P));
            return;
        }
        rng.seed(cfg.seed);
        syms = (P == kWxAws || P == kWxMsgRaw) ? 2 : 1;
        amp = c.amp;
        if (P == kWxApt) { audioRate = c.rate; if (c.snrDb < 99) sigma = amp / std::sqrt(2 * std::pow(10.0, c.snrDb / 10)) * std::sqrt(c.rate / 40000.0); return; }
        const double alpha = (P == kWxNoaaHrpt || P == kWxMeteorHrpt || P == kWxNoaaDsb) ? 1.0 : P == kWxAws ? 0.45 : P == kWxMsgRaw ? 0.35 : 0.5;
        tab.resize((size_t)(2 * kSpan * kTabRes + 1));
        double e = 0;
        for (size_t i = 0; i < tab.size(); i++) {
            const double t = (double)i / kTabRes - kSpan;
            const double w = 0.5 + 0.5 * std::cos(kPi * t / kSpan);
            tab[i] = (float)(dsp::rrc(t, alpha) * w);
            e += (double)tab[i] * tab[i] / kTabRes;
        }
        // Es/N0 in the symbol rate: noise per complex sample = n0 * rate
        scale = amp / std::sqrt(syms == 2 ? 2 * e : e);
        if (P == kWxNoaaHrpt || P == kWxMeteorHrpt || P == kWxNoaaDsb) {
            scale = 1.0 / std::sqrt(e) * (c.pmIndexDeg * kPi / 180.0);
        }
        if (c.snrDb < 99) { const double n0 = amp * amp / (std::pow(10.0, c.snrDb / 10) * rs); sigma = std::sqrt(n0 * c.rate / 2); }
        I.assign(64, 0); Q.assign(64, 0);
        tstep = rs * (1 + c.clockPpm * 1e-6) / c.rate;
        phase = c.phaseDeg * kPi / 180;
    }

    float h(double t) const {
        const double u = (t + kSpan) * kTabRes;
        if (u <= 0 || u >= (double)tab.size() - 1) return 0;
        const size_t i = (size_t)u;
        const float f = (float)(u - (double)i);
        return tab[i] + f * (tab[i + 1] - tab[i]);
    }

    void pushBytes(const uint8_t* b, size_t n, int bitsTotal = -1) {
        const size_t count = bitsTotal >= 0 ? (size_t)bitsTotal : n * 8;
        for (size_t i = 0; i < count; i++) bits.push_back(b[i >> 3] >> (7 - (i & 7)) & 1);
    }

    void nextFrame() {
        switch (P) {
        case kWxNoaaHrpt: {
            hrpt::Line l;
            l.day = 20000 & 0x1FF; l.ms = (uint32_t)(36000000 + std::llround(line * hrpt::kLineMs));
            for (int ch = 0; ch < kHrptChannels; ch++) for (int x = 0; x < kHrptWidth; x++) l.px[(size_t)ch * kHrptWidth + (size_t)x] = wxsat::wxTestPixel(ch, x, line);
            uint16_t w[hrpt::kNoaaWords];
            hrpt::encodeNoaaFrame(l, w);
            const int f = line % 3 + 1;
            w[6] = (uint16_t)((w[6] & ~0x78u) | 15 << 3);        // spacecraft 15: NOAA-19
            w[6] = (uint16_t)((w[6] & ~(3u << 7)) | f << 7);
            if (f == 1) for (int i = 0; i < 5; i++) { uint8_t t[wxsat::kDsbBytes]; tipBlock(t); for (int j = 0; j < wxsat::kDsbBytes; j++) w[104 * (i + 1) + j - 1] = (uint16_t)(t[j] << 2); }
            for (int i = 0; i < hrpt::kNoaaWords; i++)
                for (int k = 9; k >= 0; k--) { const bool b = (w[i] >> k & 1) != 0; bits.push_back(b ? 1 : 0); bits.push_back(b ? 0 : 1); }
            line = (line + 1) % std::max(1, c.lines);
            break;
        }
        case kWxNoaaGac: {
            uint8_t f[wxsat::kGacBytes];
            wxsat::gacBuildFrame(line, 15, f);
            pushBytes(f, wxsat::kGacBytes, wxsat::kGacBits);
            line = (line + 1) % std::max(1, c.lines);
            break;
        }
        case kWxNoaaDsb: {
            uint8_t t[wxsat::kDsbBytes];
            tipBlock(t);
            for (int i = 0; i < wxsat::kDsbBytes * 8; i++) { const int b = t[i >> 3] >> (7 - (i & 7)) & 1; bits.push_back((uint8_t)b); bits.push_back((uint8_t)!b); }
            break;
        }
        case kWxMeteorHrpt: {
            while (meteorStream.size() < wxsat::kMeteorPayload) { std::vector<uint8_t> f; wxsat::msuMrBuildFrame(line, f); line = (line + 1) % std::max(1, c.lines); meteorStream.insert(meteorStream.end(), f.begin(), f.end()); }
            uint8_t cadu[wxsat::kCadu];
            wxsat::meteorBuildCadu(meteorStream.data(), counter++, cadu);
            meteorStream.erase(meteorStream.begin(), meteorStream.begin() + wxsat::kMeteorPayload);
            for (int i = 0; i < wxsat::kCadu * 8; i++) { const int b = cadu[i >> 3] >> (7 - (i & 7)) & 1; bits.push_back((uint8_t)b); bits.push_back((uint8_t)!b); }
            break;
        }
        case kWxElektroRdas: case kWxArktikaRdas: {
            bool useIr = (counter % 3) != 0;
            if (useIr) {
                while (rdasIr.size() < wxsat::kRdasPayload) { std::vector<uint8_t> f; wxsat::msuGsIrBuild(line, rdasSeg, f); rdasSeg += 2; if (rdasSeg >= 36) { rdasSeg = 0; line = (line + 1) % std::max(1, c.lines); } rdasIr.insert(rdasIr.end(), f.begin(), f.end()); }
                makeRdasCadu(4, rdasIr);
            } else {
                while (rdasVis.size() < wxsat::kRdasPayload) { std::vector<uint8_t> f; wxsat::msuGsVisBuild(line * 4 % 17200, f); rdasVis.insert(rdasVis.end(), f.begin(), f.end()); }
                makeRdasCadu(2, rdasVis);
            }
            break;
        }
        case kWxMsgRaw: {
            while (awsStream.size() < wxsat::kAwsVcdu - 8) {
                const size_t before = awsStream.size();
                wxsat::seviriBuildStep(323, msgLine, msgSeq, awsStream);
                msgLine += 3;
                awsStarts.resize(awsStream.size(), 0);
                for (size_t k = 0; k < 16; k++) awsStarts[before + k * (wxsat::kSeviriPkt + 6)] = 1;
            }
            uint8_t v[wxsat::kAwsVcdu], cadu[wxsat::kAwsCadu];
            wxsat::awsBuildVcdu(0, vcCounter++, awsStream, awsStarts, v, 323);
            wxsat::awsEncodeCadu(v, cadu);
            for (int i = 0; i < wxsat::kAwsCadu * 8; i++) { uint8_t a, b; enc.push(cadu[i >> 3] >> (7 - (i & 7)) & 1, a, b); bits.push_back(a); bits.push_back(b); }
            break;
        }
        case kWxAws: {
            while (awsStream.size() < wxsat::kAwsVcdu - 8) { size_t before = awsStream.size(); wxsat::mwrBuildPackets(line, 3, seq, awsStream); line = (line + 1) % std::max(1, c.lines); awsStarts.resize(awsStream.size(), 0); awsStarts[before] = 1; }
            uint8_t v[wxsat::kAwsVcdu], cadu[wxsat::kAwsCadu];
            wxsat::awsBuildVcdu(3, vcCounter++, awsStream, awsStarts, v);
            wxsat::awsEncodeCadu(v, cadu);
            for (int i = 0; i < wxsat::kAwsCadu * 8; i++) { uint8_t a, b; enc.push(cadu[i >> 3] >> (7 - (i & 7)) & 1, a, b); bits.push_back(a); bits.push_back(b); }
            break;
        }
        default: break;
        }
    }

    void tipBlock(uint8_t t[wxsat::kDsbBytes]) {
        uint16_t h2[wxsat::kHirsChannels];
        for (int k = 0; k < wxsat::kHirsChannels; k++) h2[k] = (uint16_t)(0x1000 | wxsat::wxTestPixel(k, hirsEl, line));
        wxsat::encodeHirsTip(t, 15, minor, hirsEl, h2);
        minor = (minor + 1) % 320;
        hirsEl = (hirsEl + 1) % wxsat::kHirsWidth;
        if (hirsEl == 0 && P == kWxNoaaDsb) line = (line + 1) % std::max(1, c.lines);
    }

    void makeRdasCadu(int vcid, std::vector<uint8_t>& st) {
        uint8_t cadu[wxsat::kCadu];
        wxsat::rdasBuildCadu(vcid, counter++, st.data(), cadu);
        st.erase(st.begin(), st.begin() + wxsat::kRdasPayload);
        for (int i = 0; i < wxsat::kCadu * 8; i++) { const int b = cadu[i >> 3] >> (7 - (i & 7)) & 1; nrz ^= b; bits.push_back((uint8_t)nrz); }
    }

    void nextSym() {
        if (syms == 2) {
            while (bits.size() < 2) nextFrame();
            const uint8_t a = bits.front(); bits.pop_front();
            const uint8_t b = bits.front(); bits.pop_front();
            I[(size_t)(symN & 63)] = a ? 1.f : -1.f; Q[(size_t)(symN & 63)] = b ? 1.f : -1.f;
        } else {
            while (bits.empty()) nextFrame();
            I[(size_t)(symN & 63)] = bits.front() ? 1.f : -1.f; bits.pop_front(); Q[(size_t)(symN & 63)] = 0;
        }
        symN++;
    }

    double aptWord(int lineNo, int word) const {
        // format: sync A (39), space (47), image A (909), telemetry (45), sync B (39), space B (47), image B (909), telemetry B (45)
        static const int patA[39] = {0,0,0,0, 0,0,1,1, 0,0,1,1, 0,0,1,1, 0,0,1,1, 0,0,1,1, 0,0,1,1, 0,0,1,1, 0,0,0,0,0,0,0};
        if (word < 39) return patA[word] ? 0.95 : 0.05;
        if (word < 86) return 0.5;
        if (word < 995) return wxsat::wxTestPixel(0, word - 86, lineNo) / 1023.0;
        if (word < 1040) return 0.5;
        if (word < 1079) return (word - 1040) % 5 >= 2 ? 0.95 : 0.05;
        if (word < 1126) return 0.5;
        if (word < 2035) return wxsat::wxTestPixel(1, word - 1126, lineNo) / 1023.0;
        return 0.5;
    }

    void genApt(cf32* out, size_t n) {
        const double wStep = 2 * kPi * 2400.0 / audioRate;
        for (size_t i = 0; i < n; i++) {
            const double words = audioT * 4160.0;
            const long long w = (long long)words;
            const int lineNo = (int)(w / 2080) % std::max(1, c.lines), word = (int)(w % 2080);
            const double env = 0.1 + 0.85 * aptWord(lineNo, word);
            const double m = env * std::sin(audioPhase);
            audioPhase += wStep; if (audioPhase > 2 * kPi) audioPhase -= 2 * kPi;
            audioT += (1 + c.clockPpm * 1e-6) / audioRate;
            fmPhase += 2 * kPi * 17000.0 * m / audioRate + 2 * kPi * (c.cfoHz + freq) / audioRate;
            freq += c.driftHzPerS / audioRate;
            if (fmPhase > 2 * kPi) fmPhase -= 2 * kPi; else if (fmPhase < -2 * kPi) fmPhase += 2 * kPi;
            cf32 v((float)(amp * std::cos(fmPhase)), (float)(amp * std::sin(fmPhase)));
            if (sigma > 0) v += cf32((float)sigma * nd(rng), (float)sigma * nd(rng));
            out[i] = v;
        }
    }

    void genPsk(cf32* out, size_t n) {
        const bool pm = P == kWxNoaaHrpt || P == kWxMeteorHrpt || P == kWxNoaaDsb;
        for (size_t i = 0; i < n; i++) {
            while (symN - kSpan - 2 < (int64_t)sTime) nextSym();
            const int64_t k0 = (int64_t)std::floor(sTime);
            float a = 0, b = 0;
            for (int64_t k = k0 - kSpan + 1; k <= k0 + kSpan; k++) {
                if (k < 0) continue;
                const float hv = h(sTime - (double)k);
                a += I[(size_t)(k & 63)] * hv;
                if (syms == 2) b += Q[(size_t)(k & 63)] * hv;
            }
            std::complex<double> s;
            if (pm) { const double ph = (double)a * scale; s = std::polar(amp, ph); }
            else s = std::complex<double>((double)a * scale, (double)b * scale);
            s *= std::polar(1.0, phase);
            phase += 2 * kPi * (c.cfoHz + freq) / c.rate;
            freq += c.driftHzPerS / c.rate;
            if (phase > 2 * kPi) phase -= 2 * kPi; else if (phase < -2 * kPi) phase += 2 * kPi;
            cf32 v((float)s.real(), (float)s.imag());
            if (sigma > 0) v += cf32((float)sigma * nd(rng), (float)sigma * nd(rng));
            out[i] = v;
            sTime += tstep;
        }
    }

    void generate(cf32* out, size_t n) {
        if (P == kWxArktikaGgak) { ggak->generate(out, n); return; }
        if (dvb) { dvb->generate(out, n); return; }
        if (P == kWxApt) { genApt(out, n); return; }
        genPsk(out, n);
    }
};

WxsatGenerator::WxsatGenerator(const WxsatGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
WxsatGenerator::~WxsatGenerator() = default;
void WxsatGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }

namespace {
class Synth : public ModeSynth {
public:
    Synth(const WxsatGenConfig& c) : gen_(c), r_(c.rate) {}
    double sampleRate() const override { return r_; }
    void generate(cf32* out, size_t n) override { gen_.generate(out, n); }
private:
    WxsatGenerator gen_;
    double r_;
};
}

std::unique_ptr<ModeSynth> makeWxsatSynth(const SynthConfig& cfg, double sampleRate, int profile) {
    WxsatGenConfig c;
    c.profile = profile; c.rate = sampleRate;
    c.snrDb = cfg.snrDb; c.cfoHz = cfg.cfoHz;
    return std::make_unique<Synth>(c);
}

} // namespace dect2
