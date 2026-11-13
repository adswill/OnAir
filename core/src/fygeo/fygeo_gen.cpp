// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FengYun geostationary test signals (fygeo_gen.h).
#include "dect2/fygeo/fygeo_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/rrc.h"
#include "dect2/dvbs/dvbs_gen.h"
#include "dect2/fygeo/fygeo_tel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
using dect2::dsp::rrc;
}

// ---------------------------------------------------------------- FY-4 test files
std::vector<uint16_t> fy4TestPicture(int ch) {
    const int W = kFy4TestW, H = kFy4TestH * kFy4TestSegs;
    std::vector<uint16_t> p((size_t)W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const double dx = (x - W / 2.0) / (H / 2.0 - 1), dy = (y - H / 2.0) / (H / 2.0 - 1), r = std::sqrt(dx * dx + dy * dy);
            double v = r > 1 ? 0.03 : 0.2 + 0.6 * std::sqrt(1 - r * r) * (1 + 0.15 * std::sin(x * 0.3 + ch) * std::cos(y * 0.2));
            if (ch > 2 && y > 40 && y < 56 && x > 30 && x < 70) v = 0.98;   // a bright cloud bank across the two segments
            const int noise = (x * 7 + y * 13 + ch * 31) % 5 - 2;
            p[(size_t)y * W + x] = (uint16_t)std::max(0, std::min(4095, (int)(v * 4095) + noise));
        }
    return p;
}

const char* fy4TestText() {
    return "OnAir FY-4 test signal\r\nThis is a synthetic FengYun-4 LRIT message made by OnAir; nothing is transmitted.\r\n";
}

fygeo::Fy4Headers fy4TestImageHeaders(int round, int ch, int seg, const char* sat) {
    fygeo::Fy4Headers h;
    h.fileType = 0;
    h.hasImage = true;
    h.satName = sat; h.instName = "AGRI";
    h.bpp = 12; h.columns = kFy4TestW; h.lines = kFy4TestH; h.compression = 2;
    h.channel = ch; h.totalSeg = kFy4TestSegs; h.segNo = seg + 1; h.segPos = seg + 1; h.segLine = seg * kFy4TestH;
    h.algo = 1; h.lossless = 1; h.level = 0;
    h.hasNav = true; h.projection = "GEOS(104.7)"; h.cfac = 40932548.f; h.lfac = 40932548.f; h.coff = 1374.5f; h.loff = 1374.5f;
    char nm[200];
    const int minute = round % 60;
    const char tag[7] = {'F', 'Y', '4', sat[4], '-', 0, 0};
    snprintf(nm, sizeof nm, "%s_AGRI--_N_DISK_1047E_L1-_FDI-_C%03d_NOM_2026110112%02d00_2026110112%02d17_4000M_V0001.HDF", tag, ch, minute, minute);
    h.annotation = nm;
    h.hasTime = true; h.days = 25141; h.msOfDay = 43200000u + (uint32_t)minute * 60000u;
    return h;
}

