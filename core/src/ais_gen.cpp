// AIS test signal: GMSK bursts (ITU-R M.1371-5 annex 2: BT 0.4, 9600 bit/s, modulation index 0.5) and a simulated port area.
// The modulator is the textbook one: the data bits drive a Gaussian frequency pulse, the phase is its integral, one bit moves the phase by pi/2.
#include "dect2/ais_gen.h"
#include "dect2/ais_tel.h"
#include "dect2/gen_util.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>

namespace dect2 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBit = 9600.0;
constexpr double kSlotSec = 60.0 / 2250.0;
constexpr double kChanHz = 25000.0;
constexpr double kRefAmp = 0.3;
constexpr int kFine = 32;     // phase table entries per bit
constexpr int kLead = 3;      // bits of the phase table before the first and after the last data bit

// Step response of the Gaussian filter followed by a one bit wide rectangle (T = 1): q(v) rises from 0 to 1 over about three bits.
// Phi(x) = (1 + erf(a x)) / 2 is the Gaussian step, a = pi B sqrt(2 / ln 2) with B T = 0.4; psi is its integral.
const std::vector<double>& pulseTable() {
    static const std::vector<double> t = [] {
        const double a = kPi * 0.4 * std::sqrt(2.0 / std::log(2.0));
        auto psi = [a](double x) { return 0.5 * (x + x * std::erf(a * x) + std::exp(-a * a * x * x) / (a * std::sqrt(kPi))); };
        std::vector<double> q(6 * kFine + 1);
        for (int m = 0; m <= 6 * kFine; m++) {
            const double v = (double)(m - 3 * kFine) / kFine;
            q[m] = psi(v + 0.5) - psi(v - 0.5);
        }
        return q;
    }();
    return t;
}

// phase[j] at u = j / kFine - kLead bits, for the line bits (+1 / -1 frequency)
std::vector<float> phaseTable(const ais::Bits& line) {
    const auto& q = pulseTable();
    const int nb = (int)line.size();
    std::vector<double> pre(nb + 1, 0.0);
    for (int k = 0; k < nb; k++) pre[k + 1] = pre[k] + (line[k] ? 1.0 : -1.0);
    const int n = (nb + 2 * kLead) * kFine + 1;
    std::vector<float> ph(n);
    for (int j = 0; j < n; j++) {
        const double u = (double)j / kFine - kLead;
        const int kA = (int)std::floor(u - 3.5) + 1, kB = (int)std::ceil(u + 2.5) - 1;
        double s = pre[std::min(std::max(kA, 0), nb)];
        for (int k = std::max(kA, 0); k <= std::min(kB, nb - 1); k++) {
            const int m = j - k * kFine - kFine / 2;
            const double v = m < 0 ? 0.0 : m > 6 * kFine ? 1.0 : q[m];
            s += (line[k] ? 1.0 : -1.0) * v;
        }
        ph[j] = (float)(s * kPi / 2);
    }
    return ph;
}

struct Burst {
    double t0 = 0;            // seconds
    int ch = 0;               // 0 = AIS 1 (-25 kHz), 1 = AIS 2 (+25 kHz)
    double amp = 1, cfo = 0, theta0 = 0;
    int nbits = 0;
    std::vector<float> ph;
    double endSec(double bitRate) const { return t0 + (nbits + 1) / bitRate; }
};

Burst makeBurst(const ais::Bits& payload, int ch, double t0, double amp, double cfo, double theta0) {
    Burst b;
    const ais::Bits line = ais::burstLineBits(payload);
    b.t0 = t0; b.ch = ch; b.amp = amp; b.cfo = cfo; b.theta0 = theta0;
    b.nbits = (int)line.size();
    b.ph = phaseTable(line);
    return b;
}

