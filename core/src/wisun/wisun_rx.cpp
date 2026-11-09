// Wi-SUN FAN monitor (see wisun_rx.h).
#include "dect2/dsp/fast_phase.h"
#include "dect2/common/thread_name.h"
#include "dect2/wisun/wisun_rx.h"
#include "dect2/voicekeys/voice_keys.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/results/results_io.h"
#include "dect2/wisun/wisun_proto.h"
#include "dect2/wisun/wisun_tel.h"
#include "dect2/zigbee/zigbee_proto.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "dect2/common/bits.h"
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr int kTapsPerBranch = 16;        // polyphase prototype length: 16 M
constexpr size_t kQueueMax = 8000000;     // bin samples waiting for the decoder at most (64 MB); more bursts are dropped
constexpr int kLook = 96;                 // bin samples kept from before a burst's rise
constexpr int kHang = 160;                // quiet bin samples that end a burst
constexpr double kMaxBurstSec = 0.35;     // longer bursts are cut (a 2047-octet frame at 50 kbit/s takes 0.33 s)
constexpr float kFloorMin = 1e-9f;       // -90 dBFS: no radio is this quiet; a noiseless input (tests) does not make every sidelobe a burst
constexpr int kPhases = 8;                // sample phases the start-of-frame search tries
constexpr int kSearchBits = 512;          // how far into a burst the start of frame is looked for (bits)

using cf = std::complex<float>;

int popc32(uint32_t v) { v = v - ((v >> 1) & 0x55555555u); v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u); return (int)((((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24); }

// A windowed-sinc low-pass (Blackman), unity gain at 0 Hz; fc in cycles per sample
std::vector<float> lowpass(int n, double fc) {
    std::vector<float> h((size_t)n);
    double sum = 0;
    for (int i = 0; i < n; i++) {
        const double m = i - (n - 1) / 2.0;
        const double s = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
        const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (n - 1)) + 0.08 * std::cos(4 * kPi * i / (n - 1));
        h[(size_t)i] = (float)(s * w);
        sum += s * w;
    }
    for (float& v : h) v = (float)(v / sum);
    return h;
}

// A colour index for a node (the hop chart): its EUI-64 hashed
int colourOf(uint64_t e) { e ^= e >> 29; e *= 0xBF58476D1CE4E5B9ull; e ^= e >> 32; return (int)(e % 12); }
}