std::vector<Fy4TestFile> fy4TestFiles(int round, const char* sat) {
    std::vector<Fy4TestFile> v;
    // one cache of the compressed segments: the pictures do not change between rounds
    static std::vector<std::vector<uint8_t>> cache[16];
    static std::mutex cacheMu;
    std::lock_guard<std::mutex> cacheLk(cacheMu);
    for (int ch : {2, 12}) {
        const std::vector<uint16_t> pic = fy4TestPicture(ch);
        if (cache[ch].empty())
            for (int s = 0; s < kFy4TestSegs; s++)
                cache[ch].push_back(fygeo::j2kEncode(pic.data() + (size_t)s * kFy4TestW * kFy4TestH, kFy4TestW, kFy4TestH, 12, 0));
        for (int s = 0; s < kFy4TestSegs; s++) {
            Fy4TestFile f;
            f.vcid = 1; f.apid = 0x101 + ch;
            f.channel = ch; f.segment = s;
            f.h = fy4TestImageHeaders(round, ch, s, sat);
            const std::vector<uint8_t>& data = cache[ch][(size_t)s];
            // a few bytes of other data in front of the code stream, as the real files have: the receiver looks for the SOC / SIZ markers
            std::vector<uint8_t> body(6, 0);
            body.insert(body.end(), data.begin(), data.end());
            f.h.dataBits = (uint64_t)body.size() * 8;
            const size_t chunk = 900;
            for (size_t o = 0; o < body.size(); o += chunk) f.dataChunks.emplace_back(body.begin() + (std::ptrdiff_t)o, body.begin() + (std::ptrdiff_t)std::min(body.size(), o + chunk));
            f.file = fygeo::buildFy4Headers(f.h);
            f.file.insert(f.file.end(), body.begin(), body.end());
            v.push_back(std::move(f));
        }
    }
    // a text message
    {
        Fy4TestFile f;
        f.vcid = 2; f.apid = 0x200;
        f.h.fileType = 2;
        f.h.annotation = "FY4_MESSAGE.TXT";
        f.h.hasTime = true; f.h.days = 25141; f.h.msOfDay = 43200000u;
        const std::string t = fy4TestText();
        f.h.dataBits = (uint64_t)t.size() * 8;
        f.dataChunks.emplace_back(t.begin(), t.end());
        f.file = fygeo::buildFy4Headers(f.h);
        f.file.insert(f.file.end(), t.begin(), t.end());
        v.push_back(std::move(f));
    }
    return v;
}

goes::FileOut fy4FileOut(const Fy4TestFile& f, uint16_t counter) {
    goes::FileOut o;
    o.apid = f.apid;
    std::vector<uint8_t> first;
    first.push_back((uint8_t)(counter >> 8)); first.push_back((uint8_t)counter);
    const uint64_t bits = (uint64_t)f.file.size() * 8;
    for (int i = 7; i >= 0; i--) first.push_back((uint8_t)(bits >> (8 * i)));
    const std::vector<uint8_t> hdr = fygeo::buildFy4Headers(f.h);
    first.insert(first.end(), hdr.begin(), hdr.end());
    o.chunks.push_back(first);
    for (const auto& c : f.dataChunks) o.chunks.push_back(c);
    return o;
}

// ---------------------------------------------------------------- FY-4 signal
struct Fy4Generator::Impl {
    Fy4GenConfig c;
    goes::Mux mux;
    uint16_t counter = 0;
    int round = 0, fillLeft = 0;
    bool once = false;
    uint8_t cc = 0;
    std::vector<uint8_t> q;        // transport stream bytes waiting
    size_t qpos = 0;
    uint64_t cadus = 0;
    std::unique_ptr<dvbs::DvbsSignal> sig;

    explicit Impl(const Fy4GenConfig& cfg) : c(cfg), mux(cfg.scid, fygeo::kZoneBytes) {
        addRound();
        fillLeft = std::max(0, c.fillFrames);
        dvbs::DvbsSignalConfig sc;
        sc.sampleRate = c.rate;
        sc.snrDb = c.snrDb;
        sc.cfoHz = c.cfoHz;
        sc.clockPpm = c.sroPpm;
        sc.seed = c.seed;
        dvbs::DvbsTxConfig& t = sc.tx;
        t.standard = 2; t.mod = dvbs::kQpsk; t.shortFrame = false; t.pilots = false; t.rollOff = 0.25;
        const int link = std::max(0, std::min(2, c.link));
        t.rate = link == kFy4aLrit ? 2 : link == kFy4bLrit ? 9 : 8;     // 2/5, 8/9, 5/6
        t.symbolRate = c.symbolRate > 0 ? c.symbolRate : fygeoSymbolRate(link);
        sc.ts = [this](uint8_t* p) { nextTs(p); };
        sig = std::make_unique<dvbs::DvbsSignal>(sc);
    }
    void addRound() {
        for (const auto& f : fy4TestFiles(round, "FY-4A")) mux.addFile(f.vcid, fy4FileOut(f, counter++));
        round++;
    }
    void nextCadu() {
        uint8_t v[8 + fygeo::kZoneBytes];
        if (!mux.next(v)) {
            if (once || fillLeft > 0) { mux.fill(v); if (fillLeft > 0) fillLeft--; }
            else if (c.fillFrames < 0) { once = true; mux.fill(v); }
            else { addRound(); fillLeft = c.fillFrames; mux.next(v); }
        }
        uint8_t cadu[fygeo::kCaduBytes];
        cadu[0] = 0x1A; cadu[1] = 0xCF; cadu[2] = 0xFC; cadu[3] = 0x1D;
        memcpy(cadu + 4, v, 1020);
        fygeo::caduToTs(cadu, c.pid, cc, q);
        cadus++;
    }
    void nextTs(uint8_t* p) {
        if (qpos >= q.size()) { q.clear(); qpos = 0; nextCadu(); }
        memcpy(p, q.data() + qpos, 188);
        qpos += 188;
    }
};
Fy4Generator::Fy4Generator(const Fy4GenConfig& c) : p_(std::make_unique<Impl>(c)) {}
Fy4Generator::~Fy4Generator() = default;
void Fy4Generator::generate(cf32* out, size_t n) { p_->sig->generate(out, n); }
uint64_t Fy4Generator::cadusSent() const { return p_->cadus; }

