// CDR test signal (see cdr_gen.h): multiplex (GY/T 268.2), channel coding, sub-carrier matrix, OFDM with the windowed guard interval,
// sub-frame allocation (GY/T 268.1), then resampled to the source's rate with the noise and the carrier offset of the SynthConfig.
#include "dect2/cdr_gen.h"
#include "dect2/cdr_ldpc.h"
#include "dect2/exact_resampler.h"
#include "dect2/fftutil.h"
#include "dect2/gen_util.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

using namespace cdr;

CdrTxConfig cdrTxConfigFrom(const SynthConfig& sc) {
    CdrTxConfig c;
    c.tm = sc.modeOpt[0] >= 1 && sc.modeOpt[0] <= 3 ? sc.modeOpt[0] : 1;
    static const int sm[7] = {1, 1, 2, 9, 10, 22, 23};
    c.sm = sm[std::max(0, std::min(6, sc.modeOpt[1]))];
    c.msdMod = std::max(0, std::min(2, sc.modeOpt[2]));
    static const int rate[5] = {3, 0, 1, 2, 3};
    c.rate = rate[std::max(0, std::min(4, sc.modeOpt[3]))];
    c.sdiMod = std::max(0, std::min(2, sc.modeOpt[4]));
    c.alloc = sc.modeOpt[5] >= 1 && sc.modeOpt[5] <= 3 ? sc.modeOpt[5] : 1;
    c.textService = sc.modeOpt[7] == 0;
    return c;
}

struct CdrTransmitter::Impl {
    CdrTxConfig cfg;
    CdrTxPlan plan;
    std::shared_ptr<const Layout> lay;
    bool ok = false;
    std::unique_ptr<Fft> fftS, fftB;
    std::vector<std::vector<cf32>> logical;      // the four logical frames of the current super frame: data elements
    int slot = 16;                               // next physical sub-frame of the super frame
    uint32_t frameNo = 0;                        // logical frames made so far
    uint32_t startTime = 0;
    std::vector<cf32> tail;                      // windowed guard interval running into the next sub-frame
    float scale = 1.f;

    explicit Impl(const CdrTxConfig& c) : cfg(c) {
        lay = layoutFor(cfg.tm, cfg.sm);
        if (!lay || cfg.alloc < 1 || cfg.alloc > 3 || cfg.rate < 0 || cfg.rate > 3) return;
        fftS = std::make_unique<Fft>(lay->tp->ns);
        fftB = std::make_unique<Fft>(lay->tp->nb);
        makePlan();
        // mean power of an OFDM symbol with 1/sqrt(Ns): data cells 1, SI cells and pilots 2
        double e = 0;
        for (uint8_t k : lay->kind) e += k == kElemData ? 1.0 : 2.0;
        e /= (double)lay->sn * lay->tp->ns;
        scale = (float)(1.0 / std::sqrt(e));
        ok = plan.capacityBytes > 0;
    }

    void makePlan() {
        plan.capacityBytes = lay->msdBits(cfg.msdMod, cfg.rate) / 8;
        plan.sdiBits = lay->sdiBits(cfg.sdiMod);
        const double avail = std::max(100.0, plan.capacityBytes - 470.0 - (cfg.textService ? cfg.text.size() + 20.0 : 0.0));
        plan.rateA = (int)(avail * 0.55 * 8 / 0.64 / 100) * 100;
        plan.rateB = (int)(avail * 0.35 * 8 / 0.64 / 100) * 100;
        AudioStreamDesc a;
        a.algo = 0; a.rate100 = plan.rateA / 100; a.sampleRateCode = 7; a.channelsCode = 2; a.language = "chi";
        AudioStreamDesc b = a;
        b.rate100 = plan.rateB / 100; b.sampleRateCode = 5; b.channelsCode = 1;
        plan.streamA = {a};
        plan.streamB = {b};
        SmctEntry e;
        e.smfId = 1; e.txMode = 0xF;
        e.services = {cfg.serviceA, cfg.serviceB};
        if (cfg.textService) e.services.push_back(cfg.serviceText);
        plan.smct.frames = {e};
        plan.nit.country = "CHN";
        plan.nit.networkId = cfg.networkId;
        plan.nit.freqs = {cfg.freq10Hz};
        plan.nit.name.assign(cfg.network.begin(), cfg.network.end());
    }