// Adds the bursts that overlap [tA, tB) (sample n0 is at time n0 / rate) to out.
class Mixer {
public:
    Mixer(double rate, double sroPpm, double tuneOffsetHz, double cfoHz) : rate_(rate), bitRate_(kBit * (1.0 + sroPpm * 1e-6)), off_(tuneOffsetHz), cfo_(cfoHz) {}
    void add(Burst b) { bursts_.push_back(std::move(b)); }
    size_t pending() const { return bursts_.size(); }
    void render(cf32* out, size_t n, uint64_t n0) {
        const double tA = (double)n0 / rate_, tB = (double)(n0 + n) / rate_;
        for (const Burst& b : bursts_) {
            const double dur = (b.nbits + 1) / bitRate_;
            if (b.t0 >= tB || b.t0 + dur <= tA) continue;
            const double fc = (b.ch ? kChanHz : -kChanHz) - off_ + cfo_ + b.cfo;
            const double w = 2 * kPi * fc;
            size_t i0 = b.t0 > tA ? (size_t)std::ceil((b.t0 - tA) * rate_) : 0;
            for (size_t i = i0; i < n; i++) {
                const double t = (double)(n0 + i) / rate_ - b.t0;
                const double u = t * bitRate_;
                if (u < 0) continue;
                if (u > b.nbits) break;
                const double f = (u + kLead) * kFine;
                const size_t k = (size_t)f;
                if (k + 1 >= b.ph.size()) break;
                const double fr = f - (double)k;
                const double phi = b.ph[k] * (1 - fr) + b.ph[k + 1] * fr;
                // power ramps over four bits at both ends
                double env = 1.0;
                if (u < 4) env = 0.5 * (1 - std::cos(kPi * u / 4));
                else if (u > b.nbits - 4) env = 0.5 * (1 - std::cos(kPi * (b.nbits - u) / 4));
                const double ang = w * t + b.theta0 + phi;
                out[i] += cf32((float)(b.amp * env * std::cos(ang)), (float)(b.amp * env * std::sin(ang)));
            }
        }
        // forget what has ended
        const double now = tB;
        bursts_.erase(std::remove_if(bursts_.begin(), bursts_.end(), [&](const Burst& b) { return b.t0 + (b.nbits + 1) / bitRate_ < now - 0.01; }), bursts_.end());
    }
private:
    double rate_, bitRate_, off_, cfo_;
    std::vector<Burst> bursts_;
};

// noise sigma per real component for 'snrDb' in 48 kHz, and the common scale that keeps the peaks below 0.9
struct Levels { float sigma = 0, scale = 1; };
Levels levelsFor(double rate, double snrDb) {
    Levels l;
    if (snrDb < -100) return l;
    const double pn = kRefAmp * kRefAmp * (rate / 48000.0) / std::pow(10.0, snrDb / 10.0);
    const double sig = std::sqrt(pn / 2);
    l.sigma = (float)sig;
    if (sig > 0.15) l.scale = (float)(0.15 / sig);
    return l;
}

// ------------------------------------------------------------------------------------------------ the fleet

struct Station {
    int kind = 0;                // 0 class A, 1 class B, 2 base station, 3 aid to navigation, 4 SAR aircraft
    uint32_t mmsi = 0;
    std::string name, call, dest;
    int shipType = 0, dimA = 0, dimB = 0, dimC = 0, dimD = 0;
    double draught = 0;
    int etaMo = 0, etaDay = 0, etaH = 24, etaMin = 60;
    double lat0 = 25, lon0 = 55, sog = 0, bearing = 0;
    int path = 0;                // 0 fixed, 1 back and forth on a line, 2 circle
    double len = 0, radius = 0, dir = 1, s0 = 0;
    int nav = 0;
    bool b19 = false;            // class B that sends message 19 instead of 18
    int aidType = 0; bool virt = false;
    double level = 1, cfoErr = 0, amp0 = 0;
    // schedule
    double nextPos = 0, nextStat = 0, nextOther = 0;
    int toggle = 0, seq = 0;
    double altitude = 0;
};

struct State { double lat, lon, sog, cog, hdg, rot; };

uint32_t imoWithCheck(uint32_t six) {   // 6 digits and the check digit of the IMO number
    uint32_t s = 0, v = six;
    for (int w = 2; w <= 7; w++) { s += (v % 10) * w; v /= 10; }
    return six * 10 + s % 10;
}