// ---------------------------------------------------------------- S-VISSR
void svissrTestLine(int line, uint8_t* ir1, uint8_t* ir2, uint8_t* ir3, uint8_t* ir4, uint8_t* vis4) {
    uint8_t* ir[4] = {ir1, ir2, ir3, ir4};
    for (int k = 0; k < 4; k++)
        for (int x = 0; x < fygeo::kSvissrIrWidth; x++) {
            const double v = 128 + 100 * std::sin(x * 0.004 * (k + 1) + line * 0.09 + k) * std::cos(x * 0.0007 + k);
            ir[k][x] = (uint8_t)std::max(1, std::min(254, (int)v));
        }
    for (int r = 0; r < 4; r++)
        for (int x = 0; x < fygeo::kSvissrVisWidth; x++) {
            const double v = 120 + 100 * std::sin(x * 0.001 + (line * 4 + r) * 0.02);
            vis4[(size_t)r * fygeo::kSvissrVisWidth + x] = (uint8_t)std::max(0, std::min(255, (int)v));
        }
}

struct SvissrGenerator::Impl {
    SvissrGenConfig c;
    std::vector<float> sym;
    int64_t symBase = 0, symNext = 0;
    uint64_t frames = 0;
    std::vector<float> table;
    static constexpr int kOver = 64, kSpan = 6;
    int64_t n = 0;
    double phase0 = 0;
    std::mt19937 rng;
    dect2::normal_distribution<float> gauss{0.f, 1.f};
    float sigma = 0;
    float level = 1.f;
    std::vector<uint8_t> pre;
    int line = 0;
    std::vector<std::vector<uint8_t>> rowCache;

