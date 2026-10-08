// Radiosonde receiver (see sonde_rx.h).
#include "dect2/sonde_rx.h"
#include "dect2/fftutil.h"
#include "dect2/sonde_geo.h"
#include "dect2/sonde_rs41.h"
#include "dect2/sonde_rs92.h"
#include "sonde_chan.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#if defined(__APPLE__) || defined(__linux__)
#include <time.h>
#endif

namespace dect2 {

using sondedsp::Decim;
using sondedsp::SymbolChain;

namespace {

// CPU time of the calling thread where the system has it (a busy machine then does not make the receiver look slow), else wall time
double threadCpuSeconds() {
#if defined(__APPLE__) || defined(__linux__)
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
#endif
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr int kMaxChan = 8;
constexpr int kMaxSondes = 16;
constexpr int kTrackCap = 600;
constexpr int kTrackedSondes = 5;       // sondes whose track goes into a report (5 x 600 x 24 bytes)
constexpr double kSearchPeriodS = 0.5;
constexpr double kChanCutoffHz = 10000.0;       // half width of the channel filter
constexpr double kChanNoiseBwHz = 20000.0;      // noise bandwidth of it
constexpr double kReportPeriodS = 0.25;
constexpr int kSearchBlocks = 32;

int kindOfType(const std::string& t) {
    if (t == "RS41") return SondeRs41;
    if (t == "RS92") return SondeRs92;
    if (t == "DFM") return SondeDfm;
    if (t == "M10") return SondeM10;
    if (t == "M20") return SondeM20;
    if (t == "LMS6") return SondeLms6;
    return SondeUnknown;
}

struct SondeRec {
    SondeInfo info;
    std::string key;
    double lastFixT = -1e9, firstT = 0;
    std::vector<SondeTrackPoint> track;
    double trackStepS = 1.0, lastTrackT = -1e9;
    double announcedHz = 0;
};

struct Carrier {
    double offHz = 0;
    float level = 0;
    bool assigned = false;
    int channel = -1;
};

class Channel {
public:
    struct Dec {
        std::unique_ptr<SondeBitDecoder> d;
        int kind = 0;
        int chain = 0;
        Rs41Decoder* rs41 = nullptr;
        Rs92Decoder* rs92 = nullptr;
    };
    struct Fix { int kind; SondeFix fix; double announcedHz; };

    int id = 0;
    bool used = false;
    double fs = 0, fs1 = 0, fs2 = 0;
    double freq = 0, freq0 = 0;             // mixer frequency (Hz, relative to 0 Hz of the input), and where the carrier was first found
    double createdT = 0, lastSeenT = 0, lastFixT = -1;
    float snrDb = 0;
    std::string sondeKey;
    int lockedKind = -1;
    double lastPresentT = -1;

    void init(int chanId, double fsIn, double f, double t, double noiseVar) {
        id = chanId; used = true; fs = fsIn; freq = freq0 = f; createdT = lastSeenT = t; lastFixT = -1;
        lockedKind = -1; sondeKey.clear(); snrDb = 0; pn = noiseVar * kChanNoiseBwHz / fsIn;
        int R1 = std::max(1, (int)std::floor(fs / 200000.0));
        fs1 = fs / R1;
        const int R2 = std::max(1, (int)std::floor(fs1 / 96000.0));
        fs2 = fs1 / R2;
        const int taps1 = R1 == 1 ? 1 : 5 * R1 + 1;
        // stage 1: pass +-22 kHz, stop from fs1 - 22 kHz
        s1.init(R1, taps1, 0.5 / R1, 7.0);
        s1.setMixer(freq / fs);
        // stage 2: pass +-10 kHz, stop above about 22 kHz (Kaiser: taps ~ 3.1 / transition)
        const int taps2 = std::max(15, (int)std::ceil(3.2 * fs1 / 12000.0)) | 1;
        s2.init(R2, taps2, kChanCutoffHz / fs1, 7.0);
        b1.clear(); b2.clear();
        prevY = cf32(1, 0);
        pAvg = 0; present = false; dc = 0; dcN = 0; afcSum = 0; afcCnt = 0;
        aP = 1.0 / (0.001 * fs2);
        dcTau = 0.08 * fs2;
        afcLen = (int)(0.1 * fs2);
        // decoders
        decs.clear(); chains.clear(); syms.clear(); symT.clear();
        auto addDec = [&](std::unique_ptr<SondeBitDecoder> d, int kind, Rs41Decoder* r41, Rs92Decoder* r92 = nullptr) {
            if (!d) return;
            const double baud = d->symbolRate();
            int ci = -1;
            for (size_t i = 0; i < chains.size(); i++) if (std::fabs(chains[i].baud() - baud) < 1e-6) ci = (int)i;
            if (ci < 0) { chains.emplace_back(); chains.back().init(fs2, baud); ci = (int)chains.size() - 1; }
            Dec e; e.d = std::move(d); e.kind = kind; e.chain = ci; e.rs41 = r41; e.rs92 = r92;
            decs.push_back(std::move(e));
        };
        { auto d = std::make_unique<Rs41Decoder>(); Rs41Decoder* r = d.get(); addDec(std::move(d), SondeRs41, r); }
        { auto d = std::make_unique<Rs92Decoder>(); Rs92Decoder* r = d.get(); addDec(std::move(d), SondeRs92, nullptr, r); }
        addDec(makeDfmDecoder(), SondeDfm, nullptr);
        addDec(makeM10Decoder(), SondeM10, nullptr);
        addDec(makeM20Decoder(), SondeM20, nullptr);
        syms.assign(chains.size(), {});
        symT.assign(chains.size(), 0.0);
    }

    void release() { used = false; decs.clear(); chains.clear(); }
    void setNoise(double noiseVar) { pn = noiseVar * kChanNoiseBwHz / fs; }
    void retune(double f) { freq = f; s1.setMixer(freq / fs); }

    // x: input samples starting at stream time t0
    void process(const cf32* x, size_t n, double t0, std::vector<Fix>& fixes) {
        b1.clear(); b2.clear();
        s1.process(x, n, b1);
        s2.process(b1.data(), b1.size(), b2);
        const double dt2 = 1.0 / fs2;
        const float scale = (float)(fs2 / (2 * M_PI));
        for (size_t i = 0; i < b2.size(); i++) {
            const cf32 y = b2[i];
            const float p = y.real() * y.real() + y.imag() * y.imag();
            pAvg += (p - pAvg) * aP;
            const bool pres = present ? (pAvg > 1.8 * pn) : (pAvg > 2.6 * pn);
            const double tHere = t0 + (double)i * dt2;
            if (pres != present) {
                if (!pres) flush(tHere, fixes);
                // a short dropout does not make the decoders forget what they know (a DFM sonde needs seconds to send its
                // configuration again); a pause between bursts does
                const bool longGap = pres && (lastPresentT < 0 || tHere - lastPresentT > 0.15);
                present = pres;
                for (auto& c : chains) c.reset();
                if (longGap) for (auto& e : decs) e.d->reset();
                dc = 0; dcN = 0; afcSum = 0; afcCnt = 0;
            }
            const cf32 cp = y * std::conj(prevY);
            prevY = y;
            if (!present) continue;
            lastPresentT = tHere;
            const float d = std::atan2(cp.imag(), cp.real()) * scale;
            if (dcN < dcTau) { dcN += 1; dc += (d - dc) / (float)dcN; }
            else dc += (d - dc) * (float)(1.0 / dcTau);
            afcSum += d; afcCnt++;
            for (size_t c = 0; c < chains.size(); c++) {
                if (!chainActive(c)) continue;
                const size_t before = syms[c].size();
                chains[c].step(d, dc, syms[c]);
                if (before == 0 && !syms[c].empty()) symT[c] = tHere;
            }
            if (afcCnt >= afcLen) {
                const double mean = afcSum / afcCnt;
                afcSum = 0; afcCnt = 0;
                if (std::fabs(mean) > 40.0) {
                    const double step = std::clamp(0.7 * mean, -2000.0, 2000.0);
                    retune(freq + step);
                    dc -= (float)step;
                }
            }
        }
        flush(t0 + (double)n / fs, fixes);
    }

    double noiseVar() const { return pn * fs / kChanNoiseBwHz; }

private:
    Decim s1, s2;
    std::vector<cf32> b1, b2;
    cf32 prevY{1, 0};
    double pn = 1e-9;
    float pAvg = 0, aP = 0.01f;
    bool present = false;
    float dc = 0;
    double dcN = 0, dcTau = 1000, afcSum = 0;
    int afcCnt = 0, afcLen = 1000;
    std::vector<Dec> decs;
    std::vector<SymbolChain> chains;
    std::vector<std::vector<uint8_t>> syms;
    std::vector<double> symT;

    bool chainActive(size_t c) const {
        for (const auto& e : decs)
            if ((size_t)e.chain == c && (lockedKind < 0 || e.kind == lockedKind)) return true;
        return false;
    }

    void flush(double tNow, std::vector<Fix>& fixes) {
        for (size_t c = 0; c < chains.size(); c++) {
            if (syms[c].empty()) continue;
            for (auto& e : decs) {
                if ((size_t)e.chain != c) continue;
                if (lockedKind >= 0 && e.kind != lockedKind) continue;
                std::vector<SondeFix> out;
                e.d->push(syms[c].data(), syms[c].size(), symT[c], out);
                for (auto& f : out) {
                    Fix fx; fx.kind = e.kind; fx.fix = f; fx.announcedHz = e.rs41 ? e.rs41->announcedFreqHz() : e.rs92 ? e.rs92->announcedFreqHz() : 0;
                    fixes.push_back(std::move(fx));
                    if (f.crcOk) { lockedKind = e.kind; lastFixT = tNow; }
                }
            }
            syms[c].clear();
        }
        if (lockedKind >= 0 && lastFixT >= 0 && tNow - lastFixT > 20.0) lockedKind = -1;
    }
};

} // namespace

// ------------------------------------------------------------------------------------------------

struct SondeReceiver::Impl {
    // settings and the published report
    std::mutex mu;
    double fs = 0;
    std::atomic<double> sigOff{0}, centerHz{0};      // set from the interface thread, read by the worker
    std::atomic<int> maxChan{kMaxChan};
    std::function<void(const std::string&)> log;
    SondeTelemetry tel;
    bool syncMode = false;
    bool clearReq = false;
    // thread
    std::mutex procMu;
    std::unique_ptr<IqRing> ring;
    std::thread th;
    std::atomic<bool> run{false};
    std::mutex cvMu;
    std::condition_variable cv;
    std::atomic<uint64_t> dropped{0}, fed{0}, processed{0};
    std::atomic<double> cpu{0};
    // processing state (under procMu)
    uint64_t streamN = 0;
    uint64_t nextSearchAt = 0;
    double lastReportT = -1;
    int fftN = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    double winSq = 1;
    std::vector<cf32> blk;
    std::vector<double> accP;
    size_t fill = 0;
    int blocksLeft = 0;
    bool capturing = false;
    double noiseVar = 0;
    std::vector<Channel> chans = std::vector<Channel>(kMaxChan);
    std::vector<Carrier> carriers;
    struct Block { double f, until; };
    std::vector<Block> blocked;
    std::vector<SondeRec> sondes;
    uint64_t okFrames = 0, badFrames = 0, searches = 0;
    double lastDecodeT = -1e9;
    std::vector<Channel::Fix> fixes;

