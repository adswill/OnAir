// See sd3_gen.h. Formats from SatDump (GPL-3.0, Copyright Aang23 and contributors); code written for OnAir.
#include "dect2/sd3/sd3_gen.h"
#include "dect2/sd3/sd3_proto.h"
#include "dect2/common/mathconst.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/rrc.h"
#include "dect2/dvbs/dvbs_tx.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/image/j2k.h"
#include "dect2/meteor/meteor_proto.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

namespace dect2 {
using namespace sd3;

namespace {
using math::kPi;
constexpr int kSpan = 8, kTab = 256;
}

struct Sd3Generator::Impl {
    Sd3GenConfig c;
    int P;
    Sd3Link l;
    double rs = 1, tstep = 0;
    std::vector<float> tab;
    std::deque<float> I, Q, env;
    int64_t base = 0;
    double t = 0, phase = 0, freq = 0, fmPhase = 0, amp = 1, sigma = 0;
    std::mt19937 rng;
    dect2::normal_distribution<float> nd{0.f, 1.f};
    meteor::ConvEncoder enc;
    int nrz = 0, diffSym = 0;
    uint32_t counter = 0;
    int line = 0;
    std::vector<uint8_t> stream;             // S2: CADU bytes waiting
    uint16_t seqGrb = 0;
    std::unique_ptr<dvbs::DvbsTxSource> tx;

    explicit Impl(const Sd3GenConfig& cc) : c(cc), P(std::clamp(cc.profile, 0, (int)kSd3NumProfiles - 1)), l(sd3LinkAt(P, cc.link)), rng(cc.seed) {
        rs = l.sym * (1 + c.clockPpm * 1e-6);
        tstep = rs / c.rate;
        tab.resize((size_t)((2 * kSpan + 1) * kTab));
        double e = 0;
        for (int i = 0; i < (int)tab.size(); i++) {
            const double x = (double)i / kTab - kSpan;
            const double v = dsp::rrc(x, l.rrc);
            tab[(size_t)i] = (float)v;
            if (i % kTab == 0) e += v * v;
        }
        for (auto& v : tab) v /= (float)std::sqrt(e);
        phase = c.phaseDeg * kPi / 180;
        freq = c.cfoHz;
        // Es/N0: noise per dimension in the sample rate
        const double es = c.amp * c.amp;
        const double n0 = es / std::pow(10.0, c.snrDb / 10.0);
        sigma = c.snrDb >= 99 ? 0 : std::sqrt(n0 * (c.rate / rs) / 2);
        amp = c.amp;
        if (P == kSd3Grb || P == kSd3Uhrit) {
            dvbs::DvbsTxConfig t;
            t.standard = 2; t.symbolRate = rs; t.rollOff = l.rrc;
            if (P == kSd3Grb) { t.mod = dvbs::kQpsk; t.rate = 10; t.pilots = false; }   // QPSK 9/10
            else { t.mod = dvbs::k8psk; t.rate = 7; t.pilots = true; }                  // 8PSK 3/4
            t.gseField = [this](size_t bytes) { return s2Bytes(bytes); };
            tx = dvbs::makeDvbsTx(t, nullptr);
            s2Symbols();
        }
    }

    // ---- symbol queue ----
    void pushSym(float a, float b, float e = 1) { I.push_back(a); Q.push_back(b); env.push_back(e); }
    void pushBitsBpsk(int bit, float e = 1) { pushSym(bit ? 1.f : -1.f, 0, e); }

    std::vector<uint8_t> bitsOf(const std::vector<uint8_t>& f, int nbits) const {
        std::vector<uint8_t> b((size_t)nbits);
        for (int i = 0; i < nbits; i++) b[(size_t)i] = (f[(size_t)i >> 3] >> (7 - (i & 7))) & 1;
        return b;
    }

