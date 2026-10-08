// Iridium test signal: constellation model, burst schedule, DQPSK burst shaping and rendering (see iridium_gen.h).
#include "dect2/iridium_gen.h"
#include "dect2/gen_util.h"
#include "dect2/iridium_frame.h"
#include "dect2/iridium_phy.h"
#include "dect2/iridium_sbd.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <random>
#include <string>

namespace dect2 {

namespace {

constexpr int kOs = 32;                      // samples per symbol of the shaped baseband
constexpr double kFsb = iridium::kSymbolRate * kOs;
const cf32 kPt[4] = {cf32(M_SQRT1_2, M_SQRT1_2), cf32(-M_SQRT1_2, M_SQRT1_2), cf32(-M_SQRT1_2, -M_SQRT1_2), cf32(M_SQRT1_2, -M_SQRT1_2)};

const std::vector<float>& txTaps() {
    static const std::vector<float> t = [] {
        std::vector<float> h = iridium::rrcTaps(kOs, iridium::kRrcAlpha, 5);
        for (auto& v : h) v *= std::sqrt((float)kOs);    // mean power of the burst = amplitude^2
        return h;
    }();
    return t;
}

struct Shaped {
    std::vector<cf32> s;                     // baseband at kFsb; symbol k peaks at index delay + k * kOs
    int delay = 0;
    double ts = 0, f = 0, dr = 0, phase = 0;
    double endSec() const { return ts + (s.size() - delay) / kFsb; }
};

Shaped shapeBurst(const IridiumTestBurst& b) {
    Shaped sh;
    const std::vector<uint8_t> sym = iridium::burstSymbols(b.bits, b.downlink, b.preamble);
    const std::vector<float>& h = txTaps();
    const int L = (int)h.size();
    sh.delay = (L - 1) / 2;
    sh.s.assign(sym.size() * kOs + L, cf32(0, 0));
    for (size_t k = 0; k < sym.size(); k++) {
        const cf32 p = kPt[sym[k]] * b.amplitude;
        cf32* d = &sh.s[k * kOs];
        for (int i = 0; i < L; i++) d[i] += p * h[i];
    }
    sh.ts = b.startSec; sh.f = b.freqHz; sh.dr = b.dopplerRate; sh.phase = b.phase;
    return sh;
}

// adds one shaped burst to out (sample i at t0 + i dt)
void renderShaped(const Shaped& sh, double t0, double dt, cf32* out, size_t n) {
    const double tBeg = sh.ts - sh.delay / kFsb, tEnd = sh.ts + (sh.s.size() - 3 - sh.delay) / kFsb;
    long i0 = (long)std::ceil((tBeg - t0) / dt), i1 = (long)std::floor((tEnd - t0) / dt);
    i0 = std::max(i0, 0L); i1 = std::min(i1, (long)n - 1);
    if (i1 < i0) return;
    const double tau0 = t0 + i0 * dt - sh.ts;
    const double phi0 = sh.phase + 2 * M_PI * (sh.f * tau0 + 0.5 * sh.dr * tau0 * tau0);
    std::complex<double> ph = std::polar(1.0, phi0);
    std::complex<double> step = std::polar(1.0, 2 * M_PI * (sh.f * dt + sh.dr * tau0 * dt + 0.5 * sh.dr * dt * dt));
    const std::complex<double> chirp = std::polar(1.0, 2 * M_PI * sh.dr * dt * dt);
    const double du = dt * kFsb;
    double u = (tau0 * kFsb) + sh.delay;
    const cf32* s = sh.s.data();
    for (long i = i0; i <= i1; i++, u += du) {
        const long k = (long)u;
        const float x = (float)(u - k);
        cf32 v;
        if (k >= 1) {
            const cf32 y0 = s[k - 1], y1 = s[k], y2 = s[k + 1], y3 = s[k + 2];
            // cubic Lagrange
            const float c0 = -x * (x - 1) * (x - 2) / 6, c1 = (x + 1) * (x - 1) * (x - 2) / 2, c2 = -(x + 1) * x * (x - 2) / 2, c3 = (x + 1) * x * (x - 1) / 6;
            v = y0 * c0 + y1 * c1 + y2 * c2 + y3 * c3;
        } else {
            v = s[k] * (1 - x) + s[k + 1] * x;
        }
        out[i] += v * cf32((float)ph.real(), (float)ph.imag());
        ph *= step; step *= chirp;
        if (((i - i0) & 4095) == 4095) { ph /= std::abs(ph); step /= std::abs(step); }
    }
}

// ---------------------------------------------------------------- constellation model
// Iridium: 66 satellites in 6 near-polar planes (86.4 degrees) at 780 km, 11 per plane; co-rotating planes 31.6 degrees apart,
// neighbouring planes phased by half the in-plane spacing (Iridium constellation as published, e.g. Pratt et al., IEEE Comm.
// Surveys 1999). A circular, unperturbed model: good for Doppler and geometry, not for predicting real passes.
constexpr double kRe = 6378.137, kMu = 398600.4418, kAltKm = 780, kWe = 7.2921159e-5, kC = 299792.458;
constexpr double kDeg = M_PI / 180;
constexpr double kObsLat = 25.2048, kObsLon = 55.2708;      // Dubai
constexpr double kRaan0 = kObsLon - 2 * 31.6 - 3;

struct V3 { double x, y, z; };
inline V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double norm(V3 a) { return std::sqrt(dot(a, a)); }

V3 satEcef(int p, int s, double t) {
    const double a = kRe + kAltKm, n = std::sqrt(kMu / (a * a * a));
    const double raan = (kRaan0 + p * 31.6) * kDeg, inc = 86.4 * kDeg;
    const double u = (s * 360.0 / 11 + p * 360.0 / 22) * kDeg + n * t;
    const double x = a * (std::cos(raan) * std::cos(u) - std::sin(raan) * std::sin(u) * std::cos(inc));
    const double y = a * (std::sin(raan) * std::cos(u) + std::cos(raan) * std::sin(u) * std::cos(inc));
    const double z = a * std::sin(u) * std::sin(inc);
    const double th = kWe * t;
    return {x * std::cos(th) + y * std::sin(th), -x * std::sin(th) + y * std::cos(th), z};
}
V3 observer() {
    return {kRe * std::cos(kObsLat * kDeg) * std::cos(kObsLon * kDeg), kRe * std::cos(kObsLat * kDeg) * std::sin(kObsLon * kDeg), kRe * std::sin(kObsLat * kDeg)};
}
inline int satId(int p, int s) { return 2 + p * 16 + s; }
double rangeKm(int p, int s, double t) { return norm(sub(satEcef(p, s, t), observer())); }
double elevation(int p, int s, double t) {
    const V3 o = observer(), r = sub(satEcef(p, s, t), o);
    return std::asin(dot(r, o) / (norm(r) * norm(o))) / kDeg;
}

// point at ground distance dKm and bearing brg (radians) from lat/lon (degrees), on a sphere
void destination(double lat, double lon, double dKm, double brg, double& lat2, double& lon2) {
    const double p1 = lat * kDeg, l1 = lon * kDeg, d = dKm / kRe;
    const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(brg));
    const double l2 = l1 + std::atan2(std::sin(brg) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2));
    lat2 = p2 / kDeg;
    lon2 = std::remainder(l2 / kDeg, 360.0);
}