struct WisunReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0, offsetSet = 0;
    std::string regionSet;                // under mu
    std::atomic<bool> resetReq{true}, clearReq{false};
    std::atomic<double> centerHz{0}, dialHz{0};

    double rate = 0, offset = 0;
    double centreAbs = 0;                 // the radio's centre, Hz (the channels' reference)
    std::string region = "ANZ";
    int64_t nIn = 0;

    // ---- the polyphase filter bank: M bins of rate / M, each at 2 rate / M (a new output every D = M / 2 inputs)
    int M = 0, D = 0, N = 0, log2M = 0;
    double binHz = 0, outRate = 0;
    std::vector<float> proto;             // N taps, unity gain
    std::vector<float> xr, xi;            // input history (split), from xPos on
    size_t xPos = 0;
    std::vector<float> ur, ui;
    int64_t frame = 0;                    // output index (bin samples)

    struct Bin {
        bool used = false;
        double fHz = 0;                   // the bin's centre from the radio's centre
        bool active = false;
        int quiet = 0;
        int64_t start = 0;                // bin sample index of buf[0]
        int64_t zeroUpTo = -1;            // the look-back history counts as zero up to this bin sample (a burst's end clears it)
        std::vector<cf> buf;
        std::vector<int> chans;           // indices into chans
    };
    std::vector<Bin> bins;
    // The per-sample part of the burst detector runs over all bins as plain arrays (the compiler vectorises it); only a bin that is in a burst or
    // has just crossed the threshold goes through binSample(). The look-back history of the last kLook bin samples is one ring for all bins.
    std::vector<float> hPavg, hFloor, hSign, ringR, ringI;
    std::vector<uint8_t> hAct, hFlag;

    struct Chan {
        double mhz = 0;
        double offHz = 0;                 // from its bin's centre
        int bin = -1;
        int grid = 0;                     // index into the band plan's channel list (allMhz)
        std::vector<wisun::PhyRate> rates;   // the FSK modes to try, fastest first (the ones this receiver demodulates)
    };
    std::vector<Chan> chans;
    std::vector<double> allMhz;           // every channel centre of the band plan, every spacing, in order
    static constexpr int kMaxBitRate = 150000;   // 200 and 300 kbit/s (600 kHz channels) need more than a bin

    // results
    WisunTelemetry tel, pub;
    std::unordered_map<uint16_t, size_t> panIdx;
    std::unordered_map<uint64_t, size_t> nodeIdx;
    std::map<std::pair<uint64_t, uint64_t>, size_t> linkIdx;
    double power = 0; int64_t nPower = 0;
    double nextReport = 0, lastGood = -100;
    double snrAvg = 0;
    double nextPps = 1; int ppsAcc = 0;
    double busySec = 0, busyFrom = 0;     // processing time since the last report, and the signal time it covers
    // the last frames decoded (a burst seen by two bins, or tried on two channels, is kept once)
    struct Recent { double sec; uint32_t crc; };
    std::vector<Recent> recent;           // the decoder thread's

    // ---- the decoder thread: bursts in, frames out; the tables stay on the feed thread
    struct Job {
        std::vector<cf> buf;
        int64_t start = 0;
        std::vector<int> chans;
    };
    struct Out {
        std::vector<uint8_t> psdu;
        bool fcs2 = false;
        int chan = 0, br = 0;
        float rssi = 0, snr = 0;
        double sec = 0, cfo = 0;
    };
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<Job> jobs;
    size_t queued = 0;                    // bin samples in jobs
    bool busy = false, quit = false;
    std::atomic<bool> offline{false};
    std::vector<Out> outs;
    uint64_t wBad = 0, wFec = 0, wCorrected = 0, wDropped = 0;   // under qm
    std::vector<uint16_t> wModeSwitch;    // mode switch PHRs seen (under qm)
    std::thread worker;

    void startWorker() {
        quit = false;
        worker = dect2::namedThread("onair-wisun", [this] {
            for (;;) {
                Job j;
                {
                    std::unique_lock<std::mutex> lk(qm);
                    qcv.wait(lk, [this] { return quit || !jobs.empty(); });
                    if (quit) return;
                    j = std::move(jobs.front());
                    jobs.pop_front();
                    queued -= j.buf.size();
                    busy = true;
                }
                decode(j);
                std::lock_guard<std::mutex> lk(qm);
                busy = false;
                qcv.notify_all();
            }
        });
    }
    void stopWorker() {
        if (!worker.joinable()) return;
        { std::lock_guard<std::mutex> lk(qm); quit = true; jobs.clear(); queued = 0; }
        qcv.notify_all();
        worker.join();
    }
    ~Impl() { stopWorker(); }

    // feed thread: the decoded frames into the tables
    void takeFrames() {
        std::vector<Out> got;
        {
            std::lock_guard<std::mutex> lk(qm);
            got.swap(outs);
            tel.blocksBad += wBad; tel.fecFrames += wFec; tel.corrected += wCorrected; tel.dropped += wDropped;
            wBad = wFec = wCorrected = wDropped = 0;
            for (uint16_t v : wModeSwitch) {
                tel.modeSwitches++;
                // the PHY operating mode the frame switches to: bits 3 - 10 after the MS bit (as sent)
                const int mode = (v >> 5) & 0xFF;
                bool found = false;
                for (auto& m : tel.switchModes) if (m.first == mode) { m.second++; found = true; }
                if (!found && tel.switchModes.size() < 16) tel.switchModes.push_back({mode, 1});
            }
            wModeSwitch.clear();
        }
        for (const Out& o : got) {
            snrAvg = tel.blocksOk == 0 ? o.snr : 0.8 * snrAvg + 0.2 * o.snr;
            tel.cfoHz = o.cfo;
            accept(o.psdu, o.fcs2, chans[(size_t)o.chan], o.br, o.rssi, o.snr, o.sec);
        }
    }
    // Waits until the decoder thread has nothing left (tools and tests: never on the feed path)
    void waitIdle() {
        std::unique_lock<std::mutex> lk(qm);
        qcv.wait(lk, [this] { return jobs.empty() && !busy; });
    }

    double sigSec() const { return rate > 0 ? nIn / rate : 0; }

    // ---------------------------------------------------------------- set-up

    // regionAsked: the band plan's id as set (a copy taken under mu), "" by the frequency
    void setup(const std::string& regionAsked) {
        bins.clear(); chans.clear();
        xr.clear(); xi.clear(); xPos = 0; frame = 0;
        if (rate <= 0) return;
        // bins of about 300 kHz (250 - 400 kHz): every 200 kHz channel lies within 150 kHz of a bin's centre, its 150 kbit/s spectrum inside
        // the bin's pass band
        M = 4; log2M = 2;
        while (rate / (M * 2) >= 250e3 && M < 1024) { M *= 2; log2M++; }
        D = M / 2;
        N = kTapsPerBranch * M;
        binHz = rate / M;
        outRate = 2 * binHz;
        proto = lowpass(N, 1.0 / M);   // cut-off at one bin spacing: the pass band reaches the neighbouring bins' centres
        ur.assign((size_t)M, 0.f); ui.assign((size_t)M, 0.f);
        // the band plan (the one asked for, else the one for this frequency) and all its channel centres
        const double c = centreAbs;
        const wisun::BandPlan* bp = !regionAsked.empty() ? wisun::bandPlan(regionAsked) : nullptr;
        if (!bp) bp = &wisun::bandPlanFor(c / 1e6);
        region = bp->id;
        const auto plans = wisun::plansOf(region);
        allMhz.clear();
        for (const auto& p : plans)
            for (int k = 0; k < p.channels; k++) if (wisun::channelUsable(p, k)) allMhz.push_back(wisun::channelMhz(p, k));
        std::sort(allMhz.begin(), allMhz.end());
        allMhz.erase(std::unique(allMhz.begin(), allMhz.end(), [](double x, double y) { return std::fabs(x - y) < 1e-6; }), allMhz.end());
        bins.assign((size_t)M, Bin());
        hPavg.assign((size_t)M, 0.f); hFloor.assign((size_t)M, 0.f); hSign.assign((size_t)M, 1.f);
        for (int k = 1; k < M; k += 2) hSign[(size_t)k] = -1.f;
        ringR.assign((size_t)kLook * (size_t)M, 0.f); ringI.assign((size_t)kLook * (size_t)M, 0.f);
        hAct.assign((size_t)M, 0); hFlag.assign((size_t)M, 0);
        for (int k = 0; k < M; k++) {
            Bin& b = bins[(size_t)k];
            b.fHz = (k < M / 2 ? k : k - M) * binHz;
            b.used = std::fabs(b.fHz) <= 0.5 * rate - binHz * 0.75;
        }
        // the channels inside the coverage, each with its nearest bin
        const double edge = 0.45 * rate;
        double lo = 1e30, hi = 0;
        for (int g = 0; g < (int)allMhz.size(); g++) {
            const double mhz = allMhz[(size_t)g];
            const double off = mhz * 1e6 - c;
            if (c <= 0 || std::fabs(off) > edge) continue;
            int k = (int)std::lround(off / binHz);
            k = ((k % M) + M) % M;
            if (!bins[(size_t)k].used) continue;
            Chan ch;
            ch.mhz = mhz; ch.grid = g; ch.bin = k;
            ch.offHz = off - std::lround(off / binHz) * binHz;
            double half = 0.1;
            for (const auto& p : plans) {
                const int n = wisun::channelOf(p, mhz);
                if (n < 0 || std::fabs(wisun::channelMhz(p, n) - mhz) > 1e-6) continue;
                half = std::max(half, p.spacingKhz / 2000.0);
                for (const wisun::PhyRate& r : p.rates) {
                    if (r.bitRate <= 0 || r.bitRate > kMaxBitRate) continue;
                    bool have = false;
                    for (const auto& x : ch.rates) have |= x.bitRate == r.bitRate && x.modIndex == r.modIndex;
                    if (!have) ch.rates.push_back(r);
                }
            }
            std::sort(ch.rates.begin(), ch.rates.end(), [](const wisun::PhyRate& x, const wisun::PhyRate& y) { return x.bitRate > y.bitRate; });
            if (ch.rates.empty()) continue;
            bins[(size_t)k].chans.push_back((int)chans.size());
            chans.push_back(ch);
            lo = std::min(lo, mhz - half); hi = std::max(hi, mhz + half);
        }
        setupRates();
        tel.region = region;
        tel.bandName = bp->name;
        tel.bandLoMhz = bp->loMhz; tel.bandHiMhz = bp->hiMhz;
        tel.loMhz = chans.empty() ? 0 : lo; tel.hiMhz = chans.empty() ? 0 : hi;
        tel.bins = M; tel.binKhz = binHz / 1e3;
        tel.channelsCovered = (int)chans.size();
        if (tel.chanMhz != allMhz) { tel.chanMhz = allMhz; tel.perChannel.assign(allMhz.size(), 0); }
    }

    // The tables and the frame log stay through resets: Clear and a loaded file reach the feed thread's copy through these requests (mu), and the
    // published report at once
    bool resClear = false, resLoad = false;
    WisunTelemetry loaded;
    double loadedAt = 0;
    static void moveLists(WisunTelemetry& to, WisunTelemetry& from) {
        to.pans = std::move(from.pans); to.nodes = std::move(from.nodes); to.links = std::move(from.links); to.log = std::move(from.log);
        to.hops = std::move(from.hops);
        from.pans.clear(); from.nodes.clear(); from.links.clear(); from.log.clear(); from.hops.clear();
    }
    static void shiftTimes(WisunTelemetry& t, double dt) {
        for (auto& p : t.pans) { p.firstSec += dt; p.lastSec += dt; }
        for (auto& d : t.nodes) { d.firstSec += dt; d.lastSec += dt; }
        for (auto& l : t.links) l.lastSec += dt;
        for (auto& f : t.log) f.sec += dt;
        for (auto& h : t.hops) h.sec += dt;
    }
    void takeRequests() {   // feed thread
        std::lock_guard<std::mutex> lk(mu);
        if (!resClear && !resLoad) return;
        if (resClear) { WisunTelemetry e; moveLists(tel, e); resClear = false; }
        if (resLoad) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); resLoad = false; }
        reindex();
    }
    void resetState() {
        std::string regionAsked;
        {
            std::lock_guard<std::mutex> lk(mu);
            rate = rateSet; offset = offsetSet;
            regionAsked = regionSet;   // setRegion() writes it from the interface thread
            const double c = centerHz.load(), d = dialHz.load();
            centreAbs = c > 0 ? c : (d > 0 ? d - offset : wisunTuning().defMhz * 1e6);
        }
        stopWorker();
        { std::lock_guard<std::mutex> lk(qm); outs.clear(); wBad = wFec = wCorrected = wDropped = 0; }
        recent.clear();
        setup(regionAsked);
        if (rate > 0) startWorker();
        nIn = 0;
        power = 0; nPower = 0; nextReport = 0.25 * rate; lastGood = -100;
        tel.inputRate = rate;
        shiftTimes(tel, -tel.timeSec);    // the lists stay (clearResults): the clock starts again
        tel.timeSec = 0;
        nextPps = rate > 0 ? rate : 1; ppsAcc = 0;
        tel.ppsHist.clear(); tel.pps = 0;
        busySec = 0; busyFrom = 0;
    }

    void clearAll() {
        const uint64_t seq = tel.seq;
        WisunTelemetry fresh;
        fresh.seq = seq; fresh.inputRate = rate;
        fresh.region = tel.region; fresh.loMhz = tel.loMhz; fresh.hiMhz = tel.hiMhz; fresh.bins = tel.bins; fresh.binKhz = tel.binKhz;
        fresh.channelsCovered = tel.channelsCovered; fresh.chanMhz = tel.chanMhz; fresh.bandName = tel.bandName;
        fresh.bandLoMhz = tel.bandLoMhz; fresh.bandHiMhz = tel.bandHiMhz;
        fresh.perChannel.assign(tel.perChannel.size(), 0);
        tel = std::move(fresh);
        panIdx.clear(); nodeIdx.clear(); linkIdx.clear();
        snrAvg = 0;
        nextPps = rate > 0 ? (double)nIn + rate : 1; ppsAcc = 0;
    }

    // ---------------------------------------------------------------- channelizer

    void input(const cf32* x, size_t n) {
        const size_t have = xr.size();
        xr.resize(have + n); xi.resize(have + n);
        for (size_t i = 0; i < n; i++) {
            float re = x[i].real(), im = x[i].imag();
            if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
            power += (double)re * re + (double)im * im;
            xr[have + i] = re; xi[have + i] = im;
        }
        while (xPos + (size_t)N <= xr.size()) {
            pfbFrame(xr.data() + xPos, xi.data() + xPos);
            xPos += (size_t)D;
        }
        if (xPos > 65536) {
            xr.erase(xr.begin(), xr.begin() + (ptrdiff_t)xPos);
            xi.erase(xi.begin(), xi.begin() + (ptrdiff_t)xPos);
            xPos = 0;
        }
    }

    void pfbFrame(const float* ar, const float* ai) {
        float* u = ur.data();
        float* v = ui.data();
        const float* h = proto.data();
        for (int r = 0; r < M; r++) { u[r] = h[r] * ar[r]; v[r] = h[r] * ai[r]; }
        for (int q = 1; q < kTapsPerBranch; q++) {
            const float* hh = h + q * M;
            const float* a = ar + q * M;
            const float* b = ai + q * M;
            for (int r = 0; r < M; r++) { u[r] += hh[r] * a[r]; v[r] += hh[r] * b[r]; }
        }
        fftSplit(u, v, log2M, false);
        const bool odd = frame & 1;   // the hop of M / 2 inputs turns odd bins by pi every output
        const size_t slot = (size_t)(frame % kLook) * (size_t)M;
        float* rr = ringR.data() + slot;
        float* ri = ringI.data() + slot;
        const float* sg = hSign.data();
        float* pa = hPavg.data();
        float* fl = hFloor.data();
        uint8_t* act = hAct.data();
        uint8_t* flag = hFlag.data();
        const bool primedNow = frame > 64, learn = frame > 65;
        for (int k = 0; k < M; k++) {
            const float sgn = odd ? sg[k] : 1.f;
            const float yr = u[k] * sgn, yi = v[k] * sgn;
            rr[k] = yr; ri[k] = yi;
            const float p = yr * yr + yi * yi;
            pa[k] += (p - pa[k]) * (1.f / 16);
            const float f0 = learn ? fl[k] + (pa[k] - fl[k]) * (pa[k] < fl[k] ? (1.f / 512) : (1.f / 65536)) : pa[k];
            fl[k] = act[k] ? fl[k] : f0;
            flag[k] = (uint8_t)(act[k] | (primedNow && pa[k] > std::max(fl[k], kFloorMin) * 5.f));
        }
        for (int k = 0; k < M; k++) {
            if (!flag[k]) continue;
            Bin& b = bins[(size_t)k];
            if (b.used) binSample(b, k, cf(rr[k], ri[k]));
        }
        frame++;
    }

    void binSample(Bin& b, int k, cf y) {
        if (!b.active) {   // the threshold was crossed on this sample: the burst starts kLook samples back
            b.active = true; b.quiet = 0; hAct[(size_t)k] = 1;
            b.buf.clear();
            for (int i = 0; i < kLook; i++) {
                const int64_t idx = frame - (kLook - 1) + i;
                if (idx < 0 || idx <= b.zeroUpTo) { b.buf.push_back(cf(0, 0)); continue; }
                const size_t at = (size_t)(idx % kLook) * (size_t)M + (size_t)k;
                b.buf.push_back(cf(ringR[at], ringI[at]));
            }
            b.start = frame - kLook + 1;
            return;
        }
        b.buf.push_back(y);
        if (hPavg[(size_t)k] < std::max(hFloor[(size_t)k], kFloorMin) * 2.5f) b.quiet++; else b.quiet = 0;
        if (b.quiet >= kHang || (double)b.buf.size() > kMaxBurstSec * outRate) {
            b.active = false; hAct[(size_t)k] = 0;
            tel.bursts++;
            {
                std::unique_lock<std::mutex> lk(qm);
                if (offline) qcv.wait(lk, [&] { return queued + b.buf.size() <= kQueueMax || jobs.empty(); });
                if (queued + b.buf.size() > kQueueMax && !jobs.empty()) wDropped++;
                else {
                    Job j;
                    j.buf.swap(b.buf);
                    j.start = b.start; j.chans = b.chans;
                    queued += j.buf.size();
                    jobs.push_back(std::move(j));
                    qcv.notify_one();
                }
            }
            b.buf.clear();
            b.zeroUpTo = frame;
        }
    }

    // ---------------------------------------------------------------- demodulator

    // One bit rate's chain: the burst mixed to 0 Hz, low-pass filtered (the signal's own width) and decimated to about 5 samples per bit or
    // more, then the phase change per sample
    struct Rate {
        int br = 0;
        double h = 0.5;                   // modulation index (50 kbit/s: 1)
        int decim = 1;
        std::vector<float> lp;
    };
    std::vector<Rate> rateDefs;

    void setupRates() {
        // one chain for each FSK mode the band plan's channels carry
        rateDefs.clear();
        for (const Chan& ch : chans)
            for (const wisun::PhyRate& pr : ch.rates) {
                bool have = false;
                for (const Rate& x : rateDefs) have |= x.br == pr.bitRate && x.h == pr.modIndex;
                if (have) continue;
                Rate r;
                r.br = pr.bitRate; r.h = pr.modIndex;
                const int br = r.br;
                r.decim = std::max(1, (int)std::floor(outRate / (br * 5.0)));
                const double cut = r.h * br / 2 + 0.35 * br;   // the deviation and most of the modulation's own width
                const int n = std::max(31, (int)(5.5 * outRate / (0.6 * cut))) | 1;
                r.lp = lowpass(n, cut / outRate);
                rateDefs.push_back(std::move(r));
            }
    }

    struct Demod {
        const Rate* r = nullptr;
        std::vector<cf> mixed;            // at the bin rate
        std::vector<float> f;             // phase change per output sample (bin rate / decim)
        cf zPrev{0, 0};
        bool havePrev = false;
        std::complex<double> lo{1, 0}, step{1, 0};
    };

    // Runs the chain over buf until there are `toOut` output samples (or buf ends)
    static void extend(Demod& d, const std::vector<cf>& buf, size_t toOut) {
        const std::vector<float>& lp = d.r->lp;
        const size_t L = lp.size();
        const size_t D = (size_t)d.r->decim;
        while (d.f.size() < toOut) {
            const size_t i = d.f.size() * D;   // this output: the filter ending at input i
            if (i >= buf.size()) break;
            while (d.mixed.size() <= i) {
                d.mixed.push_back(cf(std::complex<double>(buf[d.mixed.size()]) * d.lo));
                d.lo *= d.step;
                if ((d.mixed.size() & 1023) == 0) d.lo /= std::abs(d.lo);
            }
            float re = 0, im = 0;
            const size_t m = std::min(L, i + 1);
            const cf* x = d.mixed.data() + i;
            for (size_t k = 0; k < m; k++) { re += lp[k] * x[-(ptrdiff_t)k].real(); im += lp[k] * x[-(ptrdiff_t)k].imag(); }
            const cf z(re, im);
            if (d.havePrev) {
                const cf w = z * std::conj(d.zPrev);
                d.f.push_back(fastAtan2(w.imag(), w.real()));
            } else d.f.push_back(0.f);
            d.zPrev = z; d.havePrev = true;
        }
    }

    static float at(const std::vector<float>& g, double t) {
        if (t < 0) return g.empty() ? 0.f : g[0];
        const size_t i = (size_t)t;
        if (i + 1 >= g.size()) return g.empty() ? 0.f : g.back();
        const float fr = (float)(t - (double)i);
        return g[i] * (1 - fr) + g[i + 1] * fr;
    }

    // g: f smoothed over L samples (trailing), from index `from` on
    static void smooth(const std::vector<float>& f, std::vector<float>& g, int L, size_t from) {
        g.resize(f.size());
        for (size_t i = from; i < f.size(); i++) {
            double s = 0;
            const size_t a = i + 1 >= (size_t)L ? i + 1 - (size_t)L : 0;
            for (size_t j = a; j <= i; j++) s += f[j];
            g[i] = (float)(s / L);
        }
    }

    void decode(const Job& b) {
        for (int ci : b.chans) {
            const Chan& ch = chans[(size_t)ci];
            for (const wisun::PhyRate& pr : ch.rates) {
                // too short for the shortest frame at this rate: preamble, delimiter, PHR, a few octets
                if ((double)b.buf.size() < 120.0 * outRate / pr.bitRate) continue;
                const Rate* r = nullptr;
                for (const Rate& x : rateDefs) if (x.br == pr.bitRate && x.h == pr.modIndex) r = &x;
                if (!r) continue;
                Demod d;
                d.r = r;
                d.step = std::polar(1.0, -2 * kPi * ch.offHz / outRate);
                if (tryRate(d, b, ch)) return;
            }
        }
    }

    bool tryRate(Demod& d, const Job& b, const Chan& ch) {
        const int br = d.r->br;
        const double sps = outRate / d.r->decim / br;
        extend(d, b.buf, (size_t)((kSearchBits + 40) * sps) + d.r->lp.size() / (size_t)d.r->decim + 1);
        const int L = std::max(1, (int)std::floor(sps));
        std::vector<float> g;
        smooth(d.f, g, L, 0);
        // the start-of-frame delimiter at each sample phase; the slicing level: the mean over the 16 bits before
        int bestPh = -1; double bestT = 0, bestScore = 0; bool coded = false;
        const double win = 16 * sps;
        std::vector<double> cum(g.size() + 1, 0.0);
        for (size_t i = 0; i < g.size(); i++) cum[i + 1] = cum[i] + g[i];
        auto meanBefore = [&](double t) {
            const size_t e = std::min(g.size(), (size_t)std::max(0.0, t));
            const size_t s = (size_t)std::max(0.0, t - win);
            return e > s ? (cum[e] - cum[s]) / (double)(e - s) : 0.0;
        };
        for (int ph = 0; ph < kPhases; ph++) {
            uint64_t reg = 0;
            int nb = 0;
            for (double t = (L - 1) / 2.0 + ph * sps / kPhases; t + 1 < (double)g.size() && nb < kSearchBits; t += sps, nb++) {
                const double m = meanBefore(t);
                reg = reg << 1 | (g[(size_t)t] > m ? 1u : 0u);
                if (nb < 40) continue;
                // 24 bits of the 0101 preamble before the delimiter: random bits (other systems' bursts) rarely pass both
                const bool pre = popc32((uint32_t)((reg >> 16) & 0xFFFFFF) ^ 0x555555u) <= 1;
                if (!pre) continue;
                const bool u = popc32((uint32_t)(reg & 0xFFFF) ^ wisun::kSfdUncoded) <= 1;
                const bool c = popc32((uint32_t)(reg & 0xFFFF) ^ wisun::kSfdCoded) <= 1;
                if (!u && !c) continue;
                // how well the 32 bits up to here fit: the soft distance from the slicing level
                double score = 0;
                for (int k = 0; k < 32; k++) score += std::fabs(at(g, t - k * sps) - m);
                if (bestPh < 0 || t < bestT - sps / 2 || (std::fabs(t - bestT) <= sps / 2 && score > bestScore)) {
                    bestPh = ph; bestT = t; bestScore = score; coded = c && !u;
                }
                break;
            }
        }
        if (bestPh < 0) return false;
        if (coded) { std::lock_guard<std::mutex> lk(qm); wFec++; return true; }
        return frameAt(d, b, ch, g, sps, L, bestT);
    }

    // Up to kChase of the least reliable bits flipped: the combination whose CRC syndrome cancels the frame's (the CRC is linear, so the
    // syndrome of a set of flips is the sum of each flip's). With 2^14 candidates against a 32-bit CRC a wrong frame passes once in 260000.
    static constexpr int kChase = 14;
    static uint32_t check32(const std::vector<uint8_t>& p) {
        const size_t n = p.size();
        return wisun::crc32(p.data(), n - 4) ^ ((uint32_t)p[n - 4] | (uint32_t)p[n - 3] << 8 | (uint32_t)p[n - 2] << 16 | (uint32_t)p[n - 1] << 24);
    }
    static bool chase(std::vector<uint8_t>& p, const std::vector<float>& rel) {
        if (p.size() < 5) return false;
        const uint32_t s0 = check32(p);
        std::vector<int> idx(rel.size());
        for (size_t i = 0; i < idx.size(); i++) idx[i] = (int)i;
        const int k = std::min<int>(kChase, (int)idx.size());
        std::partial_sort(idx.begin(), idx.begin() + k, idx.end(), [&](int a, int b) { return rel[(size_t)a] < rel[(size_t)b]; });
        uint32_t e[kChase];
        for (int j = 0; j < k; j++) {
            const int q = idx[(size_t)j];
            p[(size_t)(q >> 3)] ^= (uint8_t)(1u << (q & 7));
            e[j] = check32(p) ^ s0;
            p[(size_t)(q >> 3)] ^= (uint8_t)(1u << (q & 7));
        }
        uint32_t acc = s0, mask = 0;
        for (uint32_t gc = 1; gc < (1u << k); gc++) {
            const int j = __builtin_ctz(gc);
            acc ^= e[j]; mask ^= 1u << j;
            if (acc == 0) {
                for (int i = 0; i < k; i++) if (mask >> i & 1) { const int q = idx[(size_t)i]; p[(size_t)(q >> 3)] ^= (uint8_t)(1u << (q & 7)); }
                return true;
            }
        }
        return false;
    }

    bool frameAt(Demod& d, const Job& b, const Chan& ch, std::vector<float>& g, double sps, int L, double tSfd) {
        const int br = d.r->br;
        const int D = d.r->decim;
        // the carrier offset and the deviation from the 16 preamble bits and the delimiter
        double off = 0;
        for (int k = 16; k < 32; k++) off += at(g, tSfd - k * sps);
        off /= 16;
        double dev = 0;
        for (int k = 0; k < 32; k++) dev += std::fabs(at(g, tSfd - k * sps) - off);
        dev /= 32;
        if (dev <= 1e-6) return false;
        auto more = [&](size_t bits, double t) {   // the chain and the smoothing through t + bits
            const size_t to = (size_t)(t + (bits + 4) * sps) + d.r->lp.size() / (size_t)D + 1;
            if (to <= g.size()) return;
            const size_t old = d.f.size();
            extend(d, b.buf, to);
            smooth(d.f, g, L, old);
        };
        double t = tSfd;
        int prev = wisun::kSfdUncoded & 1;   // the delimiter's last bit
        double err2 = 0; int nerr = 0;
        float lastRel = 0;
        auto readBit = [&]() {
            t += sps;
            const float s = (float)(at(g, t) - off);
            const int bit = s > 0;
            if (bit != prev) {   // a transition: the sample half a bit earlier should sit on the slicing level
                const double mid = (at(g, t - sps / 2) - off) / dev;
                const double e = std::clamp(mid, -1.0, 1.0) * (bit ? 1 : -1);
                t -= 0.12 * e * sps / 2;
            }
            const double r = std::fabs(s) - dev;
            err2 += r * r; nerr++;
            off += 0.01 * (s - (bit ? dev : -dev));
            dev += 0.01 * (std::fabs(s) - dev);
            prev = bit;
            lastRel = (float)(std::fabs(s) / dev);
            return bit;
        };
        more(16, t);
        uint16_t phrBits = 0;
        for (int k = 0; k < 16; k++) phrBits = (uint16_t)(phrBits << 1 | readBit());
        const wisun::Phr phr = wisun::parsePhr(phrBits);
        const size_t fcsLen = phr.fcs2 ? 2 : 4;
        // A mode switch (the next frame comes in another PHY: FAN 1.1 OFDM or a faster FSK) or a PHR whose first bit was received wrong: read
        // as an ordinary frame first, counted as a mode switch only when that fails
        auto modeSwitch = [&]() { std::lock_guard<std::mutex> lk(qm); wModeSwitch.push_back(phrBits); return true; };
        if (phr.length < (int)fcsLen + 3) return phr.modeSwitch ? modeSwitch() : false;
        more((size_t)phr.length * 8, t);
        if ((size_t)(t + (phr.length * 8 + 1) * sps) >= g.size() + 1) return phr.modeSwitch ? modeSwitch() : false;   // the burst ends before the frame
        std::vector<uint8_t> psdu((size_t)phr.length, 0);
        std::vector<float> rel((size_t)phr.length * 8);
        for (int k = 0; k < phr.length * 8; k++) { psdu[(size_t)(k >> 3)] |= (uint8_t)(readBit() << (k & 7)); rel[(size_t)k] = lastRel; }
        if (phr.whitened) wisun::whiten(psdu.data(), psdu.size());
        if (!wisun::fcsOk(psdu.data(), psdu.size(), phr.fcs2)) {
            if (phr.fcs2 || !chase(psdu, rel)) {
                if (phr.modeSwitch) return modeSwitch();
                std::lock_guard<std::mutex> lk(qm);
                wBad++;
                return false;
            }
            std::lock_guard<std::mutex> lk(qm);
            wCorrected++;
        }
        // the frame is good: level over the frame, SNR from the slicer (positions in bin samples)
        const double i0 = (tSfd - 48 * sps) * D, i1 = t * D;
        double pw = 0; int np = 0;
        for (size_t i = (size_t)std::max(0.0, i0); i < (size_t)i1 && i < b.buf.size(); i++) { pw += std::norm(b.buf[i]); np++; }
        const float rssi = (float)(10 * std::log10(pw / std::max(1, np) + 1e-20));
        const double snr = nerr && err2 > 0 ? 10 * std::log10(dev * dev / (err2 / nerr)) : 40;
        const double cfo = off * outRate / D / (2 * kPi);
        const double sec = (double)(b.start + (int64_t)i0) / outRate;
        const uint32_t crc = wisun::crc32(psdu.data(), psdu.size());
        for (const Recent& r : recent) if (r.crc == crc && std::fabs(r.sec - sec) < 0.003) return true;   // already decoded
        recent.push_back({sec, crc});
        if (recent.size() > 64) recent.erase(recent.begin());
        Out o;
        o.psdu = std::move(psdu); o.fcs2 = phr.fcs2; o.chan = (int)(&ch - chans.data()); o.br = br;
        o.rssi = rssi; o.snr = (float)snr; o.sec = sec; o.cfo = cfo;
        std::lock_guard<std::mutex> lk(qm);
        outs.push_back(std::move(o));
        return true;
    }

    // ---------------------------------------------------------------- tables

    void reindex() {
        panIdx.clear(); nodeIdx.clear(); linkIdx.clear();
        for (size_t i = 0; i < tel.pans.size(); i++) panIdx[tel.pans[i].pan] = i;
        for (size_t i = 0; i < tel.nodes.size(); i++) nodeIdx[tel.nodes[i].euiNum] = i;
        for (size_t i = 0; i < tel.links.size(); i++) linkIdx[{tel.links[i].srcNum, tel.links[i].dstNum}] = i;
    }

    WsPan& panEntry(uint16_t pan) {
        auto it = panIdx.find(pan);
        if (it != panIdx.end()) return tel.pans[it->second];
        if (tel.pans.size() >= kWsPanMax) {
            size_t old = 0;
            for (size_t i = 1; i < tel.pans.size(); i++) if (tel.pans[i].lastSec < tel.pans[old].lastSec) old = i;
            tel.pans.erase(tel.pans.begin() + (ptrdiff_t)old);
            reindex();
        }
        WsPan p;
        p.pan = pan;
        p.firstSec = sigSec();
        tel.pans.push_back(std::move(p));
        panIdx[pan] = tel.pans.size() - 1;
        if (log && tel.pans.size() <= 20) { char b[80]; snprintf(b, sizeof b, "Wi-SUN: new PAN 0x%04X", pan); log(b); }
        return tel.pans.back();
    }

    WsNode& node(uint64_t eui) {
        auto it = nodeIdx.find(eui);
        if (it != nodeIdx.end()) return tel.nodes[it->second];
        if (tel.nodes.size() >= kWsNodeMax) {
            size_t old = 0;
            for (size_t i = 1; i < tel.nodes.size(); i++) if (tel.nodes[i].lastSec < tel.nodes[old].lastSec) old = i;
            tel.nodes.erase(tel.nodes.begin() + (ptrdiff_t)old);
            reindex();
        }
        WsNode n;
        n.euiNum = eui; n.eui = wisun::euiText(eui); n.vendor = zigbee::vendorOfExt(eui);
        n.firstSec = sigSec();
        n.channels.assign(allMhz.size(), 0);
        tel.nodes.push_back(std::move(n));
        nodeIdx[eui] = tel.nodes.size() - 1;
        return tel.nodes.back();
    }

    WsLink& link(uint64_t a, uint64_t b) {
        auto it = linkIdx.find({a, b});
        if (it != linkIdx.end()) return tel.links[it->second];
        if (tel.links.size() >= kWsLinkMax) {
            size_t old = 0;
            for (size_t i = 1; i < tel.links.size(); i++) if (tel.links[i].lastSec < tel.links[old].lastSec) old = i;
            tel.links.erase(tel.links.begin() + (ptrdiff_t)old);
            reindex();
        }
        WsLink l;
        l.srcNum = a; l.dstNum = b; l.src = wisun::euiText(a); l.dst = wisun::euiText(b);
        tel.links.push_back(std::move(l));
        linkIdx[{a, b}] = tel.links.size() - 1;
        return tel.links.back();
    }

    static void setRole(WsNode& n, const char* role) {
        static const char* const order[] = {"node", "LFN", "joining", "router", "border router"};
        auto rank = [](const std::string& r) { for (int i = 0; i < 5; i++) if (r == order[i]) return i; return -1; };
        if (rank(role) > rank(n.role)) n.role = role;
    }

    static std::string modesText(const std::vector<int>& m) {
        std::string s;
        char b[8];
        for (int v : m) { snprintf(b, sizeof b, "%s%d", s.empty() ? "" : " ", v); s += b; }
        return s;
    }

    void accept(const std::vector<uint8_t>& psdu, bool fcs2, const Chan& ch, int br, float rssi, float snr, double sec) {
        using namespace wisun;
        const Frame f = parse(psdu.data(), psdu.size() - (fcs2 ? 2 : 4));
        tel.blocksOk++;
        lastGood = sigSec();
        ppsAcc++;
        if (f.utt >= 0 && f.utt < kWsUttTypes) tel.perUtt[f.utt]++;
        else tel.plain++;
        if (f.encrypted) tel.encryptedFrames++;
        std::string decryptedNote;
        if (f.encrypted) {
            voicekeys::CallKeyInfo ki; ki.protocol = voicekeys::Protocol::WiSun;
            for (const auto& k : voicekeys::KeyStore::instance().candidates(ki)) {
                std::vector<uint8_t> pl;
                if (k.key.size() == 16 && decryptFrame(psdu.data(), psdu.size() - (fcs2 ? 2 : 4), f, k.key.data(), pl)) { char b[40]; snprintf(b, sizeof b, " [decrypted, %zu bytes]", pl.size()); decryptedNote = b; break; }
            }
        }
        if (ch.grid >= 0 && (size_t)ch.grid < tel.perChannel.size()) tel.perChannel[(size_t)ch.grid]++;
        const int64_t wall = (int64_t)time(nullptr);
        // the channel number in the plan of the frame's bit rate
        int chNum = -1, opClass = 0;
        for (const auto& p : plansOf(region)) {
            if (p.rates[0].bitRate != br && p.rates[1].bitRate != br) continue;
            const int c = channelOf(p, ch.mhz);
            if (c >= 0 && std::fabs(channelMhz(p, c) - ch.mhz) < 1e-6) { chNum = c; opClass = p.opClass; break; }
        }
        int pan = -1;
        if (f.hasSrcPan && f.srcPan != 0xFFFF) pan = f.srcPan;
        else if (f.hasDestPan && f.destPan != 0xFFFF) pan = f.destPan;
        // the log
        WsFrame lf;
        lf.wallTime = wall; lf.sec = sec; lf.mhz = ch.mhz; lf.channel = chNum; lf.opClass = opClass; lf.bitRate = br;
        lf.utt = f.utt; lf.type = f.utt >= 0 ? uttName(f.utt) : frameTypeName(f.type);
        lf.src = srcText(f); lf.dst = dstText(f); lf.pan = pan; lf.len = (int)psdu.size();
        lf.rssi = rssi; lf.snrDb = snr; lf.encrypted = f.encrypted && decryptedNote.empty(); lf.ies = f.ies; lf.info = f.info + decryptedNote;
        static const char kHex[] = "0123456789abcdef";
        for (size_t i = 0; i + (fcs2 ? 2 : 4) < psdu.size(); i++) { lf.hex += kHex[psdu[i] >> 4]; lf.hex += kHex[psdu[i] & 15]; }
        tel.log.insert(tel.log.begin(), std::move(lf));
        if (tel.log.size() > kWsLogMax) tel.log.pop_back();
        WsHop hp;
        hp.sec = sec; hp.mhz = (float)ch.mhz; hp.utt = f.utt; hp.node = f.srcMode == 3 ? colourOf(f.srcExt) : -1;
        tel.hops.push_back(hp);
        if (tel.hops.size() > kWsHopMax) tel.hops.erase(tel.hops.begin(), tel.hops.begin() + (ptrdiff_t)(tel.hops.size() - kWsHopMax));
        if (!f.ok) return;
        // the network
        if (pan >= 0) {
            WsPan& p = panEntry((uint16_t)pan);
            p.packets++; p.rssi = rssi; p.lastSec = sigSec(); p.lastWall = wall;
            if (f.hasNetName) p.netName = f.netName;
            if (f.panVersion >= 0) p.panVersion = f.panVersion;
            if (f.hasPanIe) {
                p.panSize = f.panSize; p.tpsVersion = f.tpsVersion;
                if (f.routingCost == 0 && f.panSize > 0 && f.srcMode == 3) p.borderRouter = euiText(f.srcExt);
            }
            if (f.us.present && (f.utt == PA || p.unicast.empty())) p.unicast = scheduleText(f.us);
            if (f.bs.present) p.broadcast = scheduleText(f.bs);
            if (!f.phyModes.empty()) p.phyModes = modesText(f.phyModes);
        }
        // the sender
        if (f.srcMode == 3) {
            WsNode& n = node(f.srcExt);
            n.packets++; n.rssi = rssi; n.snrDb = snr; n.lastSec = sigSec(); n.lastWall = wall; n.last = f.info;
            n.lastMhz = ch.mhz; n.bitRate = br;
            if (ch.grid >= 0 && (size_t)ch.grid < n.channels.size()) n.channels[(size_t)ch.grid] = 1;
            if (f.utt >= 0 && f.utt < kWsUttTypes) n.perUtt[f.utt]++;
            if (f.encrypted) { n.encrypted++; n.frameCounter = f.frameCounter; }
            if (pan >= 0) n.pan = pan;
            if (f.us.present) n.unicast = scheduleText(f.us);
            if (f.bs.present) n.broadcast = scheduleText(f.bs);
            if (!f.phyModes.empty()) n.phyModes = modesText(f.phyModes);
            if (f.hasPanIe) { n.routingCost = f.routingCost; n.tpsVersion = f.tpsVersion; }
            switch (f.utt) {
            // routing cost 0 marks the border router, but some networks advertise 0 (and PAN size 0) from every router: only with a PAN size
            case PA: setRole(n, f.hasPanIe && f.routingCost == 0 && f.panSize > 0 ? "border router" : "router"); break;
            case PC: setRole(n, "router"); break;
            case PAS: case PCS: setRole(n, "joining"); break;
            case LPAS: case LPCS: setRole(n, "LFN"); break;
            case LPA: case LPC: setRole(n, "router"); break;
            default: setRole(n, "node"); break;
            }
            if (f.utt == Eapol) {
                if (f.eaEui) n.authenticator = euiText(f.eaEui);
                if (n.role == "node") setRole(n, "joining");
                const size_t c = f.info.find(", EAP");
                n.eapState = c != std::string::npos ? f.info.substr(c + 2, f.info.find(',', c + 2) - (c + 2)) : "EAPOL";
            }
        }
        // links: unicast frames from the sender, acks back to it with the level the acker measured (RSL-IE)
        if (f.srcMode == 3 && f.destMode == 3) {
            const bool ack = f.utt == AckFrame || f.type == Ack;
            WsLink& l = ack ? link(f.destExt, f.srcExt) : link(f.srcExt, f.destExt);
            if (ack) { l.acks++; if (f.rslDbm > -999) l.rslDbm = f.rslDbm; }
            else { l.frames++; l.rssi = rssi; }
            l.lastSec = sigSec(); l.lastWall = wall;
            WsNode& peer = node(f.destExt);
            if (pan >= 0 && peer.pan < 0) peer.pan = pan;
            if (peer.role.empty()) peer.role = "node";
        }
    }

    void report() {
        takeFrames();
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.snrDb = (float)snrAvg;
        tel.state = sigSec() - lastGood < 10 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        if (sigSec() > busyFrom) tel.loadPct = (float)(100 * busySec / (sigSec() - busyFrom));
        busySec = 0; busyFrom = sigSec();
        while ((double)nIn >= nextPps && rate > 0) {   // one value a second
            tel.pps = (float)ppsAcc;
            tel.ppsHist.push_back((float)ppsAcc);
            if (tel.ppsHist.size() > kWsPpsHist) tel.ppsHist.erase(tel.ppsHist.begin());
            ppsAcc = 0; nextPps += rate;
        }
        WisunTelemetry out = tel;
        std::stable_sort(out.pans.begin(), out.pans.end(), [](const WsPan& x, const WsPan& y) { return x.lastSec > y.lastSec; });
        std::stable_sort(out.nodes.begin(), out.nodes.end(), [](const WsNode& x, const WsNode& y) { return x.lastSec > y.lastSec; });
        std::stable_sort(out.links.begin(), out.links.end(), [](const WsLink& x, const WsLink& y) { return x.lastSec > y.lastSec; });
        for (WsPan& p : out.pans) {
            int n = 0;
            for (const WsNode& d : out.nodes) if (d.pan == p.pan) n++;
            p.nodes = n;
        }
        std::lock_guard<std::mutex> lk(mu);
        pub = std::move(out);
    }
};

