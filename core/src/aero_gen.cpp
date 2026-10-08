#include "dect2/aero_gen.h"
#include "dect2/aero_adsc.h"
#include "dect2/aero_demod.h"
#include "dect2/aero_phy.h"
#include "dect2/aero_rx.h"
#include "dect2/gen_util.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <cstdio>
#include <random>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;

// six aircraft: ICAO addresses in their countries' blocks, registrations and flights of airlines that cross the Gulf and the Indian Ocean
struct Plane { uint32_t aes; const char* reg; const char* flight; };
const Plane kPlanes[6] = {
    {0x896411, "A6-EUA", "EK0501"}, {0x8963A2, "A6-ETA", "EY0234"}, {0x06A1C3, "A7-BAB", "QR0960"},
    {0x800B21, "VT-ANA", "AI0995"}, {0x76CC45, "9V-SMF", "SQ0495"}, {0x70C072, "A4O-SA", "WY0601"},
};
struct Script { int plane; const char* label; const char* text; };
const Script kScript[] = {
    {0, "H1", "WX OMDB 071300Z 32012KT CAVOK 35/17 Q1008 NOSIG"},
    {2, "5Z", "/DOH OPS GATE C12 STAND 214 ETA 1415Z PAX CONX 38"},
    {4, "SA", "0EV142652V"},
    {3, "_d", ""},
    {5, "H1", "ATIS OOMS INFO D 1350Z RWY 08 ILS EXP 04012KT 9999 FEW030 33/21 Q1006"},
    {1, "H1", "WX VOMM 071330Z 09008KT 6000 HZ SCT020 31/26 Q1004 VABB 071330Z 27010KT 3000 HZ NSC 30/24 Q1005"},
    {0, "5Z", "/EK OPS PLS ADVISE FUEL REMAINING ON ARR DXB"},
    {4, "H1", "FREE TEXT CREW REST AREA RESET DONE NO FURTHER ACTION MX CONTROL SIN"},
    {2, "SA", "0LS1405K2"},
    {3, "H1", "ROUTE AMDT AFTER RASKI DCT PRA L301 THEN AS FILED TO VOMM"},
    {1, "5Z", "/AUH OPS NEW STAND 63 ARR BELT 4"},
    {5, "_d", ""},
};
constexpr int kGes = 5;

// Where the six aircraft fly, for their position reports (ADS-C from five, a text POS report from VT-ANA). They move 20 times faster
// than real so a track shows within a minute of test signal; the report times run at the same pace.
struct Route { double lat, lon, trackDeg, kt; int altFt; const char* ground; };
const Route kRoutes[6] = {
    {25.60, 56.80, 105, 480, 37000, "BOMCAYA"}, {24.10, 60.20, 95, 470, 39000, "BOMCAYA"}, {23.20, 64.10, 125, 490, 35000, "MCTCAYA"},
    {20.50, 66.20, 300, 460, 33000, ""}, {14.80, 71.90, 290, 485, 38000, "MCTCAYA"}, {22.30, 59.60, 140, 450, 36000, "BOMCAYA"},
};
constexpr double kTimeScale = 20;
constexpr double kStartSec = 12 * 3600 + 50 * 60;          // the simulated reports start at 12:50:00 UTC

void routeAt(int i, double t, double& lat, double& lon) {
    const Route& r = kRoutes[i];
    const double nm = r.kt * t * kTimeScale / 3600, a = r.trackDeg * kPi / 180;
    lat = r.lat + nm * std::cos(a) / 60;
    lon = r.lon + nm * std::sin(a) / (60 * std::cos(lat * kPi / 180));
}