    // filler bytes of an audio unit: not DRA+ (see cdr_gen.h)
    static void filler(std::vector<uint8_t>& d, size_t n, uint32_t seed) {
        uint32_t s = seed * 2654435761u + 0x9E3779B9u;
        if (!s) s = 1;
        d.resize(n);
        for (size_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; d[i] = (uint8_t)(s >> 24); }
    }

    MuxSubFrame audioSubFrame(const AudioStreamDesc& st, int rate, int units, int unitTicks, uint32_t id) {
        MuxSubFrame s;
        s.hasStartTime = true; s.startTime = startTime;
        s.hasAudio = true; s.hasExt = true;
        s.streams = {st};
        const int total = rate * 64 / 800;                  // bytes in 640 ms
        for (int i = 0; i < units; i++) {
            AudioUnit u;
            u.stream = 0;
            u.relTime = i * unitTicks;
            const int n = total / units + (i < total % units ? 1 : 0);
            filler(u.data, (size_t)n, cfg.seed * 7919u + id * 104729u + frameNo * 31u + (uint32_t)i);
            s.audio.push_back(std::move(u));
        }
        return s;
    }

    std::vector<uint8_t> serviceFrame() {
        std::vector<std::vector<uint8_t>> subs;
        subs.push_back(subFrameBytes(audioSubFrame(plan.streamA[0], plan.rateA, 30, 480, 1)));   // 1024 samples at 48 kHz = 480 x 1/22500 s
        subs.push_back(subFrameBytes(audioSubFrame(plan.streamB[0], plan.rateB, 20, 720, 2)));   // 1024 samples at 32 kHz
        if (cfg.textService) {
            MuxSubFrame s;
            s.hasStartTime = true; s.startTime = startTime;
            s.hasData = true;
            DataUnit u;
            u.type = 160;
            u.data.assign(cfg.text.begin(), cfg.text.end());
            s.data.push_back(u);
            subs.push_back(subFrameBytes(s));
        }
        ServiceFrameHeader h;
        h.smfId = 1;
        return serviceFrameBytes(h, subs, (size_t)plan.capacityBytes);
    }

    void buildLogical(std::vector<cf32>& v) {
        const Layout& L = *lay;
        v.assign((size_t)L.elems(), cf32(0, 0));
        // service description channel: scrambling, 1/4 convolutional code, bit interleaver, mapping
        {
            const std::vector<uint8_t> ctl = controlFrameBytes({smctBytes(plan.smct), nitBytes(plan.nit)});
            const int T = plan.sdiBits;
            std::vector<uint8_t> bits((size_t)T, 1);
            for (int i = 0; i < T && i / 8 < (int)ctl.size(); i++) bits[(size_t)i] = (ctl[(size_t)(i / 8)] >> (7 - i % 8)) & 1;
            scrambleBits(bits.data(), T);
            std::vector<uint8_t> code((size_t)(4 * (T + 6))), il(code.size());
            convEncode(bits.data(), T, code.data());
            const std::vector<int>& R = interleaver((int)code.size());
            for (size_t n = 0; n < code.size(); n++) il[n] = code[(size_t)R[n]];
            const int mb = cdr::modBits(cfg.sdiMod);
            for (size_t i = 0; i < L.sdisPos.size(); i++) v[(size_t)L.sdisPos[i]] = mapBits(&il[i * (size_t)mb], cfg.sdiMod);
        }
        // service data channel: scrambling, LDPC, mapping (the sub-carrier interleaving is in msdsPos)
        {
            const std::vector<uint8_t> sf = serviceFrame();
            const int nbits = plan.capacityBytes * 8;
            std::vector<uint8_t> bits((size_t)nbits);
            for (int i = 0; i < nbits; i++) bits[(size_t)i] = (sf[(size_t)(i / 8)] >> (7 - i % 8)) & 1;
            scrambleBits(bits.data(), nbits);
            const CdrLdpc& code = cdrLdpc(cfg.rate);
            const int cw = L.codewordsFor(cfg.msdMod);
            std::vector<uint8_t> words((size_t)cw * kLdpcBits);
            for (int c = 0; c < cw; c++) code.encode(&bits[(size_t)c * (size_t)code.k()], &words[(size_t)c * kLdpcBits]);
            const int mb = cdr::modBits(cfg.msdMod);
            for (size_t m = 0; m < L.msdsPos.size(); m++) v[(size_t)L.msdsPos[m]] = mapBits(&words[m * (size_t)mb], cfg.msdMod);
        }
        frameNo++;
        startTime += 14400;                                   // 640 ms in 1/22500 s
    }