// 48 spot beams in rings around the sub-point (3, 9, 15, 21 beams): a stand-in for the real beam pattern
void beamCentre(const IridiumSkySat& s, double headingRad, int beam, double& lat, double& lon) {
    static const int cnt[4] = {3, 9, 15, 21};
    static const double rad[4] = {300, 900, 1500, 2100};
    int ring = 0, idx = beam;
    while (ring < 3 && idx >= cnt[ring]) { idx -= cnt[ring]; ring++; }
    const double brg = headingRad + 2 * M_PI * idx / cnt[ring] + ring * 0.3;
    destination(s.lat, s.lon, rad[ring], brg, lat, lon);
}

const char* const kPagerTexts[4] = {
    "MEET AT GATE 4 AT 1530 BRING PASSPORTS",
    "CALL OFFICE ASAP RE SHIPMENT 2231. CUSTOMS NEED THE INVOICE AND THE PACKING LIST BEFORE 1700 TODAY",
    "WX DXB 34C WIND 330/12 VIS 8KM NOSIG",
    "ETA PORT RASHID 0600 LT. CREW 14 ALL WELL. REQUEST FRESH WATER 40T AND PROVISIONS ON ARRIVAL. MASTER",
};
struct AcarsText { char mode; const char* reg; const char* label; char block; const char* text; };
const AcarsText kAcars[3] = {
    {'2', ".A6-EDA", "RA", 'A', "WX DXB 34C WIND 330/12KT VIS 10KM NOSIG QNH 1006"},
    {'2', ".A6-BLK", "C1", 'B', "CONTACT DXB GND 118.6 ON ARRIVAL STAND C14"},
    {'2', ".A7-BCA", "RA", 'C', "REVISED ETA 1645Z FUEL ON BOARD CONFIRM"},
};
const int kPagerRic[4] = {1234567, 2201844, 3349120, 3410002};   // the address field has 22 bits