// the downlink a simulated aircraft sends at stream time t (report number n)
AeroAcars positionReport(int i, double t, int n) {
    const Plane& p = kPlanes[i];
    const Route& r = kRoutes[i];
    double lat, lon;
    routeAt(i, t, lat, lon);
    AeroAcars m;
    m.aesId = p.aes; m.gesId = kGes; m.uplink = false;
    m.mode = "2"; m.registration = p.reg; m.flight = p.flight;
    char mn[8];
    std::snprintf(mn, sizeof mn, "M%02d%c", n % 100, 'A' + n % 26);
    m.msgNo = mn;
    m.blockId = std::string(1, char('1' + n % 9));
    const double sim = kStartSec + t * kTimeScale;
    if (!r.ground[0]) {
        // ARINC 702 style text report: position as degrees and minutes with tenths, waypoint, time, flight level, next waypoints
        const double la = std::fabs(lat), lo = std::fabs(lon);
        const int laD = (int)la, loD = (int)lo;
        const int laM = std::min(599, (int)std::lround((la - laD) * 600)), loM = std::min(599, (int)std::lround((lo - loD) * 600));
        const int s = (int)std::fmod(sim, 86400.0);
        const int s2 = (int)std::fmod(sim + 900, 86400.0);
        char b[160];
        std::snprintf(b, sizeof b, "#M1BPOS%c%02d%03d%c%03d%03d,PARAR,%02d%02d%02d,%03d,TOTOX,%02d%02d%02d,ORMAR,M47,28030,450", lat < 0 ? 'S' : 'N', laD, laM,
                      lon < 0 ? 'W' : 'E', loD, loM, s / 3600, s / 60 % 60, s % 60, r.altFt / 100, s2 / 3600, s2 / 60 % 60, s2 % 60);
        m.label = "H1";
        m.text = b;
        return m;
    }
    std::vector<uint8_t> g = aeroAdscBasicGroup(lat, lon, r.altFt, std::fmod(sim, 3600.0), 6);
    const auto fid = aeroAdscFlightIdGroup(p.flight);
    g.insert(g.end(), fid.begin(), fid.end());
    const auto er = aeroAdscEarthRefGroup(r.trackDeg, r.kt, 0);
    g.insert(g.end(), er.begin(), er.end());
    if (n % 2 == 0) {
        AeroAdscPoint a, b;
        routeAt(i, t + 900 / kTimeScale, a.lat, a.lon);
        routeAt(i, t + 1800 / kTimeScale, b.lat, b.lon);
        a.altFt = b.altFt = r.altFt;
        a.etaSec = 900;
        const auto pr = aeroAdscPredictedRouteGroup(a, b);
        g.insert(g.end(), pr.begin(), pr.end());
        const auto mt = aeroAdscMeteoGroup(35 + i * 4, 250 + i * 10, -52 + i);
        g.insert(g.end(), mt.begin(), mt.end());
    }
    const std::string app = aeroBuildAdscText(r.ground, p.reg, g);
    // SQ sends it as H1 with the sublabel and MFI in front, the others as label B6
    if (i == 4) { m.label = "H1"; m.text = "#M1B/B6 " + app.substr(1); }
    else { m.label = "B6"; m.text = app; }
    return m;
}

// interpolation from the modulator rate to the output rate: windowed sinc, 8 taps, 1024 phases
struct Interp {
    static constexpr int kTaps = 8, kPhases = 1024;
    std::vector<float> tab;
    Interp() : tab((size_t)kTaps * kPhases) {
        const double fc = 0.35;
        for (int p = 0; p < kPhases; p++) {
            const double frac = (double)p / kPhases;
            double sum = 0;
            for (int k = 0; k < kTaps; k++) {
                const double x = k - 3 - frac;
                const double s = std::fabs(x) < 1e-12 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
                const double w = std::fabs(x) >= 4 ? 0 : 0.42 + 0.5 * std::cos(kPi * x / 4) + 0.08 * std::cos(2 * kPi * x / 4);
                tab[(size_t)p * kTaps + k] = (float)(s * w);
                sum += s * w;
            }
            for (int k = 0; k < kTaps; k++) tab[(size_t)p * kTaps + k] = (float)(tab[(size_t)p * kTaps + k] / sum);
        }
    }
};
const Interp& interp() { static const Interp i; return i; }
} // namespace

struct AeroSynth::Impl {
    double R;
    SynthConfig cfg;
    double sigma = 0.12;
    genutil::NoiseSource noise;
    bool rec = false;
    std::vector<SentFrame> sentFrames;
    std::vector<SentMessage> sentMsgs;
    std::vector<SentLogon> sentLogons;

