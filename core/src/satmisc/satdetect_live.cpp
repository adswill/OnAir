// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Satellite carrier and burst detection (see satdetect_live.h).
#include "dect2/satmisc/satdetect_live.h"
#include "dect2/satmisc/line_tracker.h"
#include "dect2/satmisc/sat_id.h"
#include "dect2/satmisc/satdetect_tel.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace dect2 {

using namespace satmisc;

namespace {
int log2For(double rate, double binHz, int lo, int hi) {
    int l = lo;
    while (l < hi && rate / (1 << l) > binHz) l++;
    return l;
}
}

struct SatDetectReceiver::Impl {
    std::mutex mu;
    int std_ = 0;
    const SatSystem* sys = nullptr;
    double rate = 0;
    std::atomic<double> centre{0};
    std::atomic<bool> resetReq{false}, clearReq{false};
    std::atomic<double> thrReq{0};
    std::atomic<int64_t> startReq{0};
    int64_t baseUtc = 0;
    uint64_t nIn = 0, nextPub = 0, seq = 0;
    std::function<void(const std::string&)> logCb;
    SatDetectTelemetry pub;

    std::mutex satMu;
    std::vector<Tle> tles;
    Observer rxLoc;

    // Carrier
    LineTracker lines;
    std::vector<SdLine> ended;
    uint64_t ok = 0, bad = 0;
    double lastIdSec = 0;
    uint32_t idTrack = 0;
    SdLine idBest;

    // Channel
    struct ChanState { int run = 0; float duty = 0; bool on = false; uint32_t bursts = 0; float snr = 0, width = 0, last = 0; };
    std::vector<ChanState> chans;
    int chFft = 0, chFill = 0, chPerBin = 1;
    std::vector<cf32> chBuf;
    std::vector<float> chWin, chAvg, chAcc;
    int chN = 0, chM = 0, chCount = 0;

    // Frame
    std::vector<float> env;
    size_t envFill = 0;
    int envDecim = 1, envAcc = 0;
    float envSum = 0;
    double envRate = 0;
    int envFft = 16;
    std::vector<float> envAvg;
    int envCount = 0;
    SdFrameLine frame;
    // repeated sequence
    std::vector<cf32> dly;
    size_t dlyPos = 0, dlyLen = 0;
    int foldBins = 32, foldPos = 0, foldBlock = 0;
    uint64_t foldN = 0;
    // three lags (the period and one sample either side): a 50 ppm sample clock moves a 20000-sample period by a sample, and the
    // sequence is white at the capture's rate, so the product at the nominal lag alone lost the repetition
    static constexpr int kLags = 3;
    cf32 foldAcc[kLags] = {};
    float foldPow = 0;
    std::vector<float> fold[kLags], foldMag;
    int foldCount = 0;

    static int64_t nowUtc() { return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

    void setup() {
        nIn = 0; nextPub = 0; ok = 0; bad = 0; ended.clear(); idTrack = 0; idBest = SdLine{}; lastIdSec = 0;
        const int64_t s = startReq.load();
        baseUtc = s ? s : nowUtc();
        pub = SatDetectTelemetry{};
        if (!sys || rate <= 0) return;
        const double thr = thrReq.load() > 0 ? thrReq.load() : sys->thresholdDb;
        if (sys->method == SdMethod::Carrier) {
            LineTrackerConfig c;
            c.rate = rate;
            c.fftLog2 = log2For(rate, sys->binHz, 8, 20);
            const double bin = rate / (1 << c.fftLog2);
            c.average = std::max(1, (int)std::lround(0.25 * rate / ((1 << c.fftLog2) / 2)));   // about a quarter of a second per detection
            c.thresholdDb = thr; c.minDurSec = 5; c.minPoints = 8; c.gapSec = 5; c.maxSlopeHzS = 8e-6 * sys->defMhz * 1e3 * 4 + 20 * bin;
            lines.configure(c);
        } else if (sys->method == SdMethod::Channel) {
            chFft = log2For(rate, sys->binHz, 8, 18);
            chN = 1 << chFft;
            chFill = 0;
            chBuf.assign((size_t)chN, cf32(0, 0));
            chWin.resize((size_t)chN);
            for (int i = 0; i < chN; i++) chWin[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / chN));
            chAvg.assign((size_t)chN, 0.f); chAcc.assign((size_t)chN, 0.f); chCount = 0; chM = std::max(2, (int)(0.02 * rate / chN));   // decisions every 20 ms
            chPerBin = std::max(1, (int)std::lround(sys->channelHz / (rate / chN)));
            chans.assign((size_t)(chN / chPerBin + 1), ChanState{});
        } else {
            frame = SdFrameLine{}; frame.rateHz = sys->frameHz; frame.name = sys->name;
            if (sys->method == SdMethod::FrameGap) {
                envDecim = std::max(1, (int)(rate / 200e3));
                envRate = rate / envDecim;
                envFft = 16;
                env.assign((size_t)1 << envFft, 0.f);
                envAvg.assign((size_t)1 << envFft, 0.f);
                envFill = 0; envAcc = 0; envSum = 0; envCount = 0;
            } else {
                dlyLen = (size_t)std::lround(rate / sys->frameHz);
                dly.assign(dlyLen + 1, cf32(0, 0)); dlyPos = 0;
                foldBins = 64;
                for (auto& f : fold) f.assign((size_t)foldBins, 0.f);
                foldMag.assign((size_t)foldBins, 0.f);
                foldN = 0; foldPos = 0; for (auto& a : foldAcc) a = cf32(0, 0); foldPow = 0; foldCount = 0;
            }
        }
    }

