#include "dect2/inmc_gen.h"
#include "dect2/gen_util.h"
#include "dect2/inmc_rx.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <random>

namespace dect2 {

using inmc::kFrameBytes;

namespace {

std::vector<uint8_t> bytesOf(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

constexpr int kChunk = 48;      // text bytes per EGC packet in the test signal

} // namespace

InmcFrameSource::InmcFrameSource(uint32_t seed, uint32_t firstFrame, int les, int channelType) : frame_(firstFrame), les_(les), channelType_(channelType) {
    std::mt19937 rng(seed * 7919u + 17u);
    const uint8_t lesByte = (uint8_t)((3 << 6) | (les_ == 44 ? 4 : les_));    // IOR and a station: the guess for the first address byte (see inmc_pkt.h)
    auto msg = [&](uint8_t svc, int prio, int pres, const std::string& text, uint8_t areaByte) {
        InmcTestMessage m;
        m.id = (uint16_t)(1000 + rng() % 8000);
        m.service = svc; m.priority = prio; m.presentation = pres;
        m.address.assign((size_t)inmc::addressLength(svc), 0);
        m.address[0] = lesByte;
        if (m.address.size() > 1) m.address[1] = areaByte;
        for (size_t i = 2; i < m.address.size(); i++) m.address[i] = (uint8_t)(rng() & 0x7F);
        m.text = text;
        msgs_.push_back(m);
    };
    msg(0x31, 1, 0,
        "NAVAREA IX 231/26\r\nPERSIAN GULF.\r\nSTRAIT OF HORMUZ.\r\nTRAFFIC SEPARATION SCHEME OFF RAS AL KHAIMAH.\r\n"
        "DREDGING OPERATIONS IN PROGRESS WITHIN 2 MILES OF 25-34.2N 056-24.6E. VESSELS REQUESTED TO KEEP CLEAR AND PASS AT SLOW SPEED.\r\n"
        "CANCEL THIS MESSAGE 15 DAYS AFTER DATE OF ISSUE.\r\n", 9);
    msg(0x31, 1, 0,
        "METAREA IX\r\nGALE WARNING 118/26\r\nNORTHERLY WINDS 7 TO 8 BEAUFORT EXPECTED IN THE GULF OF OMAN AND THE STRAIT OF HORMUZ "
        "WITHIN 24 HOURS. SEA ROUGH TO VERY ROUGH. VISIBILITY REDUCED IN BLOWING DUST.\r\n", 9);
    msg(0x34, 2, 0,
        "SAR COORDINATION\r\nMRCC MUSCAT\r\nDHOW REPORTED OVERDUE ON PASSAGE FROM SUR TO KARACHI WITH 6 PERSONS ON BOARD. LAST KNOWN POSITION "
        "22-10N 060-05E. ALL SHIPS IN THE AREA ARE REQUESTED TO KEEP A SHARP LOOKOUT AND REPORT ANY SIGHTING TO MRCC MUSCAT.\r\n", 4);
    msg(0x31, 0, 0,
        "METAREA IX\r\nFORECAST FOR THE GULF OF OMAN AND THE ARABIAN SEA VALID 24 HOURS.\r\nGULF OF OMAN: NORTHEAST WINDS 15 TO 20 KNOTS, "
        "BECOMING NORTH 20 TO 25 KNOTS IN THE EVENING. SEAS 6 TO 8 FEET. HAZE, VISIBILITY 3 TO 5 NM.\r\n"
        "NORTHERN ARABIAN SEA: SOUTHWEST WINDS 20 TO 25 KNOTS WITH GUSTS TO 30 KNOTS. SEAS 8 TO 10 FEET, SWELL 7 FEET FROM THE SOUTHWEST. "
        "SCATTERED SHOWERS AND ISOLATED THUNDERSTORMS NEAR THE COAST OF PAKISTAN.\r\nOUTLOOK: WINDS EASING DURING THE NEXT 48 HOURS.\r\n", 9);
    msg(0x02, 0, 6, "FLEETNET OPERATIONS BULLETIN 14. NO REPLY REQUIRED. PORT CALL SCHEDULE FOR DUBAI AND FUJAIRAH UNCHANGED.\r\n", 0);

    // every message becomes packets of up to 48 text bytes: the first one 0xB1, the next ones alternate 0xB2 and 0xB1
    for (auto& m : msgs_) {
        const std::vector<uint8_t> raw = m.presentation == 6 ? inmc::textToIta2(m.text) : bytesOf(m.text);
        const int n = (int)((raw.size() + kChunk - 1) / kChunk);
        for (int i = 0; i < n; i++) {
            inmc::EgcPacket p;
            p.desc = (i % 2 == 0) ? 0xB1 : 0xB2;
            p.service = m.service;
            p.continuation = i + 1 < n;
            p.priority = m.priority;
            p.repetition = 0;
            p.msgId = m.id;
            p.packetNo = i + 1;
            p.presentation = m.presentation;
            p.address = m.address;
            p.payload.assign(raw.begin() + i * kChunk, raw.begin() + std::min<size_t>(raw.size(), (size_t)(i + 1) * kChunk));
            packets_.push_back(inmc::buildEgc(p));
        }
    }
}

void InmcFrameSource::next(uint8_t f[kFrameBytes]) {
    std::fill(f, f + kFrameBytes, 0);
    int pos = 0;
    inmc::BulletinBoard bb;
    bb.frameNo = (uint16_t)frame_;
    bb.les = les_;
    bb.channelType = channelType_;
    const auto b = inmc::buildBulletinBoard(bb);
    std::copy(b.begin(), b.end(), f + pos); pos += (int)b.size();
    uint8_t slots[28];
    for (int i = 0; i < 28; i++) slots[i] = (uint8_t)((i + frame_) & 3);
    const auto s = inmc::buildSignalling(0xE0, 1626.5 + 0.0025 * 120, slots);
    std::copy(s.begin(), s.end(), f + pos); pos += (int)s.size();
    for (int k = 0; k < 3; k++) {     // up to three EGC packets per frame, so a long message spans frames
        const auto& p = packets_[cursor_];
        if (pos + (int)p.size() > kFrameBytes - 1) break;
        std::copy(p.begin(), p.end(), f + pos); pos += (int)p.size();
        cursor_ = (cursor_ + 1) % packets_.size();
    }
    frame_++;
}

namespace {

// root raised cosine, roll-off 1, as a function of t in symbol periods (inmarsatc demodulator RRC alpha = 1)
double rrc(double t) {
    const double a = 1.0;
    if (std::fabs(t) < 1e-9) return 1.0 - a + 4.0 * a / M_PI;
    if (std::fabs(std::fabs(4.0 * a * t) - 1.0) < 1e-9)
        return a / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * a)));
    return (std::sin(M_PI * t * (1 - a)) + 4 * a * t * std::cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - (4 * a * t) * (4 * a * t)));
}