    explicit Impl(const SvissrGenConfig& cfg) : c(cfg), rng(cfg.seed) {
        table.resize((size_t)(2 * kSpan * kOver + 2));
        for (size_t i = 0; i < table.size(); i++) table[i] = (float)rrc((double)i / kOver - kSpan, c.rolloff);
        sigma = c.snrDb >= 99 ? 0.f : (float)std::sqrt(c.amplitude * c.amplitude * (c.rate / c.symbolRate) / std::pow(10.0, c.snrDb / 10) / 2);
        phase0 = dect2::uniform_real_distribution<double>(0, 2 * kPi)(rng);
        sym.assign(kSpan, 0.f); symBase = -kSpan;
        pre = fygeo::svissrPreambleBits();
        line = c.firstLine;
    }
    void frame() {
        uint8_t f[fygeo::kSvissrFrameBytes];
        static thread_local std::vector<uint8_t> ir[4], vis;
        for (auto& v : ir) v.resize(fygeo::kSvissrIrWidth);
        vis.resize((size_t)4 * fygeo::kSvissrVisWidth);
        const int ln = line;
        svissrTestLine(ln, ir[0].data(), ir[1].data(), ir[2].data(), ir[3].data(), vis.data());
        fygeo::svissrPack(f, ln, ir[0].data(), ir[1].data(), ir[2].data(), ir[3].data(), vis.data());
        fygeo::svissrScramble(f);
        if (++line >= c.firstLine + std::max(1, c.lines)) line = c.firstLine;
        // the bits: the preamble, then the frame; NRZ-M coded: a 1 changes the level
        for (size_t i = 0; i < pre.size() + (size_t)fygeo::kSvissrFrameBits; i++) {
            const uint8_t bit = i < pre.size() ? pre[i] : (uint8_t)((f[(i - pre.size()) >> 3] >> (7 - ((i - pre.size()) & 7))) & 1);
            if (bit) level = -level;
            sym.push_back(level);
        }
        symNext += (int64_t)pre.size() + fygeo::kSvissrFrameBits;
        frames++;
    }
    void generate(cf32* out, size_t cnt) {
        const double rs = c.symbolRate * (1 + c.sroPpm * 1e-6);
        const double w = 2 * kPi * c.cfoHz / c.rate;
        for (size_t i = 0; i < cnt; i++, n++) {
            const double u = (double)n / c.rate * rs;
            const int64_t k0 = (int64_t)std::floor(u);
            while (k0 + kSpan >= symNext) frame();
            const double d0 = (u - (double)(k0 - kSpan + 1) + kSpan) * kOver;
            const int j0 = (int)d0;
            const float f = (float)(d0 - j0);
            const float* sp = &sym[(size_t)(k0 - kSpan + 1 - symBase)];
            float acc = 0;
            for (int t = 0, j = j0; t < 2 * kSpan; t++, j -= kOver)
                if (j >= 0 && j + 1 < (int)table.size()) acc += sp[t] * (table[(size_t)j] + f * (table[(size_t)j + 1] - table[(size_t)j]));
            const double ph = phase0 + w * (double)n;
            cf32 s = cf32((float)(c.amplitude * acc * std::cos(ph)), (float)(c.amplitude * acc * std::sin(ph)));
            if (sigma > 0) s += cf32{sigma * gauss(rng), sigma * gauss(rng)};
            out[i] = s;
        }
        const int64_t keep = (int64_t)std::floor((double)n / c.rate * rs) - kSpan - 4;
        if (keep - symBase > 65536) { sym.erase(sym.begin(), sym.begin() + (keep - symBase)); symBase = keep; }
    }
};
SvissrGenerator::SvissrGenerator(const SvissrGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
SvissrGenerator::~SvissrGenerator() = default;
void SvissrGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }
uint64_t SvissrGenerator::framesSent() const { return p_->frames; }

// ---------------------------------------------------------------- the mode synths
namespace {
class Fy4Synth : public ModeSynth {
public:
    Fy4Synth(const SynthConfig& s, double rate, int link) : rate_(rate) {
        Fy4GenConfig c;
        c.rate = rate; c.link = link; c.snrDb = s.snrDb; c.cfoHz = s.cfoHz; c.sroPpm = s.sroPpm;
        if (s.modeVal[0] > 0) c.symbolRate = s.modeVal[0];
        gen_ = std::make_unique<Fy4Generator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<Fy4Generator> gen_;
};
class SvissrSynth : public ModeSynth {
public:
    SvissrSynth(const SynthConfig& s, double rate) : rate_(rate) {
        SvissrGenConfig c;
        c.rate = rate; c.snrDb = s.snrDb; c.cfoHz = s.cfoHz; c.sroPpm = s.sroPpm;
        if (s.modeVal[0] > 0) c.symbolRate = s.modeVal[0];
        gen_ = std::make_unique<SvissrGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<SvissrGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeFy4LritSynth(const SynthConfig& cfg, double sampleRate) { return sampleRate > 0 ? std::make_unique<Fy4Synth>(cfg, sampleRate, cfg.modeOpt[0] ? kFy4bLrit : kFy4aLrit) : nullptr; }
std::unique_ptr<ModeSynth> makeFy4aHritSynth(const SynthConfig& cfg, double sampleRate) { return sampleRate > 0 ? std::make_unique<Fy4Synth>(cfg, sampleRate, kFy4aHrit) : nullptr; }
std::unique_ptr<ModeSynth> makeFy2SvissrSynth(const SynthConfig& cfg, double sampleRate) { return sampleRate > 0 ? std::make_unique<SvissrSynth>(cfg, sampleRate) : nullptr; }

} // namespace dect2