class IridiumSynth : public ModeSynth {
public:
    IridiumSynth(const SynthConfig& cfg, double rate) : rate_(rate), cfg_(cfg) {
        nSats_ = cfg.modeOpt[0] >= 1 && cfg.modeOpt[0] <= 4 ? cfg.modeOpt[0] : 3;
        seed_ = (uint32_t)cfg.modeOpt[1];
        voice_ = cfg.modeOpt[2] != 1;
        doppler_ = cfg.modeOpt[3] != 1;
        acars_ = cfg.modeOpt[4] != 1;
        centre_ = cfg.modeVal[1] > 0 ? cfg.modeVal[1] * 1e6 : rate >= 9.5e6 ? 1622e6 : 1626.25e6;
        tStart_ = iridiumSkyStart(seed_, nSats_) + cfg.modeVal[2];
        tStartUnix_ = cfg.modeVal[2];
        rng_.seed(seed_ * 7919u + 17u);
        noise_ = genutil::NoiseSource(seed_ + 21);
        const double esn0Db = cfg.modeVal[0] > 0 ? cfg.modeVal[0] - 10 * std::log10(iridium::kSymbolRate) : cfg.snrDb;
        const double esn0 = std::pow(10.0, esn0Db / 10);
        // Es/N0 = A^2 rate / (25k * 2 sigma^2): the burst rms A at most 0.15 (several bursts at once stay below 0.9), the noise sigma
        // at most 0.08 per component
        const double k = std::sqrt(esn0 * iridium::kSymbolRate * 2 / rate);
        sigma_ = std::min(0.08, 0.15 / k);
        amp_ = (float)(sigma_ * k);
        dt_ = 1.0 / (rate * (1 + cfg.sroPpm * 1e-6));
        // channels inside the band (with room for the Doppler shift)
        for (int ch = 0; ch < iridium::kDuplexChannels; ch++)
            if (std::fabs(iridium::channelHz(ch) - centre_) < 0.45 * rate - 45e3) duplex_.push_back(ch);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        std::fill(out, out + n, cf32(0, 0));
        const double t0 = produced_ * dt_, t1 = (produced_ + n) * dt_;
        while (nextFrame_ * 0.09 < t1 + 0.1) scheduleFrame(nextFrame_++);
        for (size_t i = 0; i < pend_.size();) {
            if (pend_[i].ts - 0.001 < t1) renderShaped(pend_[i], t0, dt_, out, n);
            if (pend_[i].endSec() < t1) { pend_[i] = std::move(pend_.back()); pend_.pop_back(); }
            else i++;
        }
        noise_.add(out, n, (float)sigma_);
        produced_ += n;
    }

private:
    bool inBand(double fRel) const { return std::fabs(fRel) < 0.45 * rate_ - 25e3; }
    struct Slot { double t0, t1, f; };
    // false when the burst would collide with one already scheduled (same time, closer than about a channel): this model keeps every burst clean
    bool add(std::vector<uint8_t> bits, int fallbackBits, int preamble, double ts, double fAbs, const IridiumSkySat& s, float amp) {
        if (bits.empty()) {   // the frame layer cannot build this frame: random bits of a typical length
            bits.resize(fallbackBits);
            for (auto& b : bits) b = (uint8_t)(rng_() & 1);
        }
        const double fd = doppler_ ? s.dopplerHz : 0, dr = doppler_ ? s.dopplerRate : 0;
        const double fRel = fAbs + fd + cfg_.cfoHz - centre_;
        if (!inBand(fRel)) return false;
        const double dur = (preamble + iridium::kUwLen + bits.size() / 2 + 2) / iridium::kSymbolRate;
        for (const auto& q : busy_)
            if (ts < q.t1 + 0.0008 && q.t0 < ts + dur + 0.0008 && std::fabs(q.f - fRel) < 45e3) return false;
        busy_.push_back(Slot{ts, ts + dur, fRel});
        IridiumTestBurst b;
        b.bits = std::move(bits);
        b.preamble = preamble;
        b.startSec = ts;
        b.freqHz = fRel;
        b.dopplerRate = dr;
        b.amplitude = amp;
        b.phase = (rng_() & 0xffff) * (2 * M_PI / 65536);
        pend_.push_back(shapeBurst(b));
        return true;
    }
    // Frame layout (approximate): the simplex slot (20.32 ms) first, then 4 uplink and 4 downlink slots of 8.28 ms.
    void scheduleFrame(int F) {
        const double T = F * 0.09;            // TDMA frame: 90 ms
        busy_.erase(std::remove_if(busy_.begin(), busy_.end(), [&](const Slot& q) { return q.t1 < T - 0.01; }), busy_.end());
        std::vector<IridiumSkySat> sky = iridiumSky(tStart_ + T, 1);
        if ((int)sky.size() > nSats_) sky.resize(nSats_);
        std::sort(sky.begin(), sky.end(), [](const IridiumSkySat& a, const IridiumSkySat& b) { return a.id < b.id; });
        const std::vector<IridiumSkySat> ahead = iridiumSky(tStart_ + T + 1, -90);
        // pager messages from the first satellite, a part every 5 frames, on simplex access 11 (first, so it wins the band)
        if (!sky.empty() && F % 5 == 0) {
            const IridiumSkySat& s = sky[0];
            if (msgParts_.empty()) {
                const int m = msgIdx_ % 4;
                msgParts_ = iridiumBuildMsgParts(kPagerRic[m], msgIdx_ % 64, kPagerTexts[m]);
                msgIdx_++;
            }
            if (!msgParts_.empty()) {
                const float amp = (float)(amp_ * std::pow(10.0, -4.0 * (1 - std::sin(s.elevDeg * kDeg)) / 20));
                if (add(msgParts_.front(), 0, iridium::kPreambleLong, T + 0.0002, iridium::channelHz(250), s, amp))
                    msgParts_.erase(msgParts_.begin());
            }
        }
        // ACARS to an aircraft as short burst data: an IDA fragment a frame in the second downlink slot, one channel per message
        if (acars_ && !sky.empty() && !duplex_.empty()) {
            if (idaParts_.empty() && F % 25 == 0) {
                const AcarsText& a = kAcars[acarsIdx_ % 3];
                idaParts_ = iridiumSplitIda(iridiumBuildSbdAcars(a.mode, a.reg, 0x15, a.label, a.block, a.text));
                idaCtr_ = 0;
                idaCh_ = duplex_[(acarsIdx_ * 53 + 7) % duplex_.size()];
                idaSat_ = sky[0].id;
                acarsIdx_++;
            }
            if (!idaParts_.empty()) {
                const IridiumSkySat* s = nullptr;
                for (const auto& q : sky) if (q.id == idaSat_) s = &q;
                if (!s) idaParts_.clear();     // the satellite has set: the message is lost, as it would be
                else {
                    const bool more = idaParts_.size() > 1;
                    const float amp = (float)(amp_ * std::pow(10.0, -4.0 * (1 - std::sin(s->elevDeg * kDeg)) / 20));
                    if (add(iridiumBuildIda(idaParts_.front(), idaCtr_, more), 358, iridium::kPreambleShort, T + 0.0558 + 0.00828, iridium::channelHz(idaCh_), *s, amp)) {
                        idaParts_.erase(idaParts_.begin());
                        idaCtr_ = (idaCtr_ + 1) % 8;
                    }
                }
            }
        }
        for (size_t j = 0; j < sky.size(); j++) {
            const IridiumSkySat& s = sky[j];
            const float amp = (float)(amp_ * std::pow(10.0, -4.0 * (1 - std::sin(s.elevDeg * kDeg)) / 20));
            // heading of the sub-point (for the beam pattern)
            double heading = 0;
            for (const auto& q : ahead)
                if (q.id == s.id) heading = std::atan2((q.lon - s.lon) * std::cos(s.lat * kDeg), q.lat - s.lat);
            // ring alert (fills the simplex slot): one beam per frame, the 48 beams in turn; satellite and beam positions alternate.
            // Each satellite on its own simplex access here (7, 4, 1, 10), so that their Doppler shifts never make them collide.
            const int beam = (F + 13 * s.id) % 48;
            double lat = s.lat, lon = s.lon, alt = s.altKm;
            if (F % 2 == 0) { beamCentre(s, heading, beam, lat, lon); alt = 0; }
            std::vector<uint32_t> tmsi;
            if ((rng_() % 100) < 15) { const int np = 1 + (int)(rng_() % 2); for (int i = 0; i < np; i++) tmsi.push_back(rng_()); }
            static const int raCh[4] = {246, 243, 240, 249};
            add(iridiumBuildIra(s.id, beam, lat, lon, alt, tmsi), 864, iridium::kPreambleLong, T + 0.0002, iridium::channelHz(raCh[j & 3]), s, amp);
            // broadcast channel of this satellite: IBC and ISY in turn in the first downlink slot
            if (!duplex_.empty()) {
                const int bc = duplex_[(s.id * 37) % duplex_.size()];
                const double tDl = T + 0.0558;
                if (F % 2 == 0) add(iridiumBuildIbc(s.id, beam, kIridiumSynthEpoch + tStartUnix_ + tDl), 238, iridium::kPreambleShort, tDl, iridium::channelHz(bc), s, amp);
                else add(iridiumBuildIsy(), 358, iridium::kPreambleShort, tDl, iridium::channelHz(bc), s, amp);
                // traffic: voice-like bursts in the other downlink slots
                if (voice_)
                    for (int slot = 1; slot < 4; slot++) {
                        if ((rng_() % 100) >= 40) continue;
                        const int ch = duplex_[rng_() % duplex_.size()];
                        add(iridiumBuildVoiceLike(rng_()), 358, iridium::kPreambleShort, tDl + slot * 0.00828, iridium::channelHz(ch), s, amp);
                    }
            }
        }
    }

