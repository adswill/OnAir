// LoRaWAN test signal (see lorawan_gen.h).
#include "dect2/lorawan/lorawan_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/lorawan/lorawan_rx.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2 {

namespace {
std::vector<uint8_t> noise(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 2654435761u + 1u;
    for (auto& b : v) { x = x * 1664525u + 1013904223u; b = (uint8_t)(x >> 24); }
    return v;
}
} // namespace

std::vector<LorawanGenFrame> lorawanTestSchedule(int plan) {
    using namespace lorawan;
    std::vector<LorawanGenFrame> v;
    const double dial = lorawanPlanCentreMhz(plan);
    // the 125 kHz uplink channels nearest the dial, and one wider / narrower channel of the plan
    std::vector<PlanChannel> ch125, alt;
    for (const PlanChannel& c : planChannels(plan)) {
        if (!c.uplink || std::fabs(c.mhz - dial) > 1.3) continue;
        (c.bwKhz == 125 ? ch125 : alt).push_back(c);
    }
    std::sort(ch125.begin(), ch125.end(), [&](const PlanChannel& a, const PlanChannel& b) { return std::fabs(a.mhz - dial) < std::fabs(b.mhz - dial); });
    if (ch125.empty()) return v;
    auto slot = [&](size_t i) { return ch125[i % ch125.size()]; };
    auto mk = [&](const PlanChannel& c, int sf, bool inv, const std::vector<uint8_t>& pl, double snrOff, double cfo) {
        LorawanGenFrame f;
        int lo, hi;
        planSfRange(plan, c.bwKhz, inv, lo, hi);
        f.mhz = c.mhz;
        f.p.sf = std::max(lo, std::min(hi, sf)); f.p.bwHz = c.bwKhz * 1e3; f.p.cr = 5; f.p.crc = !inv; f.p.preamble = 8; f.p.syncWord = 0x34;
        f.p.ldro = lora::autoLdro(f.p.sf, f.p.bwHz);
        f.payload = pl; f.inverted = inv; f.snrOffDb = snrOff; f.cfoHz = cfo;
        return f;
    };
    const bool usau = plan == US915 || plan == AU915;   // the gateways answer on 500 kHz channels around 923 MHz, outside a capture around the uplinks: no downlinks in this scene
    Build b;
    // 0, 1: two uplinks that start together on different channels
    b.mtype = 2; b.devAddr = 0x26011A2B; b.adr = true; b.fcnt = 100; b.fport = 1; b.frm = noise(8, 1); b.mic = 0xA1B2C3D4;
    { LorawanGenFrame f = mk(slot(0), 7, false, build(b), 0, 1300); f.startSec = 0.05; v.push_back(f); }
    b = Build(); b.mtype = 2; b.devAddr = 0x26014C01; b.fcnt = 3041; b.fport = 2; b.frm = noise(20, 2); b.mic = 0x0BADF00D;
    { LorawanGenFrame f = mk(slot(1), 11, false, build(b), 0, -2100); f.startSec = 0.05; v.push_back(f); }
    // 2, 3: a join request and the gateway's join accept (inverted IQ), later
    { LorawanGenFrame f = mk(slot(2), 12, false, buildJoinRequest(0x70B3D57ED0000001ull, 0x70B3D57ED0123456ull, 0x4A2B, 0x12345678), 0, 900); f.startSec = 0.5; f.gapSec = 0.3; v.push_back(f); }
    if (!usau) v.push_back(mk(slot(2), 12, true, buildJoinAccept(17, 3), 0, 0));
    // 4, 5: a confirmed uplink with MAC commands in FOpts and the answer with an ACK and a LinkADRReq
    b = Build(); b.mtype = 4; b.devAddr = 0x260B1234; b.fcnt = 7; b.fport = 10; b.frm = noise(11, 4); b.fopts = {0x06, 0x4F, 0x0A, 0x02}; b.mic = 0x55AA55AA; b.adr = true; b.adrAckReq = true;
    v.push_back(mk(slot(3), 9, false, build(b), 0, 1700));
    b = Build(); b.mtype = 3; b.devAddr = 0x260B1234; b.fcnt = 2001; b.ack = true; b.fopts = {0x03, 0x51, 0xFF, 0x00, 0x01}; b.fport = -1; b.mic = 0xC001D00D;
    if (!usau) v.push_back(mk(slot(3), 9, true, build(b), 0, 0));
    // 6: the plan's other bandwidth (DR6 250 kHz, or the 500 kHz channel of US / AU)
    if (!alt.empty()) {
        b = Build(); b.mtype = 2; b.devAddr = 0x2701AAAA; b.fcnt = 2; b.fport = 5; b.frm = noise(14, 6); b.mic = 0x01020304;
        v.push_back(mk(alt[0], 7, false, build(b), 3, -900));
    }
    // 7: an uplink with a LinkCheckReq
    b = Build(); b.mtype = 2; b.devAddr = 0x27023C00; b.fcnt = 55; b.fport = 1; b.frm = noise(5, 7); b.fopts = {0x02}; b.mic = 0xDEADBEEF;
    v.push_back(mk(slot(4), 10, false, build(b), 0, 2400));
    // 8: proprietary
    { std::vector<uint8_t> pr = noise(13, 8); pr[0] = 0xE0; v.push_back(mk(slot(5), 8, false, pr, 0, -300)); }
    return v;
}