    struct Chan {
        int idx = 0, rate = 0;
        double userOffset = 0, baseHz = 0, ownCfo = 0, driftAmp = 0, driftPeriod = 300;
        double fsG = 0, amp = 0;
        std::unique_ptr<AeroFrameEncoder> enc;
        std::unique_ptr<AeroModulator> mod;
        std::vector<cf32> in;          // modulator output from sample inBase
        int64_t inBase = 0;
        double pos = 3, step = 0;       // input position of the next output (the taps reach 3 samples back)
        double phase = 0;              // carrier phase at the next sample
        uint64_t frameNo = 0;
        double frameSecs = 0;
        std::deque<AeroSu> queue;
        double nextMsg = 0, nextPos = 0;
        int posNo = 0;
        int script = 0, tablePart = 0;
        int nextLogon = 0;
        double nextLogonAt = 1.0, nextCycleAt = 600;
        std::mt19937 rng;
    };
    std::vector<Chan> ch;
    std::vector<cf32> tmp;
    uint64_t outCount = 0;

    Impl(const SynthConfig& c, double rate) : R(rate), cfg(c), noise((uint32_t)(c.modeOpt[1] ? c.modeOpt[1] : 1) * 7919u + 13u) {
        const int mask = c.modeOpt[0] ? c.modeOpt[0] : 6;
        const double ebn0 = std::pow(10.0, (c.modeVal[0] != 0 ? c.modeVal[0] : 12.0) / 10);
        const double tune = aeroTuning().tuneOffsetHz;
        const bool clean = c.modeOpt[2] == 1;
        struct Def { int bit, rate; double off, cfo, drift; };
        const Def defs[3] = {{4, 10500, -50e3, 1830, 40}, {2, 1200, 150e3, -2460, 30}, {1, 600, 100e3, 970, 25}};
        double S = 0;
        for (const Def& d : defs) {
            if (!(mask & d.bit)) continue;
            Chan k;
            k.idx = (int)ch.size();
            k.rate = d.rate;
            k.userOffset = d.off;
            k.baseHz = d.off - tune;
            k.ownCfo = clean ? 0 : d.cfo;
            k.driftAmp = clean ? 0 : d.drift;
            k.fsG = d.rate == 10500 ? 8.0 * d.rate : 16.0 * d.rate;
            k.enc = std::make_unique<AeroFrameEncoder>(d.rate);
            k.mod = std::make_unique<AeroModulator>(d.rate, k.fsG, c.sroPpm);
            k.step = k.fsG / R;
            k.frameSecs = aeroFrameFormat(d.rate)->totalBits() / (double)d.rate;
            k.rng.seed((uint32_t)(c.modeOpt[1] ? c.modeOpt[1] : 1) * 1000u + (uint32_t)d.rate);
            k.script = (int)ch.size() * 5;
            k.nextLogon = (int)ch.size() * 3;
            k.nextMsg = 3.0 + ch.size();
            k.nextPos = 4.5 + ch.size();
            k.posNo = (int)ch.size() * 2;
            k.amp = std::sqrt(ebn0 * d.rate * 2.0 / R);          // relative to sigma: P = Eb/N0 * Rb * N0, N0 = 2 sigma^2 / R
            S += k.amp;
            ch.push_back(std::move(k));
        }
        // keep the peak below about 0.9: the OQPSK carrier peaks at about 1.6 times its rms, noise at about 4 sigma
        sigma = std::min(0.12, 0.85 / (1.6 * S + 4.0));
        for (auto& k : ch) k.amp *= sigma;
    }

    bool validSu(const AeroSu& s) const {
        for (int i = 0; i < 12; i++) if (s.bytes[i]) return true;
        return false;
    }