    double rate_;
    SynthConfig cfg_;
    int nSats_ = 3;
    uint32_t seed_ = 0;
    bool voice_ = true, doppler_ = true, acars_ = true;
    std::vector<std::vector<uint8_t>> idaParts_;
    int idaCtr_ = 0, idaCh_ = 0, acarsIdx_ = 0, idaSat_ = -1;
    double centre_ = 1622e6, tStart_ = 0, sigma_ = 0.05, dt_ = 0;
    float amp_ = 0.1f;
    uint64_t produced_ = 0;
    int nextFrame_ = 0, msgIdx_ = 0;
    double tStartUnix_ = 0;          // modeVal[2]: the signal starts this far after the epoch
    std::vector<std::vector<uint8_t>> msgParts_;
    std::vector<Slot> busy_;
    std::vector<int> duplex_;
    std::vector<Shaped> pend_;
    std::mt19937 rng_;
    genutil::NoiseSource noise_{21};
};

} // namespace

std::vector<IridiumSkySat> iridiumSky(double t, double minElevDeg, double carrierHz) {
    std::vector<IridiumSkySat> v;
    const V3 o = observer();
    const V3 up = {o.x / kRe, o.y / kRe, o.z / kRe};
    const V3 east = {-std::sin(kObsLon * kDeg), std::cos(kObsLon * kDeg), 0};
    const V3 north = {-std::sin(kObsLat * kDeg) * std::cos(kObsLon * kDeg), -std::sin(kObsLat * kDeg) * std::sin(kObsLon * kDeg), std::cos(kObsLat * kDeg)};
    for (int p = 0; p < 6; p++)
        for (int s = 0; s < 11; s++) {
            const double el = elevation(p, s, t);
            if (el < minElevDeg) continue;
            IridiumSkySat q;
            q.id = satId(p, s);
            const V3 e = satEcef(p, s, t), r = sub(e, o);
            q.elevDeg = el;
            q.azDeg = std::fmod(std::atan2(dot(r, east), dot(r, north)) / kDeg + 360, 360);
            q.rangeKm = norm(r);
            q.lat = std::atan2(e.z, std::hypot(e.x, e.y)) / kDeg;
            q.lon = std::atan2(e.y, e.x) / kDeg;
            q.altKm = norm(e) - kRe;
            auto dop = [&](double tt) { return -carrierHz * (rangeKm(p, s, tt + 0.5) - rangeKm(p, s, tt - 0.5)) / kC; };
            q.dopplerHz = dop(t);
            q.dopplerRate = (dop(t + 1) - dop(t - 1)) / 2;
            (void)up;
            v.push_back(q);
        }
    std::sort(v.begin(), v.end(), [](const IridiumSkySat& a, const IridiumSkySat& b) { return a.elevDeg > b.elevDeg; });
    return v;
}