    std::string channelName(double hz) const {
        if (!sys || centre.load() <= 0) return {};
        const double mhz = (centre.load() + hz) / 1e6;
        std::string best; double bd = std::max(0.0035, 3e-5 * mhz);   // a low orbit's Doppler keeps a carrier within about 25 ppm of its channel
        for (int i = 0; i < sys->numChannels; i++) { const double d = std::fabs(sys->channels[i].mhz - mhz); if (d < bd) { bd = d; best = sys->channels[i].name; } }
        return best;
    }

    SdLine lineOf(const LineTrack& tr, bool active) {
        SdLine l;
        l.id = tr.id; l.offsetHz = tr.lastHz(); l.mhz = centre.load() > 0 ? (centre.load() + l.offsetHz) / 1e6 : 0.0;
        l.snrDb = tr.peakSnrDb; l.durSec = (float)tr.durationSec(); l.active = active; l.channel = channelName(l.offsetHz);
        if (tr.pts.size() >= 2) {
            const size_t m = std::min<size_t>(tr.pts.size(), 16);
            const auto& a = tr.pts[tr.pts.size() - m]; const auto& b = tr.pts.back();
            if (b.t > a.t) l.driftHzS = (float)((b.hz - a.hz) / (b.t - a.t));
        }
        return l;
    }

    void match(const LineTrack& tr, SdLine& l) {
        if (!sys->tleMatch || tr.durationSec() < 30 || centre.load() <= 0) return;
        std::vector<Tle> t; Observer rx;
        { std::lock_guard<std::mutex> k(satMu); t = tles; rx = rxLoc; }
        if (t.empty()) return;
        std::vector<DopplerSample> v;
        for (const auto& q : tr.pts) v.push_back({(double)baseUtc + q.t, q.hz, 0});
        SatIdOptions o; o.refHz = centre.load() + tr.pts.front().hz; o.fitOffset = true; o.goodRmsHz = 60; o.refine = 0;
        const SatIdResult r = identifySatellite(t, rx, v, o);
        if (r.ok) { l.sat = r.ranked[0].tle.name; l.norad = r.ranked[0].tle.norad; l.rmsHz = (float)r.ranked[0].rmsHz; l.ambiguous = r.ambiguous; }
    }

    void carrier(const cf32* x, size_t n) {
        lines.push(x, n);
        for (LineTrack& tr : lines.takeFinished()) {
            SdLine l = lineOf(tr, false);
            match(tr, l);
            std::lock_guard<std::mutex> lk(mu);
            ok++;
            ended.insert(ended.begin(), std::move(l));
            if (ended.size() > kSdLineMax / 2) ended.pop_back();
        }
    }