class Gen : public InmcSynth {
public:
    explicit Gen(const InmcGenConfig& c) : c_(c), src_(c.seed, c.firstFrame, c.les, c.channelType), noise_(c.seed * 31u + 5u) {
        for (int i = 0; i <= 2 * kSpan * kSps; i++) g_[i] = (float)rrc((double)(i - kSpan * kSps) / kSps);
        double e = 0;
        for (int i = 0; i <= 2 * kSpan * kSps; i++) e += (double)g_[i] * g_[i];
        ps_ = e / kSps;                                   // mean power of the waveform for unit random symbols
        const double es = ps_ / inmc::kSymbolRate;       // energy per symbol
        const double esn0 = std::pow(10.0, (c.ebn0Db - 10.0 * std::log10(2.0)) / 10.0);
        n0_ = c.noiseless ? 0.0 : es / esn0;
        const double pn = n0_ * c.rate;                   // noise power in the sampled band
        sigma_ = (float)std::sqrt(pn / 2.0);
        scale_ = (float)(c.rms / std::sqrt(ps_ + pn));
        if (c.sigScale > 0) { scale_ = (float)c.sigScale; sigma_ = 0; }
        step_ = (double)kBaseRate / (c.rate * (1.0 + c.sroPpm * 1e-6));
        w_ = 1.0;
        setStep(0);
        base_.assign(4, cf32(0, 0));
        baseStart_ = -4;                                  // base_[0] is base sample index baseStart_
    }
    double sampleRate() const override { return c_.rate; }
    const InmcFrameSource& source() const override { return src_; }
    float signalScale() const { return scale_; }