double iridiumSkyStart(uint32_t seed, int nSats) {
    const double period = 2 * M_PI * std::sqrt(std::pow(kRe + kAltKm, 3) / kMu);
    const double base = (seed % 64) * 97.0;
    double bestT = base;
    int bestN = -1;
    for (double t = base; t < base + period; t += 5) {
        int n = 1 << 30;
        for (double d = 0; d <= 60; d += 30) n = std::min(n, (int)iridiumSky(t + d, 3).size());
        if (n >= nSats) return t;
        if (n > bestN) { bestN = n; bestT = t; }
    }
    return bestT;
}

void iridiumRenderBursts(const std::vector<IridiumTestBurst>& bursts, double rate, double t0, cf32* out, size_t n, double sroPpm) {
    const double dt = 1.0 / (rate * (1 + sroPpm * 1e-6));
    for (const auto& b : bursts) renderShaped(shapeBurst(b), t0, dt, out, n);
}

struct IridiumBurstPlayer::Impl {
    double rate = 0, dt = 0;
    uint64_t pos = 0;
    std::vector<IridiumTestBurst> todo;      // sorted by start, latest first
    bool sorted = true;
    std::vector<Shaped> live;
};
IridiumBurstPlayer::IridiumBurstPlayer(double rate, double sroPpm) : p_(std::make_unique<Impl>()) {
    p_->rate = rate;
    p_->dt = 1.0 / (rate * (1 + sroPpm * 1e-6));
}
IridiumBurstPlayer::~IridiumBurstPlayer() = default;
void IridiumBurstPlayer::add(const IridiumTestBurst& b) { p_->todo.push_back(b); p_->sorted = false; }
double IridiumBurstPlayer::time() const { return p_->pos * p_->dt; }
void IridiumBurstPlayer::render(cf32* out, size_t n) {
    Impl& P = *p_;
    if (!P.sorted) {
        std::sort(P.todo.begin(), P.todo.end(), [](const IridiumTestBurst& a, const IridiumTestBurst& b) { return a.startSec > b.startSec; });
        P.sorted = true;
    }
    const double t0 = P.pos * P.dt, t1 = (P.pos + n) * P.dt;
    while (!P.todo.empty() && P.todo.back().startSec < t1 + 0.001) { P.live.push_back(shapeBurst(P.todo.back())); P.todo.pop_back(); }
    for (size_t i = 0; i < P.live.size();) {
        renderShaped(P.live[i], t0, P.dt, out, n);
        if (P.live[i].endSec() < t1) { P.live[i] = std::move(P.live.back()); P.live.pop_back(); }
        else i++;
    }
    P.pos += n;
}

std::unique_ptr<ModeSynth> makeIridiumSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<IridiumSynth>(cfg, sampleRate);
}

} // namespace dect2