    ~Impl() { stop(); }

    void stop() {
        run = false;
        cv.notify_all();
        if (th.joinable()) th.join();
    }
    void say(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }

    void configure(double rate) {
        std::lock_guard<std::mutex> pl(procMu);
        std::lock_guard<std::mutex> lk(mu);
        fs = rate;
        resetState();
    }

    // call with procMu and mu held
    void resetState() {
        streamN = 0; lastReportT = -1; capturing = false; fill = 0;
        for (auto& c : chans) c.release();
        carriers.clear(); blocked.clear(); sondes.clear();
        okFrames = badFrames = searches = 0;
        lastDecodeT = -1e9;
        noiseVar = 0;
        if (fs > 0) {
            // FFT bins of about 1.2 kHz or finer
            int n = 256;
            while (fs / n > 1300.0 && n < (1 << 17)) n <<= 1;
            fftN = n;
            fft = std::make_unique<Fft>(n);
            win.resize((size_t)n);
            winSq = 0;
            for (int i = 0; i < n; i++) { win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / n)); winSq += (double)win[(size_t)i] * win[(size_t)i]; }
            blk.assign((size_t)n, cf32());
            nextSearchAt = (uint64_t)(0.05 * fs);
        }
        const uint64_t s = tel.seq;
        tel = SondeTelemetry();
        tel.seq = s;
        tel.bandHz = fs;
        tel.centerHz = centerHz;
    }