    void generate(cf32* out, size_t n) override {
        size_t i = 0;
        while (i < n) {
            const size_t blk = std::min<size_t>(n - i, 256);
            // time-varying carrier: one step per block
            setStep(t_);
            const double need = pos_ + step_ * (double)blk + 3.0;
            ensureBase((long)need + 1);
            for (size_t k = 0; k < blk; k++) {
                const long i0 = (long)std::floor(pos_);
                const float mu = (float)(pos_ - (double)i0);
                const cf32 y0 = baseAt(i0 - 1), y1 = baseAt(i0), y2 = baseAt(i0 + 1), y3 = baseAt(i0 + 2);
                // cubic Lagrange
                const float c0 = -mu * (mu - 1) * (mu - 2) / 6, c1 = (mu + 1) * (mu - 1) * (mu - 2) / 2,
                            c2 = -(mu + 1) * mu * (mu - 2) / 2, c3 = (mu + 1) * mu * (mu - 1) / 6;
                cf32 v = y0 * c0 + y1 * c1 + y2 * c2 + y3 * c3;
                out[i + k] = v * cf32((float)w_.real(), (float)w_.imag()) * scale_;
                w_ *= stepW_;
                pos_ += step_;
            }
            const double m = std::abs(w_);
            w_ /= m;
            if (sigma_ > 0) noise_.add(out + i, blk, sigma_ * scale_);
            t_ += (double)blk / c_.rate;
            i += blk;
        }
    }

private:
    static constexpr int kSps = 40, kSpan = 4;
    static constexpr int kBaseRate = 48000;

    void setStep(double t) {
        const double f = c_.offsetHz + c_.cfoHz + c_.driftHzS * t;
        stepW_ = std::polar(1.0, 2.0 * M_PI * f / c_.rate);
    }
    cf32 baseAt(long idx) const { return base_[(size_t)(idx - baseStart_)]; }
    float symbol(long k) {          // symbol k as +-1, from the frame stream
        while (symEnd_ <= k) {
            if (bitPos_ == inmc::kFrameSyms) {
                uint8_t f[kFrameBytes];
                src_.next(f);
                inmc::encodeFrame(f, bits_);
                bitPos_ = first_ ? (int)(c_.startSkip % inmc::kFrameSyms) : 0;
                first_ = false;
            }
            symq_.push_back(((bits_[bitPos_++] != 0) != c_.invert) ? 1.f : -1.f);
            symEnd_++;
        }
        return symq_[(size_t)(k - symStart_)];
    }
    void ensureBase(long upTo) {
        // base samples up to index upTo; trim what the interpolator no longer needs
        const long keepFrom = (long)std::floor(pos_) - 2;
        if (keepFrom > baseStart_ + 4096) {
            const size_t cut = (size_t)(keepFrom - baseStart_);
            base_.erase(base_.begin(), base_.begin() + (long)cut);
            baseStart_ += (long)cut;
        }
        while (baseStart_ + (long)base_.size() <= upTo) {
            const long n = baseStart_ + (long)base_.size();
            // symbol j is centred on base sample 40 * j; the filter reaches kSpan symbols to each side
            const long jLo = (long)std::ceil((double)(n - kSpan * kSps) / kSps), jHi = (long)std::floor((double)(n + kSpan * kSps) / kSps);
            cf32 acc(0, 0);
            float re = 0;
            for (long j = std::max(jLo, 0L); j <= jHi; j++) re += symbol(j) * g_[n - j * kSps + kSpan * kSps];
            acc = cf32(re, 0);
            base_.push_back(acc);
            const long drop = (long)std::floor((double)(n - kSpan * kSps - 2 * kSps) / kSps);
            while (symStart_ < drop && !symq_.empty()) { symq_.pop_front(); symStart_++; }
        }
    }