std::vector<LrfhssGenFrame> lorawanLrfhssSchedule(int plan) {
    using namespace lorawan;
    std::vector<LrfhssGenFrame> v;
    struct Item { int dr; double mhz, start; int seq, dev; uint32_t addr; uint16_t fcnt; int port; size_t len; std::vector<uint8_t> fopts; double cfo; };
    std::vector<Item> items;
    if (plan == EU868)
        items = {{8, 868.1, 0.10, 17, 0, 0x260C0801u, 412, 2, 11, {}, 700},
                 {10, 868.3, 0.35, 260, -2, 0x260C1002u, 9, 5, 6, {0x02}, -1200},
                 {9, 868.5, 2.40, 300, 3, 0x260C0903u, 77, 3, 24, {}, 400},
                 {11, 868.3, 2.70, 451, 1, 0x260C1104u, 1500, 10, 30, {0x06, 0x50, 0x09}, -300}};
    else if (plan == US915)
        items = {{5, 904.6, 0.10, 100, 7, 0x260C0505u, 21, 2, 8, {}, 900}, {6, 903.0, 0.30, 333, -11, 0x260C0606u, 3, 4, 20, {0x02}, -600}};
    for (const Item& it : items) {
        LrfhssGenFrame f;
        for (const LrfhssRate& r : planLrfhssRates(plan))
            if (r.dr == it.dr) { f.p.cr = r.cr; f.p.bw = r.bw; f.p.grid = r.grid; f.p.headers = r.headers; }
        Build b;
        b.mtype = 2; b.devAddr = it.addr; b.fcnt = it.fcnt; b.fport = it.port; b.adr = true; b.fopts = it.fopts;
        b.frm = noise(it.len, it.addr); b.mic = 0x4C524600u + (uint32_t)it.dr;   // "LRF" and the data rate
        f.payload = build(b);
        f.mhz = it.mhz; f.hopSeqId = it.seq; f.deviceOffset = it.dev; f.startSec = it.start; f.cfoHz = it.cfo;
        f.snrOffDb = -16;            // a 488 bit/s hop is far narrower than a 125 kHz LoRa channel: 16 dB below a LoRa frame is an Eb/N0 8 dB over its SNR
        v.push_back(f);
    }
    return v;
}

double lorawanSceneSeconds(const std::vector<LorawanGenFrame>& v) {
    double t = 0.05, end = 0;
    for (const auto& f : v) {
        const double s = f.startSec >= 0 ? f.startSec : t;
        const double e = s + lora::airSeconds(f.p, f.payload.size());
        end = std::max(end, e);
        t = std::max(t, e + f.gapSec);
    }
    return end + 0.2;
}

struct LorawanGenerator::Impl {
    LorawanGenConfig c;
    double noiseRms = 0;
    genutil::NoiseSource noise;
    std::mt19937 rng;
    int64_t pos = 0;
    size_t next = 0;
    double nextStart = 0, cycle = 0;
    struct Active { lora::TxFrame f; bool inverted; };
    std::vector<Active> active;
    double lastEnd = 0;
    std::vector<cf32> tmp;
    LrfhssRenderer lr;
    bool lrRound = false;      // the LR-FHSS frames of this round of the scene are scheduled