State stateAt(const Station& s, double T) {
    State st{s.lat0, s.lon0, s.sog, s.bearing, s.bearing, 0};
    const double mps = s.sog * 0.514444;
    double x = 0, y = 0;
    if (s.path == 1 && s.len > 0) {
        const double d = std::fmod(s.s0 + mps * T, 2 * s.len);
        const bool back = d > s.len;
        const double along = back ? 2 * s.len - d : d;
        const double b = (s.bearing + (back ? 180.0 : 0.0)) * kPi / 180;
        x = along * std::sin(b); y = along * std::cos(b);
        st.cog = st.hdg = std::fmod(s.bearing + (back ? 180.0 : 0.0), 360.0);
    } else if (s.path == 2 && s.radius > 0) {
        const double w = s.dir * mps / s.radius;                       // rad/s
        const double a = s.bearing * kPi / 180 + w * T;
        x = s.radius * std::sin(a); y = s.radius * std::cos(a);
        st.cog = st.hdg = std::fmod(std::fmod(a * 180 / kPi + (s.dir > 0 ? 90.0 : -90.0), 360.0) + 360.0, 360.0);
        st.rot = w * 180 / kPi * 60;                                   // deg/min
    }
    st.lat = s.lat0 + y / 111320.0;
    st.lon = s.lon0 + x / (111320.0 * std::cos(s.lat0 * kPi / 180));
    return st;
}

double distKm(const Station& s, double lat, double lon) {
    const double dy = (s.lat0 - lat) * 111.32, dx = (s.lon0 - lon) * 111.32 * std::cos(lat * kPi / 180);
    return std::sqrt(dx * dx + dy * dy);
}

int rotRaw(double degMin) {
    const double a = std::sqrt(std::fabs(degMin)) * 4.733;
    const int r = (int)std::lround(std::min(a, 126.0));
    return degMin < 0 ? -r : r;
}

uint32_t radioStatus(std::mt19937& rng) {   // SOTDMA: sync state 0, slot time-out 0 - 7, a random sub-message (what the generator can say about it)
    return (uint32_t)(rng() % 8) << 14 | (uint32_t)(rng() % 16384);
}

ais::Bits encodePosition(int type, const Station& s, const State& st, double T, std::mt19937& rng) {
    using namespace ais;
    Bits b;
    putU(b, 0, 6, (uint32_t)type);
    putU(b, 8, 30, s.mmsi);
    const int32_t lon = (int32_t)std::lround(st.lon * 600000), lat = (int32_t)std::lround(st.lat * 600000);
    const uint32_t sec = (uint32_t)((int64_t)T % 60);
    if (type <= 3) {
        putU(b, 38, 4, (uint32_t)s.nav);
        putS(b, 42, 8, s.nav == 5 || s.nav == 1 ? -128 : rotRaw(st.rot));
        putU(b, 50, 10, (uint32_t)std::lround(st.sog * 10));
        putU(b, 60, 1, 1);
        putS(b, 61, 28, lon); putS(b, 89, 27, lat);
        putU(b, 116, 12, (uint32_t)std::lround(st.cog * 10) % 3600);
        putU(b, 128, 9, (uint32_t)std::lround(st.hdg) % 360);
        putU(b, 137, 6, sec);
        putU(b, 149, 19, radioStatus(rng));
        b.resize(168, 0);
    } else if (type == 18 || type == 19) {
        putU(b, 46, 10, (uint32_t)std::lround(st.sog * 10));
        putU(b, 56, 1, 1);
        putS(b, 57, 28, lon); putS(b, 85, 27, lat);
        putU(b, 112, 12, (uint32_t)std::lround(st.cog * 10) % 3600);
        putU(b, 124, 9, (uint32_t)std::lround(st.hdg) % 360);
        putU(b, 133, 6, sec);
        if (type == 18) {
            putU(b, 141, 1, 1);          // carrier sense unit
            putU(b, 143, 1, 1);          // DSC
            putU(b, 144, 1, 1);          // whole marine band
            putU(b, 145, 1, 1);          // accepts message 22
            putU(b, 148, 1, 1);          // CS: communication state selector
            putU(b, 149, 19, radioStatus(rng));
            b.resize(168, 0);
        } else {
            putText(b, 143, 120, s.name);
            putU(b, 263, 8, (uint32_t)s.shipType);
            putU(b, 271, 9, (uint32_t)s.dimA); putU(b, 280, 9, (uint32_t)s.dimB); putU(b, 289, 6, (uint32_t)s.dimC); putU(b, 295, 6, (uint32_t)s.dimD);
            putU(b, 301, 4, 1);
            b.resize(312, 0);
        }
    } else if (type == 9) {
        putU(b, 38, 12, (uint32_t)s.altitude);
        putU(b, 50, 10, (uint32_t)std::lround(st.sog));
        putU(b, 60, 1, 1);
        putS(b, 61, 28, lon); putS(b, 89, 27, lat);
        putU(b, 116, 12, (uint32_t)std::lround(st.cog * 10) % 3600);
        putU(b, 128, 6, sec);
        putU(b, 148, 20, radioStatus(rng));
        b.resize(168, 0);
    }
    return b;
}