    // what the frame starting now carries
    void schedule(Chan& k) {
        const double t = k.frameNo * k.frameSecs;
        // system table: one part in every frame on 10500, every fourth on the slow channels
        if (k.rate == 10500 || k.frameNo % 4 == 0) {
            AeroSu s = aeroSystemTableSu(kGes, k.tablePart);
            k.tablePart = (k.tablePart + 1) % 4;
            if (validSu(s)) k.queue.push_back(s);
        }
        // logons: one aircraft every few seconds at the start; later every ten minutes one logs off and on again
        if (k.nextLogon < 6 + k.idx * 3 && t >= k.nextLogonAt) {
            const Plane& p = kPlanes[k.nextLogon % 6];
            pushLogon(k, p.aes, true, t);
            k.nextLogon++;
            k.nextLogonAt = t + 2.5;
        } else if (t >= k.nextCycleAt) {
            const Plane& p = kPlanes[(int)(t / 600) % 6];
            pushLogon(k, p.aes, false, t);
            pushLogon(k, p.aes, true, t);
            k.nextCycleAt = t + 600;
        }
        if (t >= k.nextMsg) {
            const Script& sc = kScript[k.script % (int)(sizeof kScript / sizeof kScript[0])];
            k.script++;
            const Plane& p = kPlanes[sc.plane];
            AeroAcars m;
            m.aesId = p.aes; m.gesId = kGes; m.uplink = true;
            m.mode = "2"; m.registration = p.reg; m.flight = p.flight; m.label = sc.label;
            m.blockId = std::string(1, (char)('A' + (k.script % 26)));
            m.text = sc.text;
            m.crcOk = true;
            const auto sus = aeroAcarsToSus(m);
            for (const auto& s : sus) k.queue.push_back(s);
            if (rec && !sus.empty()) sentMsgs.push_back({k.idx, t, m});
            k.nextMsg = t + (k.rate == 10500 ? 6.0 : k.rate == 1200 ? 12.0 : 20.0);
        }
        schedulePosition(k, t);
    }
    // position reports: each channel takes the aircraft in turn
    void schedulePosition(Chan& k, double t) {
        if (t < k.nextPos) return;
        const AeroAcars m = positionReport(k.posNo % 6, t, k.posNo / 6);
        k.posNo++;
        const auto sus = aeroAcarsToSus(m);
        for (const auto& s : sus) k.queue.push_back(s);
        if (rec && !sus.empty()) sentMsgs.push_back({k.idx, t, m});
        k.nextPos = t + (k.rate == 10500 ? 4.0 : k.rate == 1200 ? 9.0 : 16.0);
    }
    void pushLogon(Chan& k, uint32_t aes, bool on, double t) {
        AeroSu s = aeroLogonSu(aes, kGes, on);
        if (!validSu(s)) return;
        k.queue.push_back(s);
        if (rec) sentLogons.push_back({k.idx, t, AeroLogon{aes, kGes, on}});
    }

    std::vector<uint8_t> frameBytes(Chan& k) {
        schedule(k);
        const AeroFrameFormat& f = *aeroFrameFormat(k.rate);
        std::vector<uint8_t> out;
        out.reserve(f.infoBytes());
        const size_t bb = aeroBlockBytes(k.rate);
        if (bb && f.infoBytes() % bb == 0) {
            for (size_t done = 0; done < (size_t)f.infoBytes(); done += bb) {
                std::vector<AeroSu> take;
                while (!k.queue.empty() && take.size() < bb / 12) { take.push_back(k.queue.front()); k.queue.pop_front(); }
                const std::vector<uint8_t> blk = aeroBuildBlock(take, k.rate);
                if (blk.size() != bb) { out.clear(); break; }
                out.insert(out.end(), blk.begin(), blk.end());
            }
        }
        if (out.size() != (size_t)f.infoBytes()) {
            // the SU layer cannot build blocks: SUs of random bytes with a valid check, so the radio layer still has a signal
            out.assign(f.infoBytes(), 0);
            for (size_t i = 0; i + 12 <= out.size(); i += 12) {
                for (int j = 0; j < 10; j++) out[i + j] = (uint8_t)k.rng();
                aeroSuSetCrc(&out[i]);
            }
            k.queue.clear();
        }
        return out;
    }

    void refill(Chan& k, size_t need) {
        while (k.mod->queuedBits() < need) {
            const std::vector<uint8_t> bytes = frameBytes(k);
            if (rec) sentFrames.push_back({k.idx, k.rate, k.frameNo, bytes});
            const auto bits = k.enc->frame(bytes.data(), aeroHeader(1, 0, (int)(k.frameNo >> 4), (int)k.frameNo));
            k.frameNo++;
            k.mod->pushBits(bits.data(), bits.size());
        }
    }