    // ---- carrier search ----
    void feedSearch(const cf32* x, size_t n) {
        size_t i = 0;
        while (i < n) {
            const uint64_t pos = streamN + i;
            if (!capturing) {
                if (pos < nextSearchAt) {
                    const uint64_t gap = nextSearchAt - pos;
                    i += (size_t)std::min<uint64_t>(gap, n - i);
                    continue;
                }
                capturing = true; blocksLeft = kSearchBlocks; fill = 0;
                accP.assign((size_t)fftN, 0.0);
            }
            const size_t seg = std::min(n - i, (size_t)fftN - fill);
            std::memcpy(&blk[fill], x + i, seg * sizeof(cf32));
            fill += seg; i += seg;
            if ((int)fill == fftN) {
                fill = 0;
                for (int k = 0; k < fftN; k++) blk[(size_t)k] *= win[(size_t)k];
                fft->forward(blk.data());
                for (int k = 0; k < fftN; k++) accP[(size_t)k] += (double)std::norm(blk[(size_t)k]);
                if (--blocksLeft == 0) {
                    capturing = false;
                    nextSearchAt = streamN + i + (uint64_t)(kSearchPeriodS * fs);
                    analyze((double)(streamN + i) / fs);
                }
            }
        }
    }

    void analyze(double tNow) {
        searches++;
        const int N = fftN;
        const double bin = fs / N;
        // fftshift: j = 0 is -fs/2
        std::vector<double> P((size_t)N);
        for (int j = 0; j < N; j++) P[(size_t)j] = accP[(size_t)((j + N / 2) % N)] / kSearchBlocks;
        // the DC spike of real radios: replace the three centre bins by their neighbours
        {
            const int c = N / 2;
            const double rep = 0.5 * (P[(size_t)c - 3] + P[(size_t)c + 3]);
            for (int j = c - 1; j <= c + 1; j++) P[(size_t)j] = rep;
        }
        const int lo = (int)(N * 0.05), hi = (int)(N * 0.95);       // the radio's filter rolls off at the edges
        std::vector<double> tmp(P.begin() + lo, P.begin() + hi);
        std::nth_element(tmp.begin(), tmp.begin() + (long)tmp.size() / 2, tmp.end());
        const double floorBin = tmp[tmp.size() / 2] * 1.0;
        if (floorBin <= 0) return;
        noiseVar = floorBin / winSq;
        for (auto& c : chans) if (c.used) c.setNoise(noiseVar);
        // box sum over about 8 kHz
        const int w = std::max(1, (int)std::lround(4000.0 / bin));
        std::vector<double> cum((size_t)N + 1, 0.0);
        for (int j = 0; j < N; j++) cum[(size_t)j + 1] = cum[(size_t)j] + P[(size_t)j];
        auto boxR = [&](int j) { const int a = std::max(0, j - w), b = std::min(N - 1, j + w); return (cum[(size_t)b + 1] - cum[(size_t)a]) / ((b - a + 1) * floorBin); };
        const double thr = std::pow(10.0, 0.6);
        std::vector<std::pair<double, int>> cand;
        const int sep = std::max(2, (int)std::lround(14000.0 / bin));
        for (int j = lo + w; j < hi - w; j++) {
            const double r = boxR(j);
            if (r < thr) continue;
            bool isMax = true;
            const int a = std::max(lo, j - sep / 2), b = std::min(hi - 1, j + sep / 2);
            for (int k = a; k <= b && isMax; k++) if (k != j && (boxR(k) > r || (boxR(k) == r && k < j))) isMax = false;
            if (isMax) cand.emplace_back(r, j);
        }
        std::sort(cand.begin(), cand.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::vector<Carrier> found;
        const int cw = std::max(1, (int)std::lround(6000.0 / bin));
        for (auto& cj : cand) {
            if (found.size() >= 32) break;
            const int j = cj.second;
            // spur test: the strongest bin against the mean over +-6 kHz
            double sum = 0, mx = 0, sw = 0, sf = 0;
            int cnt = 0;
            for (int k = std::max(0, j - cw); k <= std::min(N - 1, j + cw); k++) {
                sum += P[(size_t)k]; mx = std::max(mx, P[(size_t)k]); cnt++;
                const double s = std::max(0.0, P[(size_t)k] - floorBin);
                sw += s; sf += s * (k - N / 2) * bin;
            }
            if (mx > 7.0 * sum / cnt) continue;
            if (sw <= 0) continue;
            Carrier c;
            c.offHz = sf / sw;
            double sig = 0; int nb = 0;
            for (int k = std::max(0, j - cw / 2 - 1); k <= std::min(N - 1, j + cw / 2 + 1); k++) { sig += std::max(0.0, P[(size_t)k] - floorBin); nb++; }
            c.level = (float)(10.0 * std::log10(std::max(1e-3, sig / (floorBin * nb))));
            found.push_back(c);
        }
        // channels
        blocked.erase(std::remove_if(blocked.begin(), blocked.end(), [&](const Block& b) { return b.until < tNow; }), blocked.end());
        for (auto& car : found) {
            Channel* m = nullptr;
            for (auto& ch : chans) if (ch.used && std::fabs(ch.freq - car.offHz) < 7500.0) { m = &ch; break; }
            if (m) {
                m->lastSeenT = tNow; m->snrDb = car.level;
                car.assigned = true; car.channel = m->id;
                if (m->lastFixT < 0 && std::fabs(m->freq - car.offHz) > 300.0) m->retune(car.offHz);
                continue;
            }
            bool isBlocked = false;
            for (auto& b : blocked) if (std::fabs(b.f - car.offHz) < 6000.0) isBlocked = true;
            if (isBlocked || car.level < 6.0f) continue;
            Channel* fr = nullptr;
            int usedN = 0;
            for (auto& ch : chans) { if (ch.used) usedN++; else if (!fr) fr = &ch; }
            if (!fr || usedN >= maxChan.load()) continue;
            fr->init((int)(fr - chans.data()), fs, car.offHz, tNow, noiseVar);
            fr->snrDb = car.level;
            car.assigned = true; car.channel = fr->id;
            char b[100];
            snprintf(b, sizeof b, "Radiosonde: carrier at %+.1f kHz, %.0f dB", car.offHz / 1000.0, car.level);
            say(b);
        }
        // channels that never decoded, or that lost their sonde, are given up
        for (auto& ch : chans) {
            if (!ch.used) continue;
            const bool never = ch.lastFixT < 0 && tNow - ch.createdT > 15.0;
            const bool lost = ch.lastFixT >= 0 && tNow - ch.lastFixT > 30.0;
            const bool drift = std::fabs(ch.freq - ch.freq0) > 12000.0;
            if (never || lost || drift) {
                if (never) blocked.push_back({ch.freq, tNow + 60.0});
                for (auto& s : sondes) if (s.info.channel == ch.id) s.info.channel = -1;
                ch.release();
            }
        }
        // a channel whose carrier is off at the moment of the search (RS41 sends bursts) stays in the list
        for (auto& ch : chans) {
            if (!ch.used) continue;
            bool listed = false;
            for (auto& c : found) if (c.channel == ch.id) listed = true;
            if (!listed) { Carrier c; c.offHz = ch.freq; c.level = ch.snrDb; c.assigned = true; c.channel = ch.id; found.push_back(c); }
        }
        std::stable_sort(found.begin(), found.end(), [](const Carrier& a, const Carrier& b) { return a.level > b.level; });
        carriers = found;
    }

    // ---- results ----
    SondeRec* recFor(const std::string& key, const SondeFix& f, double tNow) {
        for (auto& s : sondes) if (s.key == key) return &s;
        if ((int)sondes.size() >= kMaxSondes) {      // drop the one heard longest ago
            size_t old = 0;
            for (size_t i = 1; i < sondes.size(); i++) if (sondes[i].lastFixT < sondes[old].lastFixT) old = i;
            sondes.erase(sondes.begin() + (long)old);
        }
        SondeRec r;
        r.key = key; r.firstT = tNow;
        r.info.type = f.type; r.info.kind = kindOfType(f.type); r.info.serial = f.serial;
        sondes.push_back(std::move(r));
        return &sondes.back();
    }

    void handleFix(Channel& ch, const Channel::Fix& fx, double tNow) {
        const SondeFix& f = fx.fix;
        if (f.crcOk) { okFrames++; lastDecodeT = tNow; } else badFrames++;
        std::string key;
        if (!f.serial.empty()) key = f.type + ":" + f.serial;
        else if (!ch.sondeKey.empty()) key = ch.sondeKey;
        if (key.empty()) return;
        if (!f.crcOk) {
            for (auto& s : sondes) if (s.key == key) { s.info.framesBad++; break; }
            return;
        }
        const bool isNew = [&] { for (auto& s : sondes) if (s.key == key) return false; return true; }();
        SondeRec* r = recFor(key, f, tNow);
        ch.sondeKey = key;
        SondeInfo& s = r->info;
        if (isNew) {
            char b[160];
            snprintf(b, sizeof b, "Radiosonde: %s %s heard at %+.1f kHz", f.subtype.empty() ? f.type.c_str() : f.subtype.c_str(), f.serial.c_str(), ch.freq / 1000.0);
            say(b);
        }
        r->lastFixT = tNow;
        s.channel = ch.id;
        s.offsetHz = ch.freq - sigOff;
        s.snrDb = ch.snrDb;
        s.framesOk++;
        if (!f.subtype.empty()) s.subtype = f.subtype;
        if (f.frame >= 0) s.frame = f.frame;
        if (!f.serial.empty()) s.serial = f.serial;
        if (f.hasTime) { s.hasTime = true; s.unixTime = f.unixTime; }
        if (f.hasPos) {
            s.hasPos = true; s.lat = f.lat; s.lon = f.lon; s.altM = f.altM;
            s.maxAltM = std::max(s.maxAltM, f.altM);
            if (tNow - r->lastTrackT >= 0.8 * r->trackStepS) {
                SondeTrackPoint tp;
                tp.lat = (float)f.lat; tp.lon = (float)f.lon; tp.altM = (float)f.altM;
                tp.unixT = f.hasTime ? (uint32_t)std::llround(f.unixTime) : 0;
                if (f.hasTemp) tp.tempC = (float)f.tempC;
                if (f.hasHumidity) tp.humidity = (float)f.humidity;
                r->track.push_back(tp);
                r->lastTrackT = tNow;
                if ((int)r->track.size() > kTrackCap) {
                    std::vector<SondeTrackPoint> th;
                    for (size_t i = 0; i < r->track.size(); i += 2) th.push_back(r->track[i]);
                    r->track.swap(th);
                    r->trackStepS *= 2;
                }
            }
        }
        if (f.hasVel) { s.hasVel = true; s.vSpeed = f.vSpeed; s.hSpeed = f.hSpeed; s.headingDeg = f.headingDeg; }
        if (f.sats >= 0) s.sats = f.sats;
        s.hasTemp = f.hasTemp; if (f.hasTemp) s.tempC = f.tempC;
        s.hasHumidity = f.hasHumidity; if (f.hasHumidity) s.humidity = f.humidity;
        s.hasPressure = f.hasPressure; if (f.hasPressure) s.pressureHpa = f.pressureHpa;
        if (f.batteryV >= 0) s.batteryV = f.batteryV;
        if (f.burstKillS >= 0) s.burstKillS = f.burstKillS;
        s.note = f.note;
        // calibration progress from the note "calibrating n/m"
        int a = 0, m = 0;
        if (std::sscanf(f.note.c_str(), "calibrating %d/%d", &a, &m) == 2) { s.calDone = a; s.calTotal = m; }
        else if (f.type == "RS41") { s.calDone = s.calTotal = kRs41CalFrames; }
        else if (f.type == "RS92") { s.calDone = s.calTotal = kRs92CalFrames; }
        if (fx.announcedHz > 0) r->announcedHz = fx.announcedHz;
    }

    void report(double tNow) {
        SondeTelemetry t;
        t.centerHz = centerHz; t.bandHz = fs; t.streamTimeS = tNow;
        t.channelsMax = maxChan.load();
        t.searches = searches;
        t.blocksOk = okFrames; t.blocksBad = badFrames;
        for (auto& c : chans) if (c.used) t.channelsUsed++;
        // sondes, most recently heard first
        std::vector<size_t> ord(sondes.size());
        for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
        std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return sondes[a].lastFixT > sondes[b].lastFixT; });
        double bestCfo = 0; float bestSnr = -1000;
        int rank = 0;
        for (size_t i : ord) {
            SondeInfo s = sondes[i].info;
            s.lastHeardS = tNow - sondes[i].lastFixT;
            s.firstHeardS = tNow - sondes[i].firstT;
            s.active = s.lastHeardS < 30.0;
            s.freqHz = centerHz > 0 ? centerHz + s.offsetHz : sondes[i].announcedHz;
            if (rank < kTrackedSondes) { s.track = sondes[i].track; s.trackIncluded = true; }
            if (s.active && s.snrDb > bestSnr) {
                bestSnr = s.snrDb;
                bestCfo = (centerHz > 0 && sondes[i].announcedHz > 0) ? s.freqHz - sondes[i].announcedHz : 0;
            }
            rank++;
            t.sondes.push_back(std::move(s));
        }
        for (auto& c : carriers) {
            SondeCarrier sc;
            sc.offsetHz = c.offHz - sigOff; sc.freqHz = centerHz > 0 ? centerHz + sc.offsetHz : 0;
            sc.levelDb = c.level; sc.assigned = c.assigned; sc.channel = c.channel;
            t.carriers.push_back(sc);
        }
        if (bestSnr < -999) for (auto& c : carriers) bestSnr = std::max(bestSnr, c.level);
        const bool decoding = tNow - lastDecodeT < 5.0;
        t.state = decoding ? 2 : (!carriers.empty() ? 1 : 0);
        t.dataValid = okFrames > 0;
        t.snrDb = bestSnr < -999 ? 0.f : bestSnr;
        t.cfoHz = bestCfo;
        std::lock_guard<std::mutex> lk(mu);
        t.seq = tel.seq + 1;
        t.centerHz = centerHz;
        tel = std::move(t);
    }

    void process(const cf32* x, size_t n) {
        const double t0 = threadCpuSeconds();
        if (fs <= 0 || n == 0) return;
        const double tNow = (double)streamN / fs;
        feedSearch(x, n);
        fixes.clear();
        for (auto& c : chans) {
            if (!c.used) continue;
            c.process(x, n, tNow, fixes);
            for (auto& fx : fixes) handleFix(c, fx, (double)(streamN + n) / fs);
            fixes.clear();
        }
        streamN += n;
        const double tEnd = (double)streamN / fs;
        if (lastReportT < 0 || tEnd - lastReportT >= kReportPeriodS) { lastReportT = tEnd; report(tEnd); }
        cpu.store(cpu.load() + (threadCpuSeconds() - t0));
    }

    void workerLoop() {
        std::vector<cf32> buf(65536);
        while (run) {
            size_t got = 0;
            {
                std::lock_guard<std::mutex> pl(procMu);
                if (ring) got = ring->read(buf.data(), buf.size());
                if (got) { process(buf.data(), got); processed += got; }
            }
            if (!got) {
                std::unique_lock<std::mutex> lk(cvMu);
                cv.wait_for(lk, std::chrono::milliseconds(2));
            }
        }
    }
};