    void channelFrame() {
        std::vector<cf32> w((size_t)chN);
        for (int i = 0; i < chN; i++) w[(size_t)i] = chBuf[(size_t)i] * chWin[(size_t)i];
        Fft f(chN); f.forward(w.data());
        for (int i = 0; i < chN; i++) chAcc[(size_t)i] += std::norm(w[(size_t)((i + chN / 2) % chN)]);
        if (++chCount < chM) return;
        chCount = 0;
        for (int i = 0; i < chN; i++) chAvg[(size_t)i] = chAcc[(size_t)i] / (float)chM;
        std::fill(chAcc.begin(), chAcc.end(), 0.f);
        const double t = (double)nIn / rate;
        std::vector<float> s = chAvg;
        std::nth_element(s.begin(), s.begin() + (long)(s.size() * 2 / 5), s.end());
        const float floorP = std::max(s[s.size() * 2 / 5], 1e-20f);
        const double thr = thrReq.load() > 0 ? thrReq.load() : sys->thresholdDb, binHz = rate / chN;
        const double logN = std::log(100.0 * chN);
        const float on = (float)std::max(std::pow(10.0, thr / 10), 1.5 * (1 + std::sqrt(2 * logN / chM) + logN / chM));   // noise alone stays under this
        std::lock_guard<std::mutex> lk(mu);
        for (size_t c = 0; c < chans.size(); c++) {
            const int b0 = (int)c * chPerBin, b1 = std::min(chN, b0 + chPerBin);
            float mx = 0; int nOn = 0;
            for (int b = b0; b < b1; b++) { mx = std::max(mx, chAvg[(size_t)b]); if (chAvg[(size_t)b] > on * floorP) nOn++; }
            auto& st = chans[c];
            const bool hit = mx > on * floorP;
            st.run = hit ? std::min(st.run + 1, 100) : 0;
            const bool active = st.run >= 2;   // two decisions in a row
            const float dt = (float)((double)chN * chM / rate);
            st.duty += (active ? 1.f : -1.f) * dt / 20.f;
            st.duty = std::min(1.f, std::max(0.f, st.duty));
            if (active) {
                if (!st.on) { st.bursts++; ok++; }
                st.last = (float)t; st.snr = 10 * std::log10(mx / floorP); st.width = (float)(nOn * binHz);
            }
            st.on = active;
        }
    }