    void synthSubframe(int f, int s, std::vector<cf32>& out) {
        const Layout& L = *lay;
        const cdr::TxParams& tp = *L.tp;
        int p, q;
        physToLogical(cfg.alloc, f, s, p, q);
        SysInfo si;
        si.nominal = nominalCode(L.spec->nominalKhz);
        si.spec = cfg.sm;
        si.frame = f; si.subframe = s;
        si.alloc = cfg.alloc;
        si.sdiMod = cfg.sdiMod; si.msdMod = cfg.msdMod;
        si.uniform = true;
        si.rateHi = cfg.rate;
        cf32 siSym[kSiSymbols];
        siSymbols(si, siSym);
        const int sn = L.sn, cols = L.cols;
        const std::vector<cf32>& lv = logical[(size_t)p];
        std::vector<cf32> buf((size_t)(kSubframeLen + tp.tg), cf32(0, 0));
        for (size_t i = 0; i < tail.size() && i < buf.size(); i++) buf[i] = tail[i];
        auto place = [&](int start, int tr, const std::vector<cf32>& body) {
            const int tu = (int)body.size(), tg = tp.tg;
            for (int t = 0; t < tr + tu + tg; t++) {
                double w = 1.0;
                if (t < tg) w = 0.5 + 0.5 * std::cos(M_PI + M_PI * (t + 0.5) / tg);
                else if (t >= tr + tu) w = 0.5 + 0.5 * std::cos(M_PI * (t - tr - tu + 0.5) / tg);
                const int k = ((t - tr) % tu + tu) % tu;
                buf[(size_t)(start + t)] += body[(size_t)k] * (float)w;
            }
        };
        // beacon: two copies of the sync signal and a cyclic prefix
        {
            std::vector<cf32> x((size_t)tp.nb, cf32(0, 0));
            for (size_t n = 0; n < L.syncCarrier.size(); n++) x[(size_t)((L.syncCarrier[n] + tp.nb) % tp.nb)] = L.beaconSeq[n];
            fftB->inverse(x.data());
            const float g = scale / std::sqrt((float)tp.nb);
            std::vector<cf32> body((size_t)tp.tu);
            for (int i = 0; i < tp.tu; i++) body[(size_t)i] = x[(size_t)(i % tp.nb)] * g;
            place(0, tp.tbcp, body);
        }
        std::vector<cf32> x((size_t)tp.ns);
        for (int a = 0; a < sn; a++) {
            std::fill(x.begin(), x.end(), cf32(0, 0));
            for (int c = 0; c < cols; c++) {
                const size_t e = (size_t)(a * cols + c);
                cf32 z;
                if (L.kind[e] == kElemSi) z = siSym[L.siSym[e]];
                else if (L.kind[e] == kElemPilot) z = L.pilot[e];
                else z = lv[(size_t)(q * sn * cols) + e];
                x[(size_t)((L.carrier[(size_t)c] + tp.ns) % tp.ns)] = z;
            }
            fftS->inverse(x.data());
            const float g = scale / std::sqrt((float)tp.ns);
            for (auto& v : x) v *= g;
            place(tp.tb + a * tp.ts, tp.tcp, x);
        }
        out.assign(buf.begin(), buf.begin() + kSubframeLen);
        tail.assign(buf.begin() + kSubframeLen, buf.end());
    }

    void next(std::vector<cf32>& out) {
        if (slot >= 16) {
            logical.assign(4, {});
            for (int p = 0; p < 4; p++) buildLogical(logical[(size_t)p]);
            slot = 0;
        }
        synthSubframe(slot / 4, slot % 4, out);
        slot++;
    }
};