    void sendBits(const std::vector<uint8_t>& bits) {
        // the channel: nrz-m, then the convolutional code, then the constellation (as the receiver undoes them)
        std::vector<uint8_t> line = bits;
        if (l.nrzm && l.mod != kSd3Qpsk && l.mod != kSd3Oqpsk) for (auto& b : line) { nrz ^= b; b = (uint8_t)nrz; }
        else if (l.nrzm && l.conv) for (auto& b : line) { nrz ^= b; b = (uint8_t)nrz; }
        std::vector<uint8_t> coded;
        if (l.conv) {
            coded.reserve(line.size() * 2);
            for (auto b : line) { uint8_t a, x; enc.push(b, a, x); coded.push_back(a); coded.push_back(x); }
        } else coded = line;
        if (l.mod == kSd3Qpsk || l.mod == kSd3Oqpsk) {
            if (l.nrzm && !l.conv) {                            // four-phase differential, the inverse of SatDump's QPSKDiff
                for (size_t i = 0; i + 1 < coded.size(); i += 2) {
                    const int wantA = coded[i], wantB = coded[i + 1];
                    for (int cur = 0; cur < 4; cur++) {
                        const int X1 = diffSym >> 1, Y1 = diffSym & 1, X = cur >> 1, Y = cur & 1;
                        int xo, yo;
                        if ((X ^ Y) == 1) { xo = Y1 ^ Y; yo = X1 ^ X; } else { xo = X1 ^ X; yo = Y1 ^ Y; }
                        if (yo == wantA && xo == wantB) { diffSym = cur; break; }
                    }
                    pushSym((diffSym & 2) ? 1.f : -1.f, (diffSym & 1) ? 1.f : -1.f);
                }
            } else {
                for (size_t i = 0; i + 1 < coded.size(); i += 2) pushSym(coded[i] ? 1.f : -1.f, coded[i + 1] ? 1.f : -1.f);
            }
        } else for (auto b : coded) pushSym(b ? 1.f : -1.f, 0);
    }

    void nextTableFrame() {
        std::vector<uint8_t> f((size_t)(l.frameBits + 7) / 8, 0);
        const int info = infoBytes(l);
        std::uniform_int_distribution<int> byte(0, 255);
        for (int i = 0; i < info; i++) f[4 + (size_t)i] = (uint8_t)byte(rng);
        // an AOS header so that the spacecraft and virtual channel show
        f[4] = 0x40 | (uint8_t)(0x12 >> 2); f[5] = (uint8_t)((0x12 & 3) << 6 | 5);
        f[6] = (uint8_t)(counter >> 16); f[7] = (uint8_t)(counter >> 8); f[8] = (uint8_t)counter;
        counter++;
        encodeFrame(l, f);
        sendBits(bitsOf(f, l.frameBits));
    }

    void nextMdl() {
        std::vector<uint8_t> f(464, 0);
        f[0] = 0x0B; f[1] = 0xEB; f[2] = 0xCC; f[3] = 0x80;
        f[4] = (uint8_t)counter; f[5] = (uint8_t)(counter >> 8); counter++;
        std::uniform_int_distribution<int> byte(0, 255);
        for (int i = 6; i < 464; i++) f[(size_t)i] = (uint8_t)byte(rng);
        for (int i = 4; i < 464; i++) f[(size_t)i] ^= 0xFF;
        sendBits(bitsOf(f, 464 * 8));
    }