    void channels_(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            chBuf[(size_t)chFill++] = x[i];
            if (chFill == chN) { channelFrame(); chFill = 0; }
        }
    }

    void frameGap(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            envSum += std::norm(x[i]);
            if (++envAcc < envDecim) continue;
            env[envFill++] = envSum / (float)envDecim;
            envAcc = 0; envSum = 0;
            if (envFill == env.size()) { envFill = 0; envBlock(); }
        }
    }

    // A power envelope block: remove its mean, take its spectrum, and add it to the running average of spectra. The frame gap shows as lines at the
    // frame rate and its harmonics; a radio clock error moves all of them in proportion, so a small range of scales is tried.
    void envBlock() {
        const size_t N = env.size();
        double mean = 0;
        for (float e : env) mean += e;
        mean /= (double)N;
        std::vector<cf32> w(N);
        for (size_t i = 0; i < N; i++) w[i] = cf32((float)((env[i] - mean) * (0.5 - 0.5 * std::cos(2 * M_PI * (double)i / (double)N))), 0.f);
        Fft f((int)N); f.forward(w.data());
        for (size_t i = 0; i < N; i++) envAvg[i] += std::norm(w[i]);
        envCount++;
        const double binH = envRate / (double)N;
        const int kh = 8;
        double bestRatio = 0, bestRate = 0;
        for (double ppm = -60; ppm <= 60; ppm += 4) {
            const double fr = sys->frameHz * (1 + ppm * 1e-6);
            double sum = 0, ref = 0;
            for (int h = 1; h <= kh; h++) {
                const double b = fr * h / binH;
                const size_t bi = (size_t)std::lround(b);
                if (bi + 40 >= N / 2 || bi < 45) continue;
                float pk = 0;
                for (size_t k = bi - 1; k <= bi + 1; k++) pk = std::max(pk, envAvg[k]);
                std::vector<float> nb;
                for (int j = 10; j <= 40; j++) { nb.push_back(envAvg[bi + (size_t)j]); nb.push_back(envAvg[bi - (size_t)j]); }
                std::nth_element(nb.begin(), nb.begin() + (long)nb.size() / 2, nb.end());
                sum += pk; ref += nb[nb.size() / 2];
            }
            if (ref > 0 && sum / ref > bestRatio) { bestRatio = sum / ref; bestRate = fr; }
        }
        const float db = (float)(10 * std::log10(std::max(bestRatio, 1e-6)));
        std::lock_guard<std::mutex> lk(mu);
        frame.levelDb = db; frame.measuredHz = bestRate;
        // 8 harmonics of an exponential spectrum, averaged over several blocks: 6 dB is far in the tail, and more blocks only make the statistic steadier
        const double need = (thrReq.load() > 0 ? thrReq.load() : sys->thresholdDb) - 3;
        if (envCount >= 4) { const bool det = db > need; if (det && !frame.detected) ok++; frame.detected = det; }
    }

    void repeatSeq(const cf32* x, size_t n) {
        const size_t D = dlyLen + 1;   // the ring holds the last D samples: from its oldest slot on, lags dlyLen + 1, dlyLen, dlyLen - 1
        for (size_t i = 0; i < n; i++) {
            const cf32 p0 = dly[dlyPos], p1 = dly[dlyPos + 1 < D ? dlyPos + 1 : 0], p2 = dly[dlyPos + 2 < D ? dlyPos + 2 : dlyPos + 2 - D];
            dly[dlyPos] = x[i];
            if (++dlyPos == D) dlyPos = 0;
            foldAcc[0] += x[i] * std::conj(p0);
            foldAcc[1] += x[i] * std::conj(p1);
            foldAcc[2] += x[i] * std::conj(p2);
            foldPow += std::norm(x[i]);
            const int pos = (int)(foldN % dlyLen * (uint64_t)foldBins / dlyLen);
            foldN++;
            if (pos != foldPos) {   // a fold bin ended: one bin per position of the frame, however the frame divides
                for (int k = 0; k < kLags; k++) { fold[k][(size_t)foldPos] += std::abs(foldAcc[k]) / std::max(foldPow, 1e-20f); foldAcc[k] = cf32(0, 0); }
                foldPow = 0;
                if (pos == 0 && ++foldCount % 400 == 0) foldEval();
                foldPos = pos;
            }
        }
    }

    void foldEval() {
        float z = 0;   // the best of the three lags
        for (int k = 0; k < kLags; k++) {
            std::vector<float> s = fold[k];
            std::nth_element(s.begin(), s.begin() + (long)s.size() / 2, s.end());
            const float med = std::max(s[s.size() / 2], 1e-12f);
            float pk = 0;
            for (float v : fold[k]) pk = std::max(pk, v);
            // the spread of the other bins sets how far out a peak must be
            float dev = 0;
            for (float v : fold[k]) dev += std::fabs(v - med);
            dev = std::max(dev / (float)fold[k].size(), 1e-12f);
            z = std::max(z, (pk - med) / dev);
        }
        std::lock_guard<std::mutex> lk(mu);
        frame.levelDb = 10 * std::log10(std::max(z, 1e-3f));
        frame.measuredHz = sys->frameHz;
        const double need = (thrReq.load() > 0 ? thrReq.load() : sys->thresholdDb) + 3;
        const bool det = frame.levelDb > need;
        if (det && !frame.detected) ok++;
        frame.detected = det;
        foldCount = 0;
        for (auto& f : fold) std::fill(f.begin(), f.end(), 0.f);
    }

    void publish() {
        SatDetectTelemetry& t = pub;
        t.seq = ++seq;
        t.standard = sys ? sys->stdMode - 1 : 0;
        t.state = 1;
        t.timeSec = (double)nIn / rate; t.inputRate = rate; t.centreHz = centre.load();
        { std::lock_guard<std::mutex> k(satMu); t.catalog = (int)tles.size(); }
        t.lines.clear(); t.channels.clear(); t.frames.clear();
        t.blocksOk = ok; t.blocksBad = bad;
        if (sys->method == SdMethod::Carrier) {
            t.binHz = lines.binHz();
            float bestSnr = -200; double bestHz = 0;
            for (const LineTrack& tr : lines.active()) {
                if (tr.durationSec() < 2.5) continue;
                SdLine l = lineOf(tr, true);
                if (idTrack == tr.id) { l.sat = idBest.sat; l.norad = idBest.norad; l.rmsHz = idBest.rmsHz; l.ambiguous = idBest.ambiguous; }
                if (l.snrDb > bestSnr) { bestSnr = l.snrDb; bestHz = l.offsetHz; }
                t.lines.push_back(std::move(l));
                if (t.lines.size() >= kSdLineMax / 2) break;
            }
            t.lines.insert(t.lines.end(), ended.begin(), ended.end());
            t.state = t.lines.empty() ? 1 : 2;
            t.dataValid = !t.lines.empty() && t.lines[0].active;
            t.snrDb = bestSnr > -200 ? bestSnr : 0.f; t.cfoHz = bestHz;
            const auto& db = lines.spectrumDb();
            if (!db.empty()) {
                t.spectrum.assign(256, -100.f);
                const size_t n = db.size();
                for (size_t i = 0; i < n; i++) { float& v = t.spectrum[i * 256 / n]; v = std::max(v, db[i]); }
            }
        } else if (sys->method == SdMethod::Channel) {
            t.binHz = rate / chN;
            for (size_t c = 0; c < chans.size(); c++) {
                const auto& st = chans[c];
                if (st.bursts == 0) continue;
                SdChan o;
                o.offsetHz = (double)(c * (size_t)chPerBin + (size_t)chPerBin / 2) * rate / chN - rate / 2;
                o.mhz = centre.load() > 0 ? (centre.load() + o.offsetHz) / 1e6 : 0.0;
                o.widthHz = st.width; o.snrDb = st.snr; o.duty = st.duty; o.bursts = st.bursts; o.lastSec = st.last; o.active = st.on;
                o.channel = channelName(o.offsetHz);
                t.channels.push_back(std::move(o));
            }
            std::sort(t.channels.begin(), t.channels.end(), [](const SdChan& a, const SdChan& b) { return a.active != b.active ? a.active : a.snrDb > b.snrDb; });
            if (t.channels.size() > kSdChanMax) t.channels.resize(kSdChanMax);
            t.state = t.channels.empty() ? 1 : 2;
            t.dataValid = !t.channels.empty() && t.channels[0].active;
            if (!t.channels.empty()) { t.snrDb = t.channels[0].snrDb; t.cfoHz = t.channels[0].offsetHz; }
        } else {
            t.frames.push_back(frame);
            t.state = frame.detected ? 2 : 1;
            t.dataValid = frame.detected;
            t.snrDb = frame.levelDb;
        }
    }
};