ais::Bits encodeStatic5(const Station& s, uint32_t imo) {
    using namespace ais;
    Bits b;
    putU(b, 0, 6, 5); putU(b, 8, 30, s.mmsi);
    putU(b, 40, 30, imo);
    putText(b, 70, 42, s.call);
    putText(b, 112, 120, s.name);
    putU(b, 232, 8, (uint32_t)s.shipType);
    putU(b, 240, 9, (uint32_t)s.dimA); putU(b, 249, 9, (uint32_t)s.dimB); putU(b, 258, 6, (uint32_t)s.dimC); putU(b, 264, 6, (uint32_t)s.dimD);
    putU(b, 270, 4, 1);
    putU(b, 274, 4, (uint32_t)s.etaMo); putU(b, 278, 5, (uint32_t)s.etaDay); putU(b, 283, 5, (uint32_t)s.etaH); putU(b, 288, 6, (uint32_t)s.etaMin);
    putU(b, 294, 8, (uint32_t)std::lround(s.draught * 10));
    putText(b, 302, 120, s.dest);
    b.resize(424, 0);
    return b;
}

ais::Bits encode24(const Station& s, int part) {
    using namespace ais;
    Bits b;
    putU(b, 0, 6, 24); putU(b, 8, 30, s.mmsi);
    putU(b, 38, 2, (uint32_t)part);
    if (part == 0) {
        putText(b, 40, 120, s.name);
        b.resize(168, 0);
    } else {
        putU(b, 40, 8, (uint32_t)s.shipType);
        putText(b, 48, 18, "SRT");
        putU(b, 66, 4, 3); putU(b, 70, 20, 100000 + (s.mmsi % 800000));
        putText(b, 90, 42, s.call);
        putU(b, 132, 9, (uint32_t)s.dimA); putU(b, 141, 9, (uint32_t)s.dimB); putU(b, 150, 6, (uint32_t)s.dimC); putU(b, 156, 6, (uint32_t)s.dimD);
        b.resize(168, 0);
    }
    return b;
}

ais::Bits encodeBase4(const Station& s, double T) {
    using namespace ais;
    Bits b;
    putU(b, 0, 6, 4); putU(b, 8, 30, s.mmsi);
    const int64_t secs = 1767225600 + (int64_t)T;       // 2026-01-01 00:00:00 UTC plus the signal time
    const int64_t days = secs / 86400, r = secs % 86400;
    // civil date from the day count (proleptic Gregorian)
    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
    const int y = (int)(yoe + era * 400 + (m <= 2 ? 1 : 0));
    putU(b, 38, 14, (uint32_t)y); putU(b, 52, 4, (uint32_t)m); putU(b, 56, 5, (uint32_t)d);
    putU(b, 61, 5, (uint32_t)(r / 3600)); putU(b, 66, 6, (uint32_t)(r / 60 % 60)); putU(b, 72, 6, (uint32_t)(r % 60));
    putU(b, 78, 1, 1);
    putS(b, 79, 28, (int32_t)std::lround(s.lon0 * 600000)); putS(b, 107, 27, (int32_t)std::lround(s.lat0 * 600000));
    putU(b, 134, 4, 1);
    b.resize(168, 0);
    return b;
}

ais::Bits encodeAton21(const Station& s, double T) {
    using namespace ais;
    Bits b;
    putU(b, 0, 6, 21); putU(b, 8, 30, s.mmsi);
    putU(b, 38, 5, (uint32_t)s.aidType);
    putText(b, 43, 120, s.name);
    putU(b, 163, 1, 1);
    putS(b, 164, 28, (int32_t)std::lround(s.lon0 * 600000)); putS(b, 192, 27, (int32_t)std::lround(s.lat0 * 600000));
    putU(b, 219, 9, (uint32_t)s.dimA); putU(b, 228, 9, (uint32_t)s.dimB); putU(b, 237, 6, (uint32_t)s.dimC); putU(b, 243, 6, (uint32_t)s.dimD);
    putU(b, 249, 4, s.virt ? 0 : 1);
    putU(b, 253, 6, s.virt ? 60 : (uint32_t)((int64_t)T % 60));
    putU(b, 269, 1, s.virt ? 1 : 0);
    b.resize(272, 0);
    return b;
}

