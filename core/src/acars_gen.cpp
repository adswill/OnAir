#include "dect2/acars_gen.h"
#include "dect2/aero_adsc.h"
#include "dect2/gen_util.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

namespace dect2 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBitRate = 2400.0;

struct Burst {
    int chan = 0;
    int64_t start = 0, total = 0;               // samples
    int64_t leadN = 0, modN = 0;                // carrier alone before the pre-key, samples of modulation
    std::vector<uint8_t> bits;
    float amp = 0;
    float depth = 0.6f;
    double carHz = 0;                           // relative to 0 Hz of the output
    // state
    int64_t n = 0;
    cf32 car{1.f, 0.f}, carStep, aud{1.f, 0.f}, step1200, step2400;
    double spb = 0;
    size_t bitIdx = 0;
    int cnt = 0;
    bool tone2400 = true;
};

struct Aircraft {
    std::string reg, flight, origin, dest;
    int chan = 0;
    double carOff = 0, levelDb = 0;
    double nextSec = 0;
    int blk = 0, msgNo = 0, phase = 0;
    int upBlk = 0;
    char lastDown = '0';
    double lat = 25.2, lon = 55.3;
    double nextPos = -1;                        // next position report (s), -1 = this aircraft sends none
    int posN = 0;
};

// The first six aircraft also send position reports, one of each kind a VHF downlink carries: ADS-C as label B6 (with and without a
// ground address in it), ADS-C as H1 "#M1B/B6", an ARINC 702 "POS" text report (H1) and a label 16 report. They fly their own routes over the
// Gulf, 10 times faster than real so that a track shows within a minute of test signal; report times run at the same pace.
struct PosPlan { double lat, lon, trackDeg, kt; int altFt; const char* ground; int kind; };   // kind: 0 B6, 1 H1 + B6, 2 H1 POS, 3 label 16
const PosPlan kPlans[6] = {
    {25.60, 56.80, 105, 480, 37000, "BOMCAYA", 0}, {24.10, 60.20, 95, 470, 39000, "MCTCAYA", 0}, {23.20, 64.10, 125, 490, 35000, "BOMCAYA", 1},
    {26.10, 53.50, 300, 460, 33000, "", 2},        {22.80, 58.40, 150, 485, 38000, "MCTCAYA", 0}, {27.40, 51.90, 20, 450, 31000, "", 3},
};
constexpr double kPosScale = 10;
constexpr double kPosStartSec = 12 * 3600 + 50 * 60;   // simulated report time starts at 12:50:00 UTC

void planAt(const PosPlan& r, double t, double& lat, double& lon) {
    const double nm = r.kt * t * kPosScale / 3600, a = r.trackDeg * kPi / 180;
    lat = r.lat + nm * std::cos(a) / 60;
    lon = r.lon + nm * std::sin(a) / (60 * std::cos(lat * kPi / 180));
}

const char* kRegs[][3] = {
    {"A6-EDA", "EK0201", "OMDB"}, {"A6-FEB", "FZ0201", "OMDB"}, {"A7-BAE", "QR0001", "OTHH"}, {"G-EUPA", "BA0107", "EGLL"},
    {"A6-ENT", "EK0073", "OMDB"}, {"HZ-AK1", "SV0123", "OEJN"}, {"A6-EEE", "EK0005", "OMDB"}, {"A9C-NA", "GF0511", "OBBI"},
    {"A6-FDB", "FZ0007", "OMDB"}, {"A7-ANA", "QR0013", "OTHH"}, {"G-XWBA", "BA0105", "EGLL"}, {"D-AIPA", "LH0630", "EDDF"},
    {"A6-EWA", "EK0771", "OMDB"}, {"VT-ANA", "AI0951", "VIDP"}, {"TC-JJA", "TK0761", "LTFM"}, {"A6-APE", "EY0011", "OMAA"},
    {"A6-EVO", "EK0041", "OMDB"}, {"4X-EDA", "LY0801", "LLBG"}, {"OE-LAA", "OS0121", "LOWW"}, {"HB-JNA", "LX0017", "LSZH"},
    {"PH-BXA", "KL0427", "EHAM"}, {"F-GKXA", "AF0218", "LFPG"}, {"EI-DEA", "EI0155", "EIDW"}, {"SU-GDA", "MS0987", "HECA"},
};
const char* kDests[] = {"EGLL", "OMDB", "OTHH", "LTFM", "EDDF", "VIDP", "OEJN", "LFPG", "EHAM", "OMAA"};