CdrTransmitter::CdrTransmitter(const CdrTxConfig& cfg) : p_(std::make_unique<Impl>(cfg)) {}
CdrTransmitter::~CdrTransmitter() = default;
bool CdrTransmitter::ok() const { return p_->ok; }
const CdrTxConfig& CdrTransmitter::config() const { return p_->cfg; }
const CdrTxPlan& CdrTransmitter::plan() const { return p_->plan; }
void CdrTransmitter::nextSubframe(std::vector<cf32>& out) {
    if (!p_->ok) { out.assign(kSubframeLen, cf32(0, 0)); return; }
    p_->next(out);
}

// ---------------------------------------------------------------- the synthetic source

namespace {

class CdrSynth : public ModeSynth {
public:
    CdrSynth(const SynthConfig& sc, double rate) : sc_(sc), rate_(rate), tx_(cdrTxConfigFrom(sc)) {
        rs_.configure(kFs, rate);
        const CdrTxConfig& c = tx_.config();
        const SpectrumMode* sp = spectrumMode(c.sm);
        host_ = sc.modeOpt[6] == 0 && sp && sp->innerKhz > 0;
        hostDev_ = sp && sp->innerKhz >= 150 ? 50000.0 : 30000.0;      // stereo programme between +-150 kHz, mono between +-100 kHz
        digAmp_ = host_ ? 0.08f : 0.2f;
        // noise: snrDb in the bandwidth of the active carriers
        const auto lay = layoutFor(c.tm, c.sm);
        const double bw = lay ? lay->cols * lay->tp->df : 100e3;
        const double pSig = (double)digAmp_ * digAmp_;
        noiseSigma_ = sc.snrDb >= 100 ? 0.f : (float)std::sqrt(pSig / std::pow(10.0, sc.snrDb / 10.0) * rate / bw / 2.0);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        while (q_.size() - qpos_ < n) {
            if (qpos_ > 0) { q_.erase(q_.begin(), q_.begin() + (long)qpos_); qpos_ = 0; }
            tx_.nextSubframe(sf_);
            for (auto& v : sf_) v *= digAmp_;
            rs_.process(sf_.data(), sf_.size(), q_);
        }
        const double w = 2 * M_PI * sc_.cfoHz / rate_;
        for (size_t i = 0; i < n; i++) {
            cf32 v = q_[qpos_ + i];
            if (host_) {
                const double m = 0.6 * std::sin(tone1_) + 0.4 * std::sin(tone2_);
                tone1_ += 2 * M_PI * 1000.0 / rate_; tone2_ += 2 * M_PI * 2300.0 / rate_;
                if (tone1_ > 2 * M_PI) tone1_ -= 2 * M_PI;
                if (tone2_ > 2 * M_PI) tone2_ -= 2 * M_PI;
                fm_ += 2 * M_PI * hostDev_ * m / rate_;
                if (fm_ > M_PI) fm_ -= 2 * M_PI; else if (fm_ < -M_PI) fm_ += 2 * M_PI;
                v += std::polar(digAmp_ * (float)std::sqrt(10.0), (float)fm_);
            }
            if (w != 0) {
                v *= cf32((float)std::cos(ph_), (float)std::sin(ph_));
                ph_ += w;
                if (ph_ > M_PI) ph_ -= 2 * M_PI; else if (ph_ < -M_PI) ph_ += 2 * M_PI;
            }
            out[i] = v;
        }
        qpos_ += n;
        if (noiseSigma_ > 0) noise_.add(out, n, noiseSigma_);
    }
private:
    SynthConfig sc_;
    double rate_;
    CdrTransmitter tx_;
    ExactResampler rs_;
    std::vector<cf32> sf_, q_;
    size_t qpos_ = 0;
    bool host_ = false;
    double hostDev_ = 50000, fm_ = 0, tone1_ = 0, tone2_ = 0, ph_ = 0;
    float digAmp_ = 0.2f, noiseSigma_ = 0;
    genutil::NoiseSource noise_{7};
};

} // namespace

std::unique_ptr<ModeSynth> makeCdrSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<CdrSynth>(cfg, sampleRate);
}

} // namespace dect2