    explicit Impl(const LorawanGenConfig& cfg)
        : c(cfg), noise(cfg.seed * 7919u + 3u), rng(cfg.seed * 977u + 5u), lr(cfg.rate, cfg.dialMhz, cfg.dialOffsetHz, cfg.sroPpm, cfg.seed) {
        nextStart = c.leadSec;
        if (c.snrDb < 99) noiseRms = c.amp / std::sqrt(std::pow(10.0, c.snrDb / 10) * 125e3 / c.rate);
    }

    void schedule(double until) {
        while (!c.frames.empty() && nextStart <= until) {
            if (next >= c.frames.size()) {
                if (!c.loop) return;
                next = 0;
                cycle = std::max(lastEnd + 0.3, nextStart);
                nextStart = cycle;
                lrRound = false;
            }
            if (!lrRound) {
                lrRound = true;
                for (const LrfhssGenFrame& g : c.lrfhss) {
                    const double st = cycle + std::max(0.0, g.startSec);
                    lr.add(g, st, c.amp * std::pow(10.0, g.snrOffDb / 20), c.cfoHz);
                    lastEnd = std::max(lastEnd, st + lrfhssFrameSeconds(g));
                }
            }
            const LorawanGenFrame& g = c.frames[next];
            const double base = (g.startSec >= 0 ? cycle + g.startSec : nextStart);
            if (base > until) return;
            next++;
            lora::TxFrame f;
            f.p = g.p;
            f.data = lora::encode(g.p, g.payload.data(), g.payload.size());
            f.startSec = base;
            f.sroPpm = c.sroPpm;
            f.amp = (float)(c.amp * std::pow(10.0, g.snrOffDb / 20));
            const double off = (g.mhz - c.dialMhz) * 1e6 + c.dialOffsetHz + c.cfoHz + g.cfoHz;
            f.freqHz = g.inverted ? -off : off;      // the conjugate puts it back
            f.phase0 = dect2::uniform_real_distribution<double>(0, 1)(rng);
            nextStart = std::max(nextStart, f.endSec() + g.gapSec);
            lastEnd = std::max(lastEnd, f.endSec());
            active.push_back({std::move(f), g.inverted});
        }
    }
};

LorawanGenerator::LorawanGenerator(const LorawanGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
LorawanGenerator::~LorawanGenerator() = default;
double LorawanGenerator::endSec() const { return p_->lastEnd; }

void LorawanGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const double t0 = (double)m.pos / m.c.rate, t1 = (double)(m.pos + (int64_t)n) / m.c.rate;
    m.schedule(t1 + 1);
    for (size_t i = 0; i < n; i++) out[i] = cf32(0, 0);
    for (const auto& a : m.active) {
        if (!a.inverted) { a.f.render(out, n, t0, m.c.rate); continue; }
        m.tmp.assign(n, cf32(0, 0));
        a.f.render(m.tmp.data(), n, t0, m.c.rate);
        for (size_t i = 0; i < n; i++) out[i] += std::conj(m.tmp[i]);
    }
    m.lr.render(out, n, t0);
    if (m.noiseRms > 0) m.noise.add(out, n, (float)(m.noiseRms / std::sqrt(2.0)));
    m.active.erase(std::remove_if(m.active.begin(), m.active.end(), [&](const Impl::Active& a) { return a.f.endSec() < t1; }), m.active.end());
    m.pos += (int64_t)n;
}

namespace {
class LorawanSynth : public ModeSynth {
public:
    LorawanSynth(const SynthConfig& s, double rate) : rate_(rate) {
        LorawanGenConfig c;
        const int plan = std::max(0, std::min((int)lorawan::kPlans - 1, s.modeOpt[0]));
        c.rate = rate;
        c.dialMhz = lorawanPlanCentreMhz(plan);
        c.dialOffsetHz = -lorawanTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.sroPpm = s.sroPpm;
        c.frames = lorawanTestSchedule(plan);
        if (s.modeOpt[1] == 0) c.lrfhss = lorawanLrfhssSchedule(plan);
        gen_ = std::make_unique<LorawanGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<LorawanGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeLorawanSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<LorawanSynth>(cfg, sampleRate);
}

} // namespace dect2