    void nextGvar() {
        static const int order[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        std::vector<uint8_t> f((size_t)kGvarBytes);
        for (int id : order) {
            auto pix = [](int ch, int y, int x) -> uint16_t { return (uint16_t)((((x / (ch == 4 ? 80 : 20)) ^ (y / (ch == 4 ? 8 : 2))) * 7 + ch * 31) & 0x3FF); };
            gvarBuildFrame(id, line, pix, 90, f.data());
            auto b = bitsOf(f, kGvarBytes * 8);
            for (auto d : b) { const int cur = (d ^ 1) ^ nrz; nrz = cur; pushSym(cur ? 1.f : -1.f, 0); }
        }
        line = (line + 1) % 1300;
    }

    void burstGap() {
        const int n = (int)std::lround(c.gapSec * rs);
        for (int i = 0; i < n; i++) pushSym(0, 0, 0);
    }

    void preamble(int n) { for (int i = 0; i < n; i++) pushSym(i & 1 ? 1.f : -1.f, 0); }

    void nextCubesat() {
        std::uniform_int_distribution<int> byte(0, 255);
        burstGap();
        preamble(48);
        if (P == kSd3Geoscan) {
            std::vector<uint8_t> f(kGeoscanData + 4);
            f[0] = 0x93; f[1] = 0x0B; f[2] = 0x51; f[3] = 0xDE;
            uint8_t payload[kGeoscanData - 2];
            for (auto& b : payload) b = (uint8_t)byte(rng);
            geoscanBuild(payload, f.data() + 4);
            for (auto b : bitsOf(f, (kGeoscanData + 4) * 8)) pushSym(b ? 1.f : -1.f, 0);
        } else if (P == kSd3Lucky7) {
            std::vector<uint8_t> f(39);
            uint8_t payload[35];
            for (auto& b : payload) b = (uint8_t)byte(rng);
            payload[1] = 0;
            lucky7Build(payload, f.data());
            for (auto b : bitsOf(f, kLucky7Bits)) pushSym(b ? 1.f : -1.f, 0);
        } else if (P == kSd3Spino) {
            std::vector<uint8_t> h(16, 0xAA), body(40);
            for (auto& b : body) b = (uint8_t)byte(rng);
            auto f = spinoBuild(h, body);
            for (auto b : bitsOf(f, (int)f.size() * 8)) pushSym(b ? 1.f : -1.f, 0);
        } else {
            nextTableFrame();
        }
    }

    // ---- S2 payloads
    void addAosPackets(const std::vector<std::vector<uint8_t>>& pkts, int vcid) {
        std::vector<uint8_t> ps;
        std::vector<size_t> starts;
        for (const auto& p : pkts) { starts.push_back(ps.size()); ps.insert(ps.end(), p.begin(), p.end()); }
        size_t pos = 0;
        const size_t caduBytes = kGrbCadu;
        while (pos < ps.size()) {
            size_t at = stream.size();
            stream.resize(at + caduBytes);
            if (P == kSd3Uhrit) buildUhritCadu(0x12, vcid, counter++, ps, pos, &stream[at], starts);
            else buildAosFrame(0x12, vcid, counter++, kGrbMpdu, ps, pos, (int)caduBytes, &stream[at], starts);
        }
    }

    void grbNext() {
        // a CONUS block of ABI channel 13 and a metadata payload, a new block row each time
        const int apid = 0xBC;
        const int w = 64, h = 16;
        std::vector<uint16_t> px((size_t)w * h);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) px[(size_t)y * w + x] = (uint16_t)(((x ^ (y + line)) * 91) & 0xFFF);
        const auto code = j2k::encodeLossless(px.data(), w, h, 12, 3);
        auto pk = grbImagePackets(apid, seqGrb, 123456, 1, code, 0, 0, (uint32_t)(line * h), (uint32_t)(h), (uint32_t)w);
        auto meta = grbGenericPackets(0x580, seqGrb, 123456, std::vector<uint8_t>(40, 0x3C));
        pk.insert(pk.end(), meta.begin(), meta.end());
        addAosPackets(pk, 5);
        line++;
    }

    void uhritNext() {
        // one small xRIT file, split into packets: primary header + annotation, then data
        std::vector<uint8_t> file;
        std::vector<uint8_t> ann = {'I', 'M', 'G', '_', 'T', 'E', 'S', 'T'};
        const uint32_t hdr = 16 + 3 + (uint32_t)ann.size();
        std::vector<uint8_t> xrit = {0, 0, 16, 0, (uint8_t)(hdr >> 24), (uint8_t)(hdr >> 16), (uint8_t)(hdr >> 8), (uint8_t)hdr};
        for (int i = 0; i < 8; i++) xrit.push_back(0);
        xrit.push_back(4); xrit.push_back(0); xrit.push_back((uint8_t)(3 + ann.size()));
        xrit.insert(xrit.end(), ann.begin(), ann.end());
        for (int i = 0; i < 6000; i++) xrit.push_back((uint8_t)(i * 37 + line));
        file.assign(10, 0);
        file.insert(file.end(), xrit.begin(), xrit.end());
        std::vector<std::vector<uint8_t>> pk;
        size_t at = 0; bool first = true;
        while (at < file.size()) {
            const size_t n = std::min<size_t>(3000, file.size() - at);
            const bool last = at + n == file.size();
            std::vector<uint8_t> r(6 + 8, 0);
            const int flag = first ? (last ? 3 : 1) : (last ? 2 : 0);
            r[0] = 0x08; r[1] = 0x20;
            r[2] = (uint8_t)(flag << 6); r[3] = (uint8_t)seqGrb++;
            r.insert(r.end(), file.begin() + (std::ptrdiff_t)at, file.begin() + (std::ptrdiff_t)(at + n));
            const size_t total = r.size() + 4;
            const size_t len = total - 6 - 8 - 1;
            r[4] = (uint8_t)(len >> 8); r[5] = (uint8_t)len;
            const uint32_t crc = crc32Hdlc(r.data(), r.size());
            r.push_back((uint8_t)(crc >> 24)); r.push_back((uint8_t)(crc >> 16)); r.push_back((uint8_t)(crc >> 8)); r.push_back((uint8_t)crc);
            pk.push_back(std::move(r));
            at += n; first = false;
        }
        addAosPackets(pk, 7);
        line++;
    }