ais::Bits encodeText14(const Station& s, const std::string& txt) {
    using namespace ais;
    Bits b;
    putU(b, 0, 6, 14); putU(b, 8, 30, s.mmsi);
    putText(b, 40, txt.size() * 6, txt);
    return b;
}

ais::Bits encodeBinary8(const Station& s, int dac, int fi, int nbytes, std::mt19937& rng) {
    using namespace ais;
    Bits b;
    putU(b, 0, 6, 8); putU(b, 8, 30, s.mmsi);
    putU(b, 40, 10, (uint32_t)dac); putU(b, 50, 6, (uint32_t)fi);
    for (int i = 0; i < nbytes; i++) putU(b, 56 + 8 * i, 8, (uint32_t)(rng() & 255));
    return b;
}

class Fleet {
public:
    Fleet(const SynthConfig& cfg, double rate)
        : rate_(rate), mixer_(rate, cfg.sroPpm, kTuneOffset, cfg.cfoHz), rng_(cfg.modeOpt[1] > 0 ? (uint32_t)cfg.modeOpt[1] : 1u),
          noise_(cfg.modeOpt[1] > 0 ? (uint32_t)cfg.modeOpt[1] : 1u), speed_(cfg.modeVal[0] > 0 ? cfg.modeVal[0] : 1.0) {
        levels_ = levelsFor(rate, cfg.snrDb);
        int nv = cfg.modeOpt[0] > 0 ? std::min(cfg.modeOpt[0], 40) : 15;
        buildVessels(nv);
        if (!(cfg.modeOpt[3] & 1)) buildExtras();
        for (Station& s : st_) {
            s.cfoErr = ((double)(rng_() % 2001) - 1000.0) / 1000.0 * 200.0;
            if (cfg.modeOpt[2] == 0) {
                s.level = std::min(1.0, std::max(0.1, 2.0 / std::max(distKm(s, kRxLat, kRxLon), 0.01)));
            }
            const double ivl = posInterval(s, stateAt(s, 0));
            s.nextPos = u01() * ivl;
            s.nextStat = u01() * 30.0;
            s.nextOther = u01() * 30.0;
            s.toggle = (int)(rng_() & 1);
        }
    }

    void generate(cf32* out, size_t n) {
        for (size_t i = 0; i < n; i++) out[i] = cf32(0.f, 0.f);
        const double tA = (double)pos_ / rate_, tB = (double)(pos_ + n) / rate_;
        plan(tA, tB + 0.6);
        mixer_.render(out, n, pos_);
        if (levels_.sigma > 0) noise_.add(out, n, levels_.sigma);
        if (levels_.scale != 1.f) for (size_t i = 0; i < n; i++) out[i] *= levels_.scale;
        for (size_t i = 0; i < n; i++) {
            float re = out[i].real(), im = out[i].imag();
            re = std::max(-0.89f, std::min(0.89f, re)); im = std::max(-0.89f, std::min(0.89f, im));
            out[i] = cf32(re, im);
        }
        pos_ += n;
    }

    bool logOn = false;
    std::vector<AisSentBurst> sentLog;

private:
    static constexpr double kTuneOffset = 0;      // aisTuning().tuneOffsetHz
    static constexpr double kRxLat = 25.02, kRxLon = 55.00;

    double u01() { return (double)rng_() / 4294967296.0; }

    double posInterval(const Station& s, const State& st) const {
        if (s.kind == 0) {
            if (s.nav == 1 || s.nav == 5 || st.sog < 0.3) return 30.0;      // 3 minutes in the standard, 30 s here so the screen fills
            if (st.sog > 23) return 2.0;
            if (st.sog > 14) return 6.0;
            return 10.0;
        }
        if (s.kind == 1) return st.sog > 2 ? 30.0 : 60.0;
        if (s.kind == 2) return 10.0;
        if (s.kind == 3) return 30.0;
        return 10.0;
    }