    InmcGenConfig c_;
    InmcFrameSource src_;
    genutil::NoiseSource noise_;
    float g_[2 * kSpan * kSps + 1];
    double ps_ = 0, n0_ = 0, step_ = 1, pos_ = 0, t_ = 0;
    float sigma_ = 0, scale_ = 1;
    std::complex<double> w_{1.0, 0.0}, stepW_{1.0, 0.0};
    std::vector<cf32> base_;
    long baseStart_ = 0;
    std::deque<float> symq_;
    long symStart_ = 0, symEnd_ = 0;
    uint8_t bits_[inmc::kFrameSyms];
    int bitPos_ = inmc::kFrameSyms;
    bool first_ = true;
};

// the main channel with noise, plus the others without noise at the same signal level
class MultiGen : public InmcSynth {
public:
    explicit MultiGen(const InmcGenConfig& c) : main_(std::make_unique<Gen>(c)) {
        static const double off[3] = {-20000, -90000, 30000}, cfo[3] = {1200, -2300, 700};
        static const int les[3] = {4, 12, 21};
        static const uint32_t skip[3] = {3000, 7100, 5200};
        for (int i = 0; i < c.extraChannels && i < 3; i++) {
            InmcGenConfig e = c;
            e.extraChannels = 0;
            e.offsetHz = off[i];
            e.cfoHz = c.cfoHz + cfo[i];
            e.seed = c.seed + 100u * (uint32_t)(i + 1);
            e.firstFrame = 5000u + 700u * (uint32_t)i;
            e.les = les[i]; e.channelType = 2;
            e.sigScale = main_->signalScale();
            e.startSkip = skip[i];
            e.driftHzS = 0;
            extra_.push_back(std::make_unique<Gen>(e));
        }
    }
    double sampleRate() const override { return main_->sampleRate(); }
    const InmcFrameSource& source() const override { return main_->source(); }
    void generate(cf32* out, size_t n) override {
        main_->generate(out, n);
        tmp_.resize(n);
        for (auto& g : extra_) {
            g->generate(tmp_.data(), n);
            for (size_t i = 0; i < n; i++) out[i] += tmp_[i];
        }
    }
    size_t extraCount() const override { return extra_.size(); }
    const InmcFrameSource& extraSource(size_t i) const override { return extra_[i]->source(); }
private:
    std::unique_ptr<Gen> main_;
    std::vector<std::unique_ptr<Gen>> extra_;
    std::vector<cf32> tmp_;
};

} // namespace

std::unique_ptr<InmcSynth> makeInmcGenerator(const InmcGenConfig& c) {
    if (c.rate < 48000) return nullptr;
    if (c.extraChannels > 0) return std::make_unique<MultiGen>(c);
    return std::make_unique<Gen>(c);
}

std::unique_ptr<ModeSynth> makeInmcSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    InmcGenConfig c;
    c.rate = sampleRate;
    c.seed = cfg.modeOpt[0] ? (uint32_t)cfg.modeOpt[0] : 1u;
    c.invert = cfg.modeOpt[1] == 1;
    c.firstFrame = cfg.modeOpt[2] ? (uint32_t)cfg.modeOpt[2] : 1000u;
    c.ebn0Db = cfg.modeVal[0] != 0 ? cfg.modeVal[0] : cfg.snrDb - 20.0;
    c.driftHzS = cfg.modeVal[1];
    c.extraChannels = std::max(0, std::min(3, cfg.modeOpt[3]));
    c.cfoHz = cfg.cfoHz;
    c.sroPpm = cfg.sroPpm;
    c.offsetHz = -inmcTuning().tuneOffsetHz;
    return makeInmcGenerator(c);
}

} // namespace dect2