SondeReceiver::SondeReceiver() : p_(std::make_unique<Impl>()) {}
SondeReceiver::~SondeReceiver() = default;

void SondeReceiver::configure(double inputRateHz) { p_->configure(inputRateHz); }
void SondeReceiver::setSignalOffset(double hz) { p_->sigOff = hz; }
bool SondeReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->fs >= sondeTuning().minSampleRate - 1;
}
void SondeReceiver::reset() {
    std::lock_guard<std::mutex> pl(p_->procMu);
    if (p_->ring) { std::vector<cf32> junk(65536); while (p_->ring->read(junk.data(), junk.size())) {} }
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetState();
}
void SondeReceiver::setSynchronous(bool on) { p_->syncMode = on; }
void SondeReceiver::setCenterMhz(double mhz) { p_->centerHz = mhz * 1e6; }
void SondeReceiver::setMaxChannels(int n) { p_->maxChan = std::clamp(n, 1, kMaxChan); }
void SondeReceiver::clearSondes() {
    // done by the worker's lock: the list belongs to the processing state
    std::lock_guard<std::mutex> pl(p_->procMu);
    p_->sondes.clear();
}
uint64_t SondeReceiver::droppedSamples() const { return p_->dropped.load(); }
double SondeReceiver::cpuSeconds() const { return p_->cpu.load(); }