    void generate(cf32* out, size_t n) {
        for (size_t i = 0; i < n; i++) out[i] = cf32(0, 0);
        // carrier phases are integrated per block of 256 samples (the frequency changes slowly)
        for (auto& k : ch) {
            size_t done = 0;
            while (done < n) {
                const size_t m = std::min<size_t>(256, n - done);
                const double t = (double)(outCount + done) / R;
                const double f = k.baseHz + k.ownCfo + cfg.cfoHz + cfg.modeVal[1] * t + k.driftAmp * std::sin(2 * kPi * t / k.driftPeriod);
                const double w = 2 * kPi * f / R;
                std::complex<double> o = std::polar(1.0, k.phase);
                const std::complex<double> rot = std::polar(1.0, w);
                tmp.assign(m, cf32(0, 0));
                genBlock(k, tmp.data(), m, o, rot);
                for (size_t i = 0; i < m; i++) out[done + i] += tmp[i];
                k.phase = std::fmod(k.phase + w * (double)m, 2 * kPi);
                done += m;
            }
        }
        noise.add(out, n, (float)sigma);
        outCount += n;
    }

    void genBlock(Chan& k, cf32* out, size_t n, std::complex<double> o, std::complex<double> rot) {
        const Interp& ip = interp();
        for (size_t j = 0; j < n; j++) {
            const int64_t i0 = (int64_t)std::floor(k.pos);
            if (i0 + 5 - k.inBase > (int64_t)k.in.size()) {
                const size_t add = 2048;
                refill(k, (size_t)(add / (k.fsG / k.rate)) + 64);
                const size_t old = k.in.size();
                k.in.resize(old + add);
                k.mod->generate(k.in.data() + old, add);
            }
            const int ph = std::min(Interp::kPhases - 1, (int)((k.pos - (double)i0) * Interp::kPhases));
            const float* h = &ip.tab[(size_t)ph * Interp::kTaps];
            const cf32* x = &k.in[(size_t)(i0 - 3 - k.inBase)];
            float re = 0, im = 0;
            for (int q = 0; q < Interp::kTaps; q++) { re += h[q] * x[q].real(); im += h[q] * x[q].imag(); }
            out[j] = cf32((float)((re * o.real() - im * o.imag()) * k.amp), (float)((re * o.imag() + im * o.real()) * k.amp));
            o *= rot;
            k.pos += k.step;
        }
        const int64_t keep = (int64_t)std::floor(k.pos) - 8 - k.inBase;
        if (keep > 4096) { k.in.erase(k.in.begin(), k.in.begin() + keep); k.inBase += keep; }
    }
};

AeroSynth::AeroSynth(const SynthConfig& cfg, double sampleRate) : p_(std::make_unique<Impl>(cfg, sampleRate)) {}
AeroSynth::~AeroSynth() = default;
double AeroSynth::sampleRate() const { return p_->R; }
void AeroSynth::generate(cf32* out, size_t n) { p_->generate(out, n); }
std::vector<AeroSynth::ChannelInfo> AeroSynth::channels() const {
    std::vector<ChannelInfo> v;
    for (const auto& k : p_->ch) v.push_back({k.rate, k.userOffset});
    return v;
}
void AeroSynth::record(bool on) { p_->rec = on; }
std::vector<AeroSynth::SentFrame> AeroSynth::takeFrames() { return std::move(p_->sentFrames); }
std::vector<AeroSynth::SentMessage> AeroSynth::takeMessages() { return std::move(p_->sentMsgs); }
std::vector<AeroSynth::SentLogon> AeroSynth::takeLogons() { return std::move(p_->sentLogons); }
double AeroSynth::signalAmplitude(int channel) const { return channel >= 0 && channel < (int)p_->ch.size() ? p_->ch[channel].amp : 0; }
double AeroSynth::noiseSigma() const { return p_->sigma; }

std::unique_ptr<ModeSynth> makeAeroSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<AeroSynth>(cfg, sampleRate);
}

} // namespace dect2