WisunReceiver::WisunReceiver() : p_(std::make_unique<Impl>()) {}
WisunReceiver::~WisunReceiver() = default;

void WisunReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void WisunReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void WisunReceiver::setCenterHz(double hz) { if (hz != p_->centerHz.exchange(hz)) p_->resetReq = true; }
void WisunReceiver::setDialHz(double hz) { if (hz != p_->dialHz.exchange(hz)) p_->resetReq = true; }
void WisunReceiver::setRegion(const std::string& r) { std::lock_guard<std::mutex> lk(p_->mu); if (r != p_->regionSet) { p_->regionSet = r; p_->resetReq = true; } }
bool WisunReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= wisunTuning().minSampleRate - 1; }
void WisunReceiver::reset() { p_->resetReq = true; }
void WisunReceiver::clearResults() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);   // the report at once; the feed thread's lists (and counters) on its next block
        WisunTelemetry e;
        Impl::moveLists(p_->pub, e);
        p_->resLoad = false;
        p_->pub.seq++;
    }
    p_->clearReq = true;
}

void WisunReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.clearAll();
    m.takeRequests();
    m.takeFrames();
    if (m.rate < wisunTuning().minSampleRate - 1 || n == 0) {
        if (m.rate > 0) { m.nIn += (int64_t)n; if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); } }
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    size_t i = 0;
    while (i < n) {
        // up to the next report
        const size_t k = std::min(n - i, (size_t)std::max(1.0, m.nextReport - (double)m.nIn));
        m.input(x + i, k);
        m.nIn += (int64_t)k; m.nPower += (int64_t)k;
        i += k;
        if ((double)m.nIn >= m.nextReport) {
            m.busySec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            m.nextReport += 0.25 * m.rate;
            m.report();
        }
    }
    m.busySec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void WisunReceiver::setOffline(bool on) { p_->offline = on; }