std::string clock4(double sec) {
    const int m = ((int)(sec / 60.0) + 7 * 60 + 14) % 1440;
    char b[8];
    snprintf(b, sizeof b, "%02d%02d", m / 60, m % 60);
    return b;
}

class AcarsSynth : public ModeSynth {
public:
    AcarsSynth(const AcarsGenOptions& o, const SynthConfig& c, double rate)
        : o_(o), cfg_(c), fs_(rate), fsEff_(rate * (1.0 + c.sroPpm * 1e-6)), rng_(o.seed ? o.seed : 1), noise_(18 + (uint32_t)o.seed) {
        if (o_.channelsHz.empty()) o_.channelsHz = {131.525e6};
        nch_ = (int)o_.channelsHz.size();
        o_.levelDb.resize((size_t)nch_, 0.0);
        o_.aircraft = std::max(1, std::min(24, o_.aircraft));
        o_.depth = std::max(0.05, std::min(0.98, o_.depth));
        chanFree_.assign((size_t)nch_, 0);
        // carrier amplitudes: noise has unit power per complex sample before the final scaling; a channel at its nominal level has
        // a carrier-to-noise ratio of snrDb in 7 kHz
        const double a0 = std::sqrt(std::pow(10.0, cfg_.snrDb / 10.0) * 7000.0 / fs_);
        double peak = 0;
        for (int k = 0; k < nch_; k++) {
            chanAmp_.push_back(a0 * std::pow(10.0, o_.levelDb[(size_t)k] / 20.0));
            peak += chanAmp_.back() * (1.0 + o_.depth) * 1.1;   // 1.1: carriers of one channel may differ in level
        }
        gain_ = (float)(0.85 / std::max(peak, 1e-9));
        for (int i = 0; i < o_.aircraft; i++) {
            Aircraft a;
            const auto* r = kRegs[i % 24];
            a.reg = r[0]; a.flight = r[1]; a.origin = r[2];
            a.dest = kDests[(size_t)(rng_() % 10)];
            if (a.dest == a.origin) a.dest = "EGLL";
            a.chan = i % nch_;
            a.carOff = ((double)(rng_() % 2001) / 1000.0 - 1.0) * o_.cfoSpreadHz;
            a.levelDb = -((double)(rng_() % 400) / 100.0);
            a.nextSec = 0.1 + (double)(rng_() % 2500) / 1000.0 * (i + 1) / o_.aircraft;
            a.phase = (int)(rng_() % 12);
            a.msgNo = (int)(rng_() % 40);
            a.lat = 24.0 + (double)(rng_() % 1000) / 200.0; a.lon = 50.0 + (double)(rng_() % 1000) / 100.0;
            if (o_.positions && i < 6) a.nextPos = 1.5 + 1.3 * i;
            ac_.push_back(a);
        }
    }