    void buildVessels(int nv) {
        struct V { int kind; uint32_t mmsi; const char *name, *call, *dest; int type, A, B, C, D; double dr; int mo, d, h, mi; double lat, lon, sog, brg; int path; double len, rad; int nav; bool b19; };
        static const V v[15] = {
            {0, 636092117, "GULF PIONEER", "D5AB3", "FUJAIRAH ANCH", 80, 300, 30, 30, 30, 20.5, 1, 2, 18, 0, 25.22, 54.78, 11.5, 120, 1, 28000, 0, 0, false},
            {0, 470611000, "AL KHALEEJ STAR", "A6DE2", "JEBEL ALI", 71, 180, 20, 15, 15, 11.2, 1, 1, 21, 30, 25.15, 55.25, 14.8, 215, 1, 25000, 0, 0, false},
            {0, 538006710, "EASTERN HARMONY", "V7KZ4", "SINGAPORE", 70, 150, 15, 14, 14, 9.8, 1, 3, 6, 0, 25.005, 55.045, 0, 310, 0, 0, 0, 5, false},
            {0, 257421000, "NORDIC SPIRIT", "LAHB8", "FUJAIRAH ANCH", 82, 170, 14, 16, 16, 12.4, 1, 2, 9, 0, 25.12, 54.90, 0, 75, 0, 0, 0, 1, false},
            {0, 477180600, "SEA BREEZE", "VRPD7", "MUMBAI", 79, 140, 10, 12, 12, 8.6, 1, 4, 12, 15, 25.28, 55.10, 9.2, 80, 1, 22000, 0, 0, false},
            {0, 470103000, "DUBAI TUG 1", "A6TG1", "JEBEL ALI", 52, 18, 8, 5, 5, 3.4, 0, 0, 24, 60, 25.00, 55.065, 6.0, 0, 2, 0, 700, 0, false},
            {0, 470103500, "DUBAI TUG 2", "A6TG2", "JEBEL ALI", 52, 20, 7, 5, 5, 3.6, 0, 0, 24, 60, 25.01, 55.03, 4.5, 20, 1, 2500, 0, 0, false},
            {0, 470201700, "JEBEL PILOT", "A6PL4", "JEBEL ALI", 50, 12, 6, 3, 3, 1.8, 0, 0, 24, 60, 25.06, 54.95, 18.2, 150, 1, 9000, 0, 0, false},
            {1, 470388020, "AL NOOR", "A6DH7", "", 79, 18, 6, 4, 4, 0, 0, 0, 24, 60, 24.94, 55.12, 6.2, 310, 1, 6000, 0, 0, false},
            {1, 470501234, "BLUE MARLIN", "A6YX3", "", 37, 10, 4, 2, 2, 0, 0, 0, 24, 60, 25.08, 55.15, 12.0, 250, 1, 8000, 0, 0, false},
            {1, 235098765, "SEA GYPSY", "2ABC9", "", 37, 9, 3, 2, 2, 0, 0, 0, 24, 60, 25.04, 55.20, 5.0, 190, 1, 4000, 0, 0, true},
            {0, 248935000, "OCEAN VOYAGER", "9HAB2", "FUJAIRAH", 60, 220, 20, 18, 18, 7.3, 1, 2, 22, 45, 25.30, 55.30, 16.5, 40, 1, 30000, 0, 0, false},
            {0, 470655100, "ADNOC SUPPLY 7", "A6SP7", "ZAKUM FIELD", 90, 70, 10, 10, 10, 5.2, 1, 2, 3, 20, 25.10, 54.85, 10.0, 305, 1, 15000, 0, 0, false},
            {0, 563210400, "PACIFIC HARMONY", "9V4421", "COLOMBO", 70, 190, 20, 16, 16, 10.1, 1, 6, 8, 0, 25.18, 54.70, 12.4, 100, 1, 26000, 0, 0, false},
            {0, 470299000, "AL WAFRA", "A6DR5", "JEBEL ALI", 33, 60, 15, 10, 10, 4.0, 1, 2, 24, 60, 25.03, 54.99, 0, 45, 0, 0, 0, 3, false},
        };
        static const char* const pre[8] = {"GULF", "AL", "EASTERN", "OCEAN", "DUBAI", "ARABIAN", "RED SEA", "BLUE"};
        static const char* const suf[8] = {"TRADER", "WAVE", "PRIDE", "CARRIER", "SPIRIT", "FORTUNE", "VOYAGER", "STAR"};
        for (int i = 0; i < nv; i++) {
            Station s;
            if (i < 15) {
                const V& a = v[i];
                s.kind = a.kind; s.mmsi = a.mmsi; s.name = a.name; s.call = a.call; s.dest = a.dest; s.shipType = a.type;
                s.dimA = a.A; s.dimB = a.B; s.dimC = a.C; s.dimD = a.D; s.draught = a.dr; s.etaMo = a.mo; s.etaDay = a.d; s.etaH = a.h; s.etaMin = a.mi;
                s.lat0 = a.lat; s.lon0 = a.lon; s.sog = a.sog; s.bearing = a.brg; s.path = a.path; s.len = a.len; s.radius = a.rad; s.nav = a.nav; s.b19 = a.b19;
            } else {
                const V& a = v[i % 15];
                s = Station();
                s.kind = a.kind;
                s.mmsi = (uint32_t)(a.mmsi / 1000 * 1000 + 500 + i);
                s.name = std::string(pre[i % 8]) + " " + suf[(i / 8 + i) % 8];
                if (s.name.size() > 20) s.name.resize(20);
                s.call = std::string("A6") + (char)('A' + i % 26) + (char)('A' + (i * 7) % 26) + (char)('0' + i % 10);
                s.dest = a.dest; s.shipType = a.type; s.dimA = a.A; s.dimB = a.B; s.dimC = a.C; s.dimD = a.D; s.draught = a.dr;
                s.etaMo = a.mo; s.etaDay = a.d; s.etaH = a.h; s.etaMin = a.mi;
                s.lat0 = 24.9 + 0.35 * u01(); s.lon0 = 54.7 + 0.6 * u01(); s.sog = a.sog > 0 ? a.sog * (0.7 + 0.6 * u01()) : 0; s.bearing = 360.0 * u01();
                s.path = a.path; s.len = a.len; s.radius = a.rad; s.nav = a.nav; s.b19 = false;
            }
            s.s0 = s.len * u01();
            s.dir = (rng_() & 1) ? 1 : -1;
            st_.push_back(s);
        }
    }

