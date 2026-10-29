// LoRaWAN test signal (see lorawan_gen.h).
#include "dect2/lorawan/lorawan_gen.h"
#include "dect2/common/mathconst.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/lorawan/lorawan_fsk.h"
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

std::vector<LorawanFskFrame> lorawanFskTestSchedule(int plan) {
    using namespace lorawan;
    std::vector<LorawanFskFrame> v;
    const std::vector<double> ch = planFskChannelsMhz(plan);
    if (ch.empty()) return v;
    auto mk = [&](const std::vector<uint8_t>& pl, double start, double cfo, double bt, uint8_t pre) {
        LorawanFskFrame f;
        f.mhz = ch[0]; f.payload = pl; f.startSec = start; f.cfoHz = cfo; f.bt = bt; f.preamble = pre;
        return f;
    };
    Build b;
    // an uplink that starts while the SF11 uplink of the LoRa scene is still on the air (on another channel)
    b.mtype = 2; b.devAddr = 0x26015A7E; b.adr = true; b.fcnt = 812; b.fport = 3; b.frm = noise(24, 11); b.mic = 0x6C1E2F33;
    v.push_back(mk(build(b), 0.12, 3000, 1.0, 0x55));
    // a join request from an SX1276 set up as LoRaMac-node does it: no Gaussian shaping, 0xAA preamble
    v.push_back(mk(buildJoinRequest(0x70B3D57ED0000002ull, 0x0004A30B001C0530ull, 0x1D2C, 0x7E57F00D), 0.6, -4200, 0.0, 0xAA));
    // the gateway's answer to the uplink in RX1 (one second later, same channel and data rate): ACK and a LinkADRReq
    b = Build(); b.mtype = 3; b.devAddr = 0x26015A7E; b.fcnt = 77; b.ack = true; b.fopts = {0x03, 0x71, 0x07, 0x00, 0x01}; b.mic = 0x0DDBA110;
    v.push_back(mk(build(b), 1.12, 0, 1.0, 0x55));
    return v;
}

double lorawanFskSceneSeconds(const std::vector<LorawanFskFrame>& v) {
    double end = 0;
    for (const auto& f : v) end = std::max(end, f.startSec + lorawan::fsk::airSeconds(f.payload.size()));
    return end > 0 ? end + 0.05 : 0;
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
    struct ActiveFsk { int64_t at; std::vector<cf32> s; };   // FSK frames rendered whole, from sample `at`
    std::vector<ActiveFsk> fsk;
    double fskCycle = 0, fskPeriod = 0, fskEnd = 0;

    explicit Impl(const LorawanGenConfig& cfg) : c(cfg), noise(cfg.seed * 7919u + 3u), rng(cfg.seed * 977u + 5u) {
        nextStart = c.leadSec;
        if (c.snrDb < 99) noiseRms = c.amp / std::sqrt(std::pow(10.0, c.snrDb / 10) * 125e3 / c.rate);
        fskPeriod = c.fskPeriodSec > 0 ? c.fskPeriodSec : std::max(c.frames.empty() ? 0.0 : lorawanSceneSeconds(c.frames), lorawanFskSceneSeconds(c.fsk)) + 0.3;
    }

    void scheduleFsk(double until) {
        while (!c.fsk.empty() && fskCycle <= until) {
            for (const LorawanFskFrame& g : c.fsk) {
                const double t = fskCycle + g.startSec;
                const double sampleAt = t * c.rate;
                const int64_t at = (int64_t)std::floor(sampleAt);
                const double off = (g.mhz - c.dialMhz) * 1e6 + c.dialOffsetHz + c.cfoHz + g.cfoHz;
                const std::vector<uint8_t> bits = lorawan::fsk::frameBits(g.payload.data(), g.payload.size(), g.preamble);
                const double ph = dect2::uniform_real_distribution<double>(0, 2 * math::kPi)(rng);
                ActiveFsk a;
                a.at = at;
                a.s = lorawan::fsk::modulate(bits, c.rate, off, g.bt, c.sroPpm, (double)at - sampleAt, (float)(c.amp * std::pow(10.0, g.snrOffDb / 20)), ph);
                fskEnd = std::max(fskEnd, t + (double)a.s.size() / c.rate);
                fsk.push_back(std::move(a));
            }
            if (!c.loop) { fskCycle = 1e300; break; }
            fskCycle += fskPeriod;
        }
    }

    void schedule(double until) {
        while (!c.frames.empty() && nextStart <= until) {
            if (next >= c.frames.size()) {
                if (!c.loop) return;
                next = 0;
                cycle = std::max(lastEnd + 0.3, nextStart);
                nextStart = cycle;
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
double LorawanGenerator::endSec() const { return std::max(p_->lastEnd, p_->fskEnd); }

void LorawanGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const double t0 = (double)m.pos / m.c.rate, t1 = (double)(m.pos + (int64_t)n) / m.c.rate;
    m.schedule(t1 + 1);
    m.scheduleFsk(t1 + 1);
    for (size_t i = 0; i < n; i++) out[i] = cf32(0, 0);
    for (const auto& a : m.fsk) {
        const int64_t s0 = std::max(a.at, m.pos), s1 = std::min(a.at + (int64_t)a.s.size(), m.pos + (int64_t)n);
        for (int64_t j = s0; j < s1; j++) out[(size_t)(j - m.pos)] += a.s[(size_t)(j - a.at)];
    }
    m.fsk.erase(std::remove_if(m.fsk.begin(), m.fsk.end(), [&](const Impl::ActiveFsk& a) { return a.at + (int64_t)a.s.size() <= m.pos + (int64_t)n; }), m.fsk.end());
    for (const auto& a : m.active) {
        if (!a.inverted) { a.f.render(out, n, t0, m.c.rate); continue; }
        m.tmp.assign(n, cf32(0, 0));
        a.f.render(m.tmp.data(), n, t0, m.c.rate);
        for (size_t i = 0; i < n; i++) out[i] += std::conj(m.tmp[i]);
    }
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
        c.fsk = lorawanFskTestSchedule(plan);
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