void SondeReceiver::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    if (s.fs <= 0 || n == 0) return;
    if (s.syncMode) {
        std::lock_guard<std::mutex> pl(s.procMu);
        s.process(x, n);
        return;
    }
    if (!s.run.load()) {
        std::lock_guard<std::mutex> pl(s.procMu);
        if (!s.run.load()) {
            if (!s.ring) {
                size_t cap = 1u << 18;
                while (cap < (size_t)(0.4 * s.fs) && cap < (1u << 23)) cap <<= 1;
                s.ring = std::make_unique<IqRing>(cap);
            }
            s.run = true;
            s.th = std::thread([&s] { s.workerLoop(); });
        }
    }
    const size_t w = s.ring->write(x, n);
    if (w < n) s.dropped += n - w;
    s.fed += w;
    s.cv.notify_one();
}

void SondeReceiver::flush() {
    Impl& s = *p_;
    if (s.syncMode || !s.run.load()) return;
    for (int i = 0; i < 20000; i++) {
        if (s.processed.load() >= s.fed.load()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool SondeReceiver::telemetry(SondeTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}
void SondeReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning sondeTuning() {
    ModeTuning t;
    t.stdMode = 15; t.id = "sonde"; t.name = "Radiosonde";
    t.minMhz = 380; t.maxMhz = 1700; t.defMhz = 403;
    t.sampleRate = 8000000;
    t.basebandHz = 7000000;
    t.bandwidthMhz = 6;
    t.minSampleRate = 2000000;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