    void buildExtras() {
        Station b; b.kind = 2; b.mmsi = 4700001; b.name = "JEBEL ALI VTS"; b.lat0 = 25.0; b.lon0 = 55.07;
        st_.push_back(b);
        Station a1; a1.kind = 3; a1.mmsi = 994700001; a1.name = "JA CH BUOY 1"; a1.lat0 = 25.045; a1.lon0 = 55.0; a1.aidType = 25; a1.dimA = a1.dimB = a1.dimC = a1.dimD = 1;
        st_.push_back(a1);
        Station a2; a2.kind = 3; a2.mmsi = 994700002; a2.name = "JA VIRTUAL SAFE WATER"; a2.lat0 = 25.07; a2.lon0 = 54.92; a2.aidType = 29; a2.virt = true;
        a2.name = "JA VIRTUAL SW";
        st_.push_back(a2);
        Station sar; sar.kind = 4; sar.mmsi = 111470201; sar.name = "SAR HELI"; sar.lat0 = 25.10; sar.lon0 = 55.15; sar.sog = 70; sar.path = 2; sar.radius = 3000;
        sar.bearing = 0; sar.dir = 1; sar.altitude = 450;
        st_.push_back(sar);
    }

    // slots are numbered from the start of the signal: slot k covers [k, k + 1) * kSlotSec
    int64_t takeSlots(int ch, double due, double tNow, int nslots) {
        int64_t k = (int64_t)std::ceil(std::max(due, tNow) / kSlotSec) + (int64_t)(rng_() % 24);
        for (;; k++) {
            bool free = true;
            for (int j = 0; j < nslots && free; j++) free = !busy_[ch].count(k + j);
            if (free) break;
        }
        for (int j = 0; j < nslots; j++) busy_[ch].insert(k + j);
        while (!busy_[ch].empty() && *busy_[ch].begin() < (int64_t)(tNow / kSlotSec) - 4) busy_[ch].erase(busy_[ch].begin());
        return k;
    }

    // sends 'payload' on channel ch at about 'due'; returns the time it starts
    double send(Station& s, const ais::Bits& payload, int ch, double due, double tNow) {
        const int bits = (int)ais::burstLineBits(payload).size();
        const int nslots = std::max(1, (int)std::ceil((bits + 8) / 256.0));
        const int64_t k = takeSlots(ch, due, tNow, nslots);
        const double t0 = (double)k * kSlotSec;
        mixer_.add(makeBurst(payload, ch, t0, kRefAmp * s.level, s.cfoErr, 2 * kPi * u01()));
        if (logOn) sentLog.push_back({t0, ch ? 'B' : 'A', payload});
        return t0;
    }