    std::vector<uint8_t> s2Bytes(size_t n) {
        while (stream.size() < n) { if (P == kSd3Grb) grbNext(); else uhritNext(); }
        std::vector<uint8_t> out(stream.begin(), stream.begin() + (std::ptrdiff_t)n);
        stream.erase(stream.begin(), stream.begin() + (std::ptrdiff_t)n);
        return out;
    }

    void s2Symbols() {
        std::vector<cf32> buf(4096);
        tx->generate(buf.data(), buf.size());
        for (auto& s : buf) pushSym(s.real() * (float)std::sqrt(2.0), s.imag() * (float)std::sqrt(2.0));
    }

    void refill() {
        switch (P) {
        case kSd3Grb: case kSd3Uhrit: s2Symbols(); break;
        case kSd3Gvar: nextGvar(); break;
        case kSd3Mdl: nextMdl(); break;
        case kSd3Geoscan: case kSd3Lucky7: case kSd3Spino: case kSd3Erminaz: nextCubesat(); break;
        default: nextTableFrame(); break;
        }
    }

    void generate(cf32* out, size_t n) {
        const bool twoRail = l.mod == kSd3Qpsk || l.mod == kSd3Oqpsk || P == kSd3Grb || P == kSd3Uhrit;
        const bool fsk = l.mod == kSd3Fsk && P != kSd3Grb && P != kSd3Uhrit;
        const bool pm = l.mod == kSd3Pm;
        const double dev = (P >= kSd3Geoscan ? 0.25 : 0.5) * rs;
        const double oq = l.mod == kSd3Oqpsk ? 0.5 : 0;
        for (size_t i = 0; i < n; i++) {
            while ((int64_t)I.size() < 2 * kSpan + 4 + (int64_t)std::floor(t) - base) refill();
            int64_t idx = (int64_t)std::floor(t);
            while (base < idx - kSpan - 2) { I.pop_front(); Q.pop_front(); env.pop_front(); base++; }
            float a = 0, b = 0, en = 0;
            for (int64_t k = idx - kSpan + 1; k <= idx + kSpan; k++) {
                const int64_t at = k - base;
                if (at < 0 || at >= (int64_t)I.size()) continue;
                const float h = tabAt(t - (double)k);
                a += I[(size_t)at] * h;
                en += env[(size_t)at] * h;
                if (twoRail) b += Q[(size_t)at] * tabAt(t - (double)k - oq);
            }
            std::complex<float> s;
            if (fsk) { fmPhase += 2 * kPi * dev * a / c.rate; s = std::polar(std::clamp(en * 1.05f, 0.f, 1.f), (float)fmPhase); }
            else if (pm) s = std::polar(1.f, a * 1.05f);
            else s = std::complex<float>(a, twoRail ? b : 0) * (twoRail ? 1 / 1.4142f : 1.f);
            s *= (float)amp * std::polar(1.f, (float)phase);
            phase += 2 * kPi * freq / c.rate;
            freq += c.driftHzPerS / c.rate;
            if (phase > 2 * kPi) phase -= 2 * kPi; else if (phase < -2 * kPi) phase += 2 * kPi;
            if (sigma > 0) s += std::complex<float>((float)(sigma * nd(rng)), (float)(sigma * nd(rng)));
            if (c.invert) s = std::conj(s);
            out[i] = cf32(s.real(), s.imag());
            t += tstep;
        }
    }

    float tabAt(double x) const {
        const double p = (x + kSpan) * kTab;
        if (p < 0 || p >= (double)tab.size() - 2) return 0;
        const int k = (int)p;
        const float f = (float)(p - k);
        return tab[(size_t)k] * (1 - f) + tab[(size_t)k + 1] * f;
    }
};

Sd3Generator::Sd3Generator(const Sd3GenConfig& c) : p_(std::make_unique<Impl>(c)) {}
Sd3Generator::~Sd3Generator() = default;
void Sd3Generator::generate(cf32* out, size_t n) { p_->generate(out, n); }

namespace {
class Synth : public ModeSynth {
public:
    Synth(const SynthConfig& cfg, double rate, int profile) : r_(rate) {
        Sd3GenConfig g;
        g.profile = profile; g.rate = rate; g.snrDb = cfg.snrDb; g.cfoHz = cfg.cfoHz;
        gen_ = std::make_unique<Sd3Generator>(g);
    }
    double sampleRate() const override { return r_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double r_;
    std::unique_ptr<Sd3Generator> gen_;
};
}

std::unique_ptr<ModeSynth> makeSd3Synth(const SynthConfig& cfg, double sampleRate, int profile) { return std::make_unique<Synth>(cfg, sampleRate, profile); }

} // namespace dect2