    double sampleRate() const override { return fs_; }

    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) out[i] = cf32(0.f, 0.f);
        const int64_t end = pos_ + (int64_t)n;
        // events whose time has come
        const double endSec = (double)end / fs_;
        // in time order, so that the signal does not depend on how the calls are chunked
        for (;;) {
            Aircraft* first = nullptr;
            bool isPos = false;
            double when = 0;
            for (auto& a : ac_) {
                if (!first || a.nextSec < when) { first = &a; isPos = false; when = a.nextSec; }
                if (a.nextPos >= 0 && a.nextPos < when) { first = &a; isPos = true; when = a.nextPos; }
            }
            if (!first || when >= endSec) break;
            if (isPos) {
                sendPosition(*first);
                first->nextPos += 7.0 / std::max(0.01, o_.rateFactor);
            } else {
                sendDownlink(*first);
                first->nextSec += expo(15.0 / std::max(0.01, o_.rateFactor));
            }
        }
        for (auto& b : bursts_) render(b, out, pos_, end);
        bursts_.erase(std::remove_if(bursts_.begin(), bursts_.end(), [&](const Burst& b) { return b.start + b.total <= end; }), bursts_.end());
        noise_.add(out, n, gain_ * 0.70710678f);
        pos_ = end;
    }

private:
    double expo(double mean) { return -mean * std::log(1.0 - (double)(rng_() % 1000000) / 1000000.0); }
    double uni(double a, double b) { return a + (b - a) * (double)(rng_() % 1000000) / 1000000.0; }

    // one block on the channel: returns the end time in seconds
    double queue(int chan, double reqSec, const AcarsBlockSpec& spec, double carOff, double levelDb) {
        const auto bytes = acarsBuildFrame(spec);
        Burst b;
        b.chan = chan;
        b.bits = acarsFrameBits(bytes);
        b.depth = (float)o_.depth;
        b.spb = fsEff_ / kBitRate;
        b.leadN = (int64_t)(0.006 * fs_);
        b.modN = (int64_t)(b.bits.size() * b.spb);
        const int64_t tailN = (int64_t)(0.006 * fs_);
        b.total = b.leadN + b.modN + tailN;
        int64_t st = (int64_t)(reqSec * fs_);
        st = std::max(st, chanFree_[(size_t)chan] + (int64_t)(o_.guardSec * fs_));
        st = std::max(st, pos_);
        b.start = st;
        chanFree_[(size_t)chan] = st + b.total;
        b.amp = (float)(chanAmp_[(size_t)chan] * std::pow(10.0, levelDb / 20.0)) * gain_;
        b.carHz = o_.channelsHz[(size_t)chan] - o_.centerHz + carOff + cfg_.cfoHz;
        const double w = 2 * kPi * b.carHz / fsEff_;
        b.carStep = cf32((float)std::cos(w), (float)std::sin(w));
        const double ph = uni(0, 2 * kPi);
        b.car = cf32((float)std::cos(ph), (float)std::sin(ph));
        const double pa = uni(0, 2 * kPi);
        b.aud = cf32((float)std::cos(pa), (float)std::sin(pa));
        const double w12 = 2 * kPi * 1200.0 / fsEff_, w24 = 2 * kPi * 2400.0 / fsEff_;
        b.step1200 = cf32((float)std::cos(w12), (float)std::sin(w12));
        b.step2400 = cf32((float)std::cos(w24), (float)std::sin(w24));
        bursts_.push_back(b);
        if (o_.log) {
            AcarsSent s;
            s.startSec = (double)st / fs_;
            s.endSec = (double)(st + b.total) / fs_;
            s.freqHz = o_.channelsHz[(size_t)chan];
            s.spec = spec;
            std::lock_guard<std::mutex> lk(o_.log->mu);
            o_.log->sent.push_back(s);
        }
        return (double)(st + b.total) / fs_;
    }

    void render(Burst& b, cf32* out, int64_t pos, int64_t end) {
        const int64_t s0 = std::max(pos, b.start), s1 = std::min(end, b.start + b.total);
        const int64_t ramp = (int64_t)(0.003 * fs_), mramp = (int64_t)(0.0005 * fs_) + 1;
        for (int64_t s = s0; s < s1; s++) {
            const int64_t k = s - b.start;
            float amp = b.amp;
            if (k < ramp) amp *= 0.5f * (1.f - std::cos((float)kPi * (float)k / (float)ramp));
            else if (b.total - k < ramp) amp *= 0.5f * (1.f - std::cos((float)kPi * (float)(b.total - k) / (float)ramp));
            float mod = 0.f;
            if (k >= b.leadN && k < b.leadN + b.modN) {
                mod = b.depth;
                const int64_t a = k - b.leadN, z = b.leadN + b.modN - k;
                if (a < mramp) mod *= (float)a / (float)mramp;
                if (z < mramp) mod *= (float)z / (float)mramp;
                // bit clock and tone
                const size_t idx = std::min(b.bits.size() - 1, (size_t)((double)(k - b.leadN) / b.spb));
                if (idx != b.bitIdx || (k == b.leadN)) {
                    const uint8_t prev = idx == 0 ? 1 : b.bits[idx - 1];
                    b.tone2400 = b.bits[idx] == prev;
                    b.bitIdx = idx;
                }
                b.aud *= b.tone2400 ? b.step2400 : b.step1200;
            }
            const float e = amp * (1.f + mod * b.aud.real());
            out[s - pos] += e * b.car;
            b.car *= b.carStep;
            if (++b.cnt >= 512) { b.cnt = 0; b.car /= std::abs(b.car); b.aud /= std::abs(b.aud); }
            b.n++;
        }
    }

    // ---------------------------------------------------------------- content
    std::string flightLine(Aircraft& a, char seq, const std::string& payload, std::string& msgNo) {
        char b[16];
        snprintf(b, sizeof b, "M%02d%c", a.msgNo % 100, seq);
        msgNo = b;
        return msgNo + a.flight + payload;
    }

    // one position report, in the shape the aircraft's plan says
    void sendPosition(Aircraft& a) {
        const int idx = (int)(&a - ac_.data());
        const PosPlan& r = kPlans[idx % 6];
        const double now = a.nextPos;
        double lat, lon;
        planAt(r, now, lat, lon);
        const double sim = kPosStartSec + now * kPosScale;
        const int n = a.posN++;
        std::string payload, label = "H1";
        if (r.kind <= 1) {
            std::vector<uint8_t> g = aeroAdscBasicGroup(lat, lon, r.altFt, std::fmod(sim, 3600.0), 6);
            const auto fid = aeroAdscFlightIdGroup(a.flight);
            g.insert(g.end(), fid.begin(), fid.end());
            const auto er = aeroAdscEarthRefGroup(r.trackDeg, r.kt, 0);
            g.insert(g.end(), er.begin(), er.end());
            if (n % 2 == 0) {                       // the predicted route and the weather every other time, as FANS aircraft do
                AeroAdscPoint p1, p2;
                planAt(r, now + 900 / kPosScale, p1.lat, p1.lon);
                planAt(r, now + 1800 / kPosScale, p2.lat, p2.lon);
                p1.altFt = p2.altFt = r.altFt;
                p1.etaSec = 900;
                const auto pr = aeroAdscPredictedRouteGroup(p1, p2);
                g.insert(g.end(), pr.begin(), pr.end());
                const auto mt = aeroAdscMeteoGroup(35 + idx * 4, 250 + idx * 10, -52 + idx);
                g.insert(g.end(), mt.begin(), mt.end());
            }
            const std::string app = aeroBuildAdscText(r.ground, a.reg, g);
            if (r.kind == 1) payload = "#M1B/B6 " + app.substr(1);
            else { label = "B6"; payload = app; }
        } else if (r.kind == 2) {
            const double la = std::fabs(lat), lo = std::fabs(lon);
            const int laD = (int)la, loD = (int)lo;
            const int laM = std::min(599, (int)std::lround((la - laD) * 600)), loM = std::min(599, (int)std::lround((lo - loD) * 600));
            const int sec = (int)std::fmod(sim, 86400.0);
            char b[160];
            snprintf(b, sizeof b, "#M1BPOS%c%02d%03d%c%03d%03d,PARAR,%02d%02d%02d,%03d,TOTOX", lat < 0 ? 'S' : 'N', laD, laM, lon < 0 ? 'W' : 'E', loD, loM,
                     sec / 3600, sec / 60 % 60, sec % 60, r.altFt / 100);
            payload = b;
        } else {
            char b[96];
            snprintf(b, sizeof b, "%c %.3f,%c %.3f,%d,6, 290", lat < 0 ? 'S' : 'N', std::fabs(lat), lon < 0 ? 'W' : 'E', std::fabs(lon), r.altFt);
            label = "16";
            payload = b;
        }
        AcarsBlockSpec s;
        s.reg = a.reg;
        s.ack = 0x15;
        s.label[0] = label[0]; s.label[1] = label[1];
        s.blockId = (char)('0' + (a.blk++ % 10));
        std::string mn;
        s.text = flightLine(a, 'A', payload, mn);
        a.msgNo++;
        s.lastBlock = true;
        queue(a.chan, now, s, a.carOff, a.levelDb);
        a.lastDown = s.blockId;
    }

    void sendDownlink(Aircraft& a) {
        const double now = a.nextSec;
        static const char* seq[] = {"QA", "5Z", "QB", "H1", "H1", "80", "H1", "10", "QC", "QD", "SA", "Q0"};
        int ty = a.phase++ % 12;
        if (rng_() % 100 < 25) { static const int extra[] = {11, 1, 5, 3, 10}; ty = extra[rng_() % 5]; }
        const std::string lab = seq[ty];
        const std::string t = clock4(now);
        std::string payload;
        bool longMsg = false;
        if (lab == "QA" || lab == "QB") payload = a.origin + t;
        else if (lab == "QC" || lab == "QD") payload = a.dest + t;
        else if (lab == "10") { char b[64]; snprintf(b, sizeof b, "ARR01%-7s%s%s", a.flight.c_str(), a.dest.c_str(), clock4(now + 600).c_str()); payload = b; }
        else if (lab == "H1") {
            a.lat += 0.05; a.lon += 0.08;
            char b[160];
            snprintf(b, sizeof b, "#M1B/PS POS N%02d%03d E%03d%03d,FL%03d,%s,%s,%02d", (int)a.lat, (int)((a.lat - (int)a.lat) * 1000), (int)a.lon,
                     (int)((a.lon - (int)a.lon) * 1000), 300 + (int)(rng_() % 90), t.c_str(), a.dest.c_str(), (int)(rng_() % 60));
            payload = b;
        } else if (lab == "5Z") {
            static const char* f[] = {"REQ GATE INFO ", "REQ CREW CONNECTIONS ", "CABIN LOG LAV L2 INOP ", "FUEL REQ FOR "};
            payload = std::string(f[rng_() % 4]) + a.dest;
        } else if (lab == "80") {
            char b[120];
            snprintf(b, sizeof b, "\r\n01 TIME %s\r\nFUEL %d.%d\r\nWX REQ %s", t.c_str(), 5 + (int)(rng_() % 8), (int)(rng_() % 10), a.dest.c_str());
            payload = b;
        } else if (lab == "SA") {
            const double s = now;
            char b[48];
            snprintf(b, sizeof b, "0EV%02d%02d%02dV", (int)(s / 3600) % 24, (int)(s / 60) % 60, (int)s % 60);
            payload = b;
        } else if (lab == "Q0") payload = "";
        // an occasional two-block message: a long weather request
        if (lab == "5Z" && rng_() % 6 == 0) {
            payload = "WX REQUEST " + a.dest + " " + a.origin + " ";
            while (payload.size() < 300) payload += "METAR TAF SIGMET AIRMET NOTAM ";
            longMsg = true;
        }
        a.msgNo++;
        std::vector<std::string> parts;
        if (longMsg) { parts.push_back(payload.substr(0, 200)); parts.push_back(payload.substr(200, 60)); }
        else parts.push_back(payload);
        double end = now;
        for (size_t p = 0; p < parts.size(); p++) {
            AcarsBlockSpec s;
            s.reg = a.reg;
            s.ack = 0x15;
            s.label[0] = lab[0]; s.label[1] = lab[1];
            s.blockId = (char)('0' + (a.blk++ % 10));
            std::string mn;
            s.text = flightLine(a, (char)('A' + p), parts[p], mn);
            s.lastBlock = p + 1 == parts.size();
            end = queue(a.chan, p == 0 ? now : end, s, a.carOff, a.levelDb);
            a.lastDown = s.blockId;
        }
        if (!o_.uplinks) return;
        // the ground answers: an acknowledgement, and now and then a text
        AcarsBlockSpec u;
        u.reg = a.reg;
        u.ack = (uint8_t)a.lastDown;
        u.blockId = (char)('A' + (a.upBlk++ % 26));
        const int r = (int)(rng_() % 100);
        if (r < 55 || lab == "QA" || lab == "QB" || lab == "QC" || lab == "QD") {
            u.label[0] = '_'; u.label[1] = 'd';
            u.hasText = false;
        } else if (r < 80) {
            u.label[0] = 'H'; u.label[1] = '1';
            u.text = "- #M1/WX METAR " + a.dest + " " + t + "Z 31008KT 9999 FEW040 36/18 Q1004 NOSIG";
        } else if (r < 92) {
            u.label[0] = '5'; u.label[1] = 'Z';
            u.text = "REQUEST RECEIVED. GATE B" + std::to_string(10 + (int)(rng_() % 30)) + " WILL BE CONFIRMED BEFORE TOP OF DESCENT";
        } else {
            u.label[0] = '8'; u.label[1] = '0';
            u.text = "CLEARED TO " + a.dest + " VIA DOGAR UL612 FL" + std::to_string(330 + 10 * (int)(rng_() % 8)) + " SQUAWK " +
                     std::to_string(4000 + (int)(rng_() % 1000)) + " CONTACT CONTROL 125.5";
        }
        queue(a.chan, end + uni(0.3, 1.2), u, 0.0, -1.5);
    }

    AcarsGenOptions o_;
    SynthConfig cfg_;
    double fs_, fsEff_;
    std::mt19937 rng_;
    genutil::NoiseSource noise_;
    int nch_ = 1;
    std::vector<double> chanAmp_;
    std::vector<int64_t> chanFree_;
    float gain_ = 1.f;
    std::vector<Aircraft> ac_;
    std::vector<Burst> bursts_;
    int64_t pos_ = 0;
};

} // namespace