void WisunReceiver::flush() {
    Impl& m = *p_;
    if (m.rate <= 0) return;
    m.waitIdle();
    m.report();
}

std::string WisunReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.pans, p_->pub.nodes, p_->pub.links, p_->pub.log, p_->pub.hops);
}
bool WisunReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    WisunTelemetry l;
    if (!results::unpack(blob, t0, l.pans, l.nodes, l.links, l.log, l.hops)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    WisunTelemetry c = l;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->resLoad = true;
    p_->pub.seq++;
    return true;
}
bool WisunReceiver::telemetry(WisunTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void WisunReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning wisunTuning() {
    ModeTuning t;
    t.stdMode = 86; t.id = "wisun"; t.name = "Wi-SUN FAN";
    t.minMhz = 470; t.maxMhz = 2484; t.defMhz = 921.5;  // the band plans run from 470 MHz (China) to 2.4 GHz; the middle of the ANZ band
    t.sampleRate = 20000000;                 // all of 915 - 928 MHz at once (the nodes hop over every channel)
    t.basebandHz = 0;
    t.bandwidthMhz = 18;
    t.minSampleRate = 2000000;               // a few channels at a time from slower radios
    t.tuneOffsetHz = 0;                      // the DC spike falls between the 200 kHz channels
    return t;
}

} // namespace dect2