    void plan(double tNow, double horizon) {
        for (Station& s : st_) {
            // positions
            while (s.nextPos < horizon) {
                const double T = std::max(s.nextPos, 0.0) * speed_;
                const State stt = stateAt(s, T);
                const int ch = s.toggle++ & 1;
                const double due = s.nextPos;
                if (s.kind == 0) {
                    send(s, encodePosition(s.nav == 1 || s.nav == 5 ? 3 : 1, s, stt, T, rng_), ch, due, tNow);
                } else if (s.kind == 1) {
                    send(s, encodePosition(s.b19 ? 19 : 18, s, stt, T, rng_), ch, due, tNow);
                } else if (s.kind == 2) {
                    send(s, encodeBase4(s, s.nextPos), ch, due, tNow);
                } else if (s.kind == 3) {
                    send(s, encodeAton21(s, s.nextPos), ch, due, tNow);
                } else {
                    send(s, encodePosition(9, s, stt, T, rng_), ch, due, tNow);
                }
                s.nextPos += posInterval(s, stt) * (0.9 + 0.2 * u01());
            }
            // static and voyage data, other messages
            while (s.nextStat < horizon) {
                const int ch = s.toggle++ & 1;
                const double due = s.nextStat;
                if (s.kind == 0) {
                    const uint32_t six = (s.mmsi % 900000u) + 100000u;
                    send(s, encodeStatic5(s, imoWithCheck(six)), ch, due, tNow);
                } else if (s.kind == 1) {
                    send(s, encode24(s, 0), ch, due, tNow);
                    send(s, encode24(s, 1), ch, due + 1.0, tNow);
                }
                s.nextStat += 30.0 * (0.9 + 0.2 * u01());
            }
            while (s.kind == 2 && s.nextOther < horizon) {
                const int ch = s.toggle++ & 1;
                if (s.seq++ & 1) send(s, encodeText14(s, "OnAir synthetic test signal"), ch, s.nextOther, tNow);
                else send(s, encodeBinary8(s, 470, 1, 12, rng_), ch, s.nextOther, tNow);
                s.nextOther += 30.0;
            }
            if (s.kind != 2) s.nextOther = 1e18;
        }
    }

    double rate_;
    Mixer mixer_;
    std::mt19937 rng_;
    genutil::NoiseSource noise_;
    double speed_;
    Levels levels_;
    std::vector<Station> st_;
    std::set<int64_t> busy_[2];
    uint64_t pos_ = 0;
};

class AisSynth : public ModeSynth {
public:
    AisSynth(const SynthConfig& cfg, double rate) : rate_(rate), fleet_(cfg, rate) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { fleet_.generate(out, n); }
    Fleet& fleet() { return fleet_; }
    const Fleet& fleet() const { return fleet_; }
private:
    double rate_;
    Fleet fleet_;
};

} // namespace

std::unique_ptr<ModeSynth> makeAisSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 200e3 || sampleRate > 21e6) return nullptr;
    return std::make_unique<AisSynth>(cfg, sampleRate);
}

void aisSynthLog(ModeSynth& s, bool on) {
    if (auto* a = dynamic_cast<AisSynth*>(&s)) a->fleet().logOn = on;
}
std::vector<AisSentBurst> aisSynthSent(const ModeSynth& s) {
    if (auto* a = dynamic_cast<const AisSynth*>(&s)) return a->fleet().sentLog;
    return {};
}

std::vector<cf32> renderAisBursts(const std::vector<AisBurstSpec>& specs, const AisRenderConfig& c) {
    const size_t n = (size_t)(c.durationSec * c.rate);
    std::vector<cf32> out(n, cf32(0, 0));
    Mixer mx(c.rate, c.sroPpm, c.tuneOffsetHz, c.cfoHz);
    for (const auto& s : specs) mx.add(makeBurst(s.payload, s.channel == 'B' ? 1 : 0, s.startSec, kRefAmp * s.level, s.cfoHz, s.phase));
    const size_t chunk = 65536;
    for (size_t i = 0; i < n; i += chunk) mx.render(out.data() + i, std::min(chunk, n - i), i);
    const Levels l = levelsFor(c.rate, c.snrDb);
    genutil::NoiseSource ns(c.seed);
    if (l.sigma > 0) ns.add(out.data(), n, l.sigma);
    for (auto& v : out) {
        v *= l.scale;
        v += cf32((float)c.dcOffset, (float)c.dcOffset);
        if (c.quantBits == 8) v = cf32(std::round(v.real() * 127.f) / 127.f, std::round(v.imag() * 127.f) / 127.f);
    }
    return out;
}

} // namespace dect2