SatDetectReceiver::SatDetectReceiver() : p_(std::make_unique<Impl>()) {}
SatDetectReceiver::~SatDetectReceiver() = default;

void SatDetectReceiver::setSystem(int stdMode) { std::lock_guard<std::mutex> lk(p_->mu); p_->std_ = stdMode; p_->sys = satSystem(stdMode); if (p_->rate > 0) p_->setup(); }
void SatDetectReceiver::configure(double inputRateHz) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = inputRateHz; p_->setup(); }
void SatDetectReceiver::setSignalOffset(double) {}
void SatDetectReceiver::setCenterHz(double hz) { p_->centre = hz; }
void SatDetectReceiver::setStartUtc(int64_t utcSec) { p_->startReq = utcSec; std::lock_guard<std::mutex> lk(p_->mu); p_->baseUtc = utcSec; }
bool SatDetectReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->sys && p_->rate > 0; }
void SatDetectReceiver::reset() { p_->resetReq = true; }
void SatDetectReceiver::clearResults() { p_->clearReq = true; }
void SatDetectReceiver::setThresholdDb(double db) { p_->thrReq = db; }
void SatDetectReceiver::setSatellites(const std::vector<Tle>& tles, double latDeg, double lonDeg, double altKm) {
    std::lock_guard<std::mutex> k(p_->satMu);
    p_->tles = tles; p_->rxLoc = Observer{latDeg, lonDeg, altKm};
}
void SatDetectReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }

void SatDetectReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false) || m.clearReq.exchange(false)) { std::lock_guard<std::mutex> lk(m.mu); m.setup(); }
    if (!m.sys || m.rate <= 0 || n == 0) return;
    static thread_local std::vector<cf32> clean;
    for (size_t i = 0; i < n; i++)
        if (!std::isfinite(x[i].real()) || !std::isfinite(x[i].imag())) {
            clean.assign(x, x + n);
            for (auto& v : clean) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            x = clean.data();
            break;
        }
    m.nIn += n;
    if (m.sys->method == SdMethod::Carrier) m.carrier(x, n);
    else if (m.sys->method == SdMethod::Channel) m.channels_(x, n);
    else if (m.sys->method == SdMethod::FrameGap) m.frameGap(x, n);
    else m.repeatSeq(x, n);
    if (m.nIn >= m.nextPub) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.nextPub = m.nIn + (uint64_t)(0.25 * m.rate);
        m.publish();
    }
}

bool SatDetectReceiver::telemetry(SatDetectTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq == 0 || p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

ModeTuning satDetectTuning(int stdMode) {
    ModeTuning t;
    const SatSystem* s = satSystem(stdMode);
    if (!s) return t;
    t.stdMode = stdMode; t.id = s->id; t.name = s->name;
    t.minMhz = s->minMhz; t.maxMhz = s->maxMhz; t.defMhz = s->defMhz;
    t.sampleRate = s->sampleRate; t.minSampleRate = std::min(s->sampleRate, 250e3);
    t.bandwidthMhz = s->sampleRate * 0.9 / 1e6;
    t.tuneOffsetHz = 0;
    t.carrierAtCentre = s->method == SdMethod::Carrier;
    return t;
}

} // namespace dect2