std::vector<uint8_t> acarsFrameBits(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (uint8_t b : bytes)
        for (int i = 0; i < 8; i++) bits.push_back((uint8_t)((b >> i) & 1));
    return bits;
}

std::unique_ptr<ModeSynth> makeAcarsSynthEx(const AcarsGenOptions& o, const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 1e5) return nullptr;
    return std::make_unique<AcarsSynth>(o, cfg, sampleRate);
}

std::unique_ptr<ModeSynth> makeAcarsSynth(const SynthConfig& cfg, double sampleRate) {
    AcarsGenOptions o;
    if (cfg.modeOpt[0] > 0) o.aircraft = cfg.modeOpt[0];
    o.positions = true;                         // the app's test signal fills the aircraft map
    if (cfg.modeOpt[1] > 0) o.seed = (uint32_t)cfg.modeOpt[1];
    if (cfg.modeOpt[2] > 0) o.depth = cfg.modeOpt[2] / 100.0;
    if (cfg.modeOpt[3] == 1) o.channelsHz = {131.525e6};
    else if (cfg.modeOpt[3] == 2) o.channelsHz = {131.125e6, 131.550e6, 131.725e6};
    if (cfg.modeVal[0] > 0) o.rateFactor = cfg.modeVal[0];
    if (cfg.modeVal[1] > 0 && !o.channelsHz.empty()) { o.levelDb.assign(o.channelsHz.size(), 0.0); o.levelDb.back() = -cfg.modeVal[1]; }
    return makeAcarsSynthEx(o, cfg, sampleRate);
}

} // namespace dect2
