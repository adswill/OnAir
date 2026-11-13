// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/qo100/qo100_rx.h"
#include "dect2/qo100/qo100_tel.h"
#include "dect2/qo100/qo100_dsp.h"
#include "dect2/qo100/qo100_auto.h"
#include "dect2/wb/wb_export.h"
#include "dect2/dmr/dmr_dsp.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/marine/marine_dsp.h"
#include "dect2/specx/specx_measure.h"
#include "../audio/ham_listen.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <vector>

namespace dect2 {

using namespace qo100;

namespace {

constexpr double kFineSnrDb = 15;     // a CW beacon counts when its fine peak stands this far over the median bin
constexpr int kFineN = 4096;
constexpr int kMissMax = 6;           // fine estimates in a row without a beacon before the lock is given up (about 2.5 s)

struct Spec {                         // Hann-windowed power spectrum of blocks of N, accumulated (fft order shifted: bin N/2 = 0 Hz)
    int N = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    double wsum2 = 1;
    std::vector<cf32> buf, tmp;
    void setup(int n) {
        N = n;
        fft = std::make_unique<Fft>(N);
        win.resize((size_t)N);
        double s = 0;
        for (int i = 0; i < N; i++) { win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / N)); s += win[(size_t)i]; }
        wsum2 = s * s;
        buf.clear(); buf.reserve((size_t)N); tmp.resize((size_t)N);
    }
    void power(const cf32* x, std::vector<double>& acc) {
        for (int i = 0; i < N; i++) tmp[(size_t)i] = x[i] * win[(size_t)i];
        fft->forward(tmp.data());
        if (acc.size() != (size_t)N) acc.assign((size_t)N, 0.0);
        for (int i = 0; i < N; i++) acc[(size_t)i] += std::norm(tmp[(size_t)((i + N / 2) % N)]) / wsum2;
    }
};

int pow2AtLeast(double v, int lo, int hi) { int k = lo; while (k < v && k < hi) k <<= 1; return k; }


// Upper sideband, 300 .. 3000 Hz above the dial, to 8 kHz audio with a slow AGC: the listening channel, and the recorders of conversations
struct SsbDemod {
    marine::Front front;
    std::vector<cf32> taps, hist;
    int pos = 0, ph = 0;
    float env = 0;
    double off = 1e300, pow = 0; uint64_t cnt = 0;
    std::vector<cf32> base;
    void configure(double rate) {
        front.configure(rate); front.reset(); off = 1e300;
        const auto lp = dmr::lowpassTaps(1250, 1550, marine::kBaseRate, 60, 9, 801);
        taps.resize(lp.size());
        for (size_t k = 0; k < lp.size(); k++) {   // band-pass 300 .. 3000 Hz: the low-pass shifted up to 1650 Hz
            const double w = 2 * M_PI * 1650.0 * ((double)k - (double)(lp.size() / 2)) / marine::kBaseRate;
            taps[k] = cf32((float)(lp[k] * std::cos(w)), (float)(lp[k] * std::sin(w)));
        }
        hist.assign(2 * taps.size(), cf32(0, 0)); pos = ph = 0; env = 0; pow = 0; cnt = 0;
    }
    // appends 8 kHz audio; offHz: where the dial is in the input
    void feed(const cf32* x, size_t n, double offHz, std::vector<float>& out) {
        if (offHz != off) { off = offHz; front.setOffsetHz(offHz); }
        base.clear();
        front.process(x, n, base);
        const int T = (int)taps.size();
        for (const cf32 v : base) {
            hist[(size_t)pos] = v; hist[(size_t)(pos + T)] = v;
            if (++pos >= T) pos = 0;
            if (++ph < 3) continue;
            ph = 0;
            const cf32* w = &hist[(size_t)pos];   // oldest first
            float ar = 0, ai = 0;
            for (int k = 0; k < T; k++) {
                const cf32 h = taps[(size_t)(T - 1 - k)], sv = w[k];
                ar += h.real() * sv.real() - h.imag() * sv.imag();
                ai += h.real() * sv.imag() + h.imag() * sv.real();
            }
            const float m2 = ar * ar + ai * ai;
            pow += m2; cnt++;
            const float mag = std::sqrt(m2);
            env += (mag > env ? 0.01f : 0.0001f) * (mag - env);
            out.push_back(std::clamp(ar * 0.5f / (env + 1e-9f), -1.f, 1.f));
        }
    }
};

// A conversation being recorded (to a WAV file) and / or handed to speech to text
struct Recorder {
    SsbDemod ssb;
    std::vector<float> pcm, chunk;
    std::string path;
    bool file = true;
    double lastWrite = 0;
    bool dirty = false;
};

constexpr double kRecMaxSec = 1800;

} // namespace

struct Qo100Receiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> cb;
    double rate = 0;
    std::atomic<double> centreReq{0}, loReq{9750e6}, searchReq{600e3}, dialReq{10489.600e6};
    std::atomic<bool> resetReq{true};
    hfdig::Listen listen;
    Qo100Telemetry pub;

    // set up by setup()
    bool ok = false;
    double lo = 9750e6, centreIf = 0, rc = 0;   // rc: apparent RF of the radio's centre
    bool inv = false;
    // narrowband channel
    bool nbOk = false;
    double nbOff = 0, nbRate = 0, nbUse = 0;
    Nco nbNco;
    Decimator nbDec;
    Spec nbSpec;
    std::vector<double> nbAcc, nbAvg, srAcc;
    int nbAccN = 0, srN = 0;
    // fine beacon estimates
    bool fineOn = false;
    Nco fineNco[2];
    Decimator fineDec[2];
    std::vector<cf32> fineBuf[2];
    int fineHop[2] = {0, 0};
    double fineRate = 0;
    Spec fineSpec;
    std::vector<double> finePw;
    uint64_t fineGen = 0;
    Peak lastPk[2];
    bool epochReady[2] = {false, false};
    int miss = 0;
    // the lock
    int state = 0;
    bool corrValid = false;
    double lnbErr = 0, lockStart = 0;
    uint64_t meas = 0;
    std::vector<float> histT, histErr;
    // wideband
    int wbN = 0;
    uint64_t wbHop = 0, wbSkip = 0;
    Spec wbSpec;
    std::vector<double> wbAcc, wbAvg;
    int wbAccN = 0;
    // SSB
    SsbDemod ssb;
    double audPow = 0; uint64_t audN = 0; float audioDb = -200;
    // squelch of the listening channel: a noise floor under the audio power and a gate with a hold
    double sqFloor = 0, sqAcc = 0; int sqN = 0; bool sqOpen = false; int sqHold = 0;
    std::atomic<bool> squelch{false};
    // the narrowband auto decoder and the conversations
    Qo100Tracker trk;
    Qo100Conversations convs;
    Qo100AutoDecoder autod;
    Decimator abDec;
    std::vector<cf32> abS;
    double abRate = 0;
    std::atomic<unsigned> autoMask{0};
    std::atomic<int> maxSlotsReq{6}, listenConvReq{0}, autoListenReq{0};
    std::atomic<float> cpuReq{0.7f};
    std::atomic<double> gapReq{90};
    std::atomic<double> startUtcReq{std::numeric_limits<double>::quiet_NaN()};
    std::atomic<bool> clearReq{false};
    std::mutex recMu;
    std::map<int, bool> recWant;
    std::string recDir;
    std::function<std::string(const float*, size_t, double)> stt;
    std::map<int, Recorder> recs;
    int listenEff = 0, pick = 0;
    double pickSince = 0;
    // buffers and counters
    std::vector<cf32> mixed, nbS, fs, base;
    std::vector<float> aud;
    uint64_t inSamples = 0, nextPub = 0;
    double power = 0; uint64_t nPower = 0;

    double tSec() const { return rate > 0 ? (double)inSamples / rate : 0; }
    std::atomic<bool> autoCorr{true};   // off: the beacons are still measured (shown) but no frequency is corrected (a GPS-locked LNB)
    double corr() const { return corrValid && autoCorr.load() ? lnbErr : 0.0; }

    void log(const char* s) { if (cb) cb(s); }

    void setup() {
        lo = loReq.load();
        inv = invertedLo(lo);
        centreIf = centreReq.load();
        if (centreIf <= 0) centreIf = ifOf(rate >= 9e6 ? kWholeCentreHz : kNarrowCentreHz, lo);
        rc = rfOf(centreIf, lo);
        nbOff = kNbCentreHz - rc;
        nbOk = std::fabs(nbOff) + 260e3 < 0.48 * rate;
        nbNco.set(nbOff, rate); nbNco.reset();
        nbDec.configure(rate, std::min(1.0e6, 0.4 * rate), 1.8e6);
        nbDec.reset();
        nbRate = nbDec.outRate();
        nbUse = std::min(nbDec.factor() > 1 ? std::min(1.0e6, 0.4 * rate) : 0.45 * nbRate, 0.48 * rate - std::fabs(nbOff));
        nbSpec.setup(pow2AtLeast(nbRate / 100, 4096, 65536));
        nbAcc.clear(); nbAvg.clear(); srAcc.clear(); nbAccN = srN = 0;
        for (int i = 0; i < 2; i++) { fineDec[i].configure(nbRate, 1200, 3500); fineDec[i].reset(); fineBuf[i].clear(); fineHop[i] = 0; epochReady[i] = false; }
        fineRate = fineDec[0].outRate();
        fineSpec.setup(kFineN);
        fineOn = false; miss = 0;
        state = nbOk ? 1 : 0;
        corrValid = false; lnbErr = 0; meas = 0; histT.clear(); histErr.clear(); lockStart = 0;
        wbN = pow2AtLeast(rate / 1500, 1024, 16384);
        wbSpec.setup(wbN);
        wbHop = std::max<uint64_t>((uint64_t)wbN, (uint64_t)(rate / 40)); wbSkip = 0;
        wbAcc.clear(); wbAvg.clear(); wbAccN = 0;
        ssb.configure(nbRate);
        sqFloor = 0; sqAcc = 0; sqN = 0; sqOpen = false; sqHold = 0;
        abDec.configure(nbRate, 260e3, 520e3); abDec.reset(); abRate = abDec.outRate();
        autod.reset(); trk.reset(); convs.reset(); recs.clear(); listenEff = pick = 0;
        if (nbOk) autod.configure(abRate);
        listen.reset();
        inSamples = 0; nextPub = 0; power = 0; nPower = 0;
        ok = rate > 0;
        if (cb && ok) {
            char b[200];
            snprintf(b, sizeof b, "QO-100: %.3f Msps at IF %.3f MHz (LO %.3f MHz%s): %.3f - %.3f MHz%s", rate / 1e6, centreIf / 1e6, lo / 1e6, inv ? ", inverted" : "",
                     (rc - rate / 2) / 1e6, (rc + rate / 2) / 1e6, nbOk ? "" : "; the narrowband transponder is outside this band");
            cb(b);
        }
    }

    // the fine channels on the two beacons, from a coarse shift (apparent - nominal)
    void startFine(double shift) {
        const double c[2] = {kLowBeaconHz + shift - kNbCentreHz, kHighBeaconHz + shift - kNbCentreHz};
        for (int i = 0; i < 2; i++) { fineNco[i].set(c[i], nbRate); fineNco[i].reset(); fineDec[i].reset(); fineBuf[i].clear(); fineHop[i] = 0; epochReady[i] = false; }
        fineOn = true; miss = 0; fineGen++;
    }

    void epoch() {
        epochReady[0] = epochReady[1] = false;
        bool good[2]; double err[2];
        for (int i = 0; i < 2; i++) {
            good[i] = lastPk[i].ok && lastPk[i].snrDb >= kFineSnrDb;
            const double app = kNbCentreHz + fineNco[i].hz() + lastPk[i].hz;
            err[i] = (i ? kHighBeaconHz : kLowBeaconHz) - app;
        }
        bool have = false; double e = 0;
        if (good[0] && good[1]) {
            if (std::fabs(err[0] - err[1]) < 150) { e = 0.5 * (err[0] + err[1]); have = true; }
            else { const int k = lastPk[0].snrDb >= lastPk[1].snrDb ? 0 : 1; e = err[k]; have = true; }
        } else if (good[0] || good[1]) { e = good[0] ? err[0] : err[1]; have = true; }
        if (have && corrValid && state == 2 && std::fabs(e - lnbErr) > 2000) have = false;   // a jump: something else than the beacon
        if (have) {
            if (state != 2) {
                lockStart = tSec();
                char b[128]; snprintf(b, sizeof b, "QO-100: beacons locked, LNB error %+.3f kHz (%+.3f ppm)", e / 1e3, e / lo * 1e6); log(b);
            }
            state = 2; corrValid = true; lnbErr = e; meas++; miss = 0;
            histT.push_back((float)tSec()); histErr.push_back((float)e);
            if (histT.size() > 1500) { histT.erase(histT.begin(), histT.begin() + 300); histErr.erase(histErr.begin(), histErr.begin() + 300); }
            // keep each beacon near the middle of its fine channel
            bool move = false;
            for (int i = 0; i < 2; i++) if (good[i] && std::fabs(lastPk[i].hz) > 300) move = true;
            if (move) startFine(-e);
        } else if (++miss >= kMissMax) {
            if (state == 2) { state = 3; log("QO-100: beacons lost, keeping the last correction"); }
            fineOn = false; srAcc.clear(); srN = 0;
        }
    }

    void fineFeed(int i, const cf32* x, size_t n) {
        fs.clear();
        for (size_t k = 0; k < n; k++) mixed[k] = fineNco[i].mix(x[k]);
        fineDec[i].process(mixed.data(), n, fs);
        std::vector<cf32>& b = fineBuf[i];
        const uint64_t gen = fineGen;
        for (const cf32 v : fs) {
            b.push_back(v);
            if ((int)b.size() < kFineN) continue;
            std::fill(finePw.begin(), finePw.end(), 0.0);   // blocks of kFineN, half overlapped
            fineSpec.power(b.data(), finePw);
            b.erase(b.begin(), b.begin() + kFineN / 2);
            lastPk[i] = strongest(finePw, fineRate, 1000);
            epochReady[i] = true;
            if (epochReady[0] && epochReady[1]) epoch();
            if (!fineOn || gen != fineGen) return;   // lost, or both channels moved: the rest of this block belongs to the old setting
        }
    }

    void search() {
        const double bin = nbRate / nbSpec.N;
        std::vector<double> m(srAcc.size());
        for (size_t k = 0; k < m.size(); k++) m[k] = srAcc[k] / std::max(1, srN);
        const double maxShift = std::max(0.0, std::min(searchReq.load(), nbUse - 252e3));
        const BeaconFit f = findBeacons(m, kNbCentreHz - nbSpec.N / 2 * bin, bin, maxShift);
        srAcc.clear(); srN = 0;
        if (!f.ok) return;
        char b[160]; snprintf(b, sizeof b, "QO-100: beacons found %+.1f kHz from nominal (%.1f dB over the floor)", f.shiftHz / 1e3, std::min(f.lowDb, f.highDb) - f.floorDb); log(b);
        startFine(f.shiftHz);
    }

    void nbFeed(const cf32* x, size_t n) {
        for (size_t k = 0; k < n; k++) {
            nbSpec.buf.push_back(x[k]);
            if ((int)nbSpec.buf.size() == nbSpec.N) {
                nbSpec.power(nbSpec.buf.data(), nbAcc); nbAccN++;
                if (!fineOn) {
                    if (srAcc.size() != nbAcc.size()) srAcc.assign(nbAcc.size(), 0.0);
                    // the last frame is the newest part of nbAcc: add it to the search average too
                    const std::vector<cf32>& t = nbSpec.tmp;
                    const int N = nbSpec.N;
                    for (int i = 0; i < N; i++) srAcc[(size_t)i] += std::norm(t[(size_t)((i + N / 2) % N)]) / nbSpec.wsum2;
                    srN++;
                    if ((double)srN * N / nbRate >= 0.5) search();
                }
                nbSpec.buf.clear();
            }
        }
        if (fineOn) {
            if (mixed.size() < n) mixed.resize(n);
            for (int i = 0; i < 2 && fineOn; i++) fineFeed(i, x, n);
        }
        if (listen.on.load()) ssbFeed(x, n);
        autoFeed(x, n);
    }

    void ssbFeed(const cf32* x, size_t n) {
        const double off = dialReq.load() - corr() - kNbCentreHz;
        aud.clear();
        ssb.feed(x, n, off, aud);
        audPow += ssb.pow; audN += ssb.cnt; ssb.pow = 0; ssb.cnt = 0;
        if (squelch.load()) {
            // blocks of 0.1 s: the audio power against a floor that falls at once and rises slowly (3 % a second)
            for (float& v : aud) {
                sqAcc += (double)v * v; 
                if (++sqN >= 800) {
                    const double bp = sqAcc / sqN; sqAcc = 0; sqN = 0;
                    sqFloor = sqFloor <= 0 ? bp : std::min(bp, sqFloor * 1.0003);
                    if (bp > sqFloor * 4.0 + 1e-9) { sqOpen = true; sqHold = 4; }
                    else if (sqHold > 0 && bp > sqFloor * 2.5) sqHold = 4;
                    else if (sqHold > 0) sqHold--;
                    if (sqHold == 0) sqOpen = false;
                }
                if (!sqOpen) v = 0.f;
            }
        }
        listen.play(aud.data(), aud.size());
    }

    // the stream of the auto decoder and the recorders
    void autoFeed(const cf32* x, size_t n) {
        const bool decoding = (autoMask.load() & ~(unsigned)kQoDecSsb) != 0;
        bool recording;
        { std::lock_guard<std::mutex> lk(recMu); recording = !recs.empty(); }
        if (!decoding && !recording) return;
        abS.clear();
        abDec.process(x, n, abS);
        if (decoding) autod.feed(abS.data(), abS.size());
        if (recording) recordFeed();
    }

    void recordFeed() {
        std::lock_guard<std::mutex> lk(recMu);
        for (auto& kv : recs) {
            Recorder& r = kv.second;
            const Qo100Conversation* c = convs.find(kv.first);
            if (!c) continue;
            const double off = c->dialHz - corr() - kNbCentreHz;
            r.chunk.clear();
            r.ssb.feed(abS.data(), abS.size(), std::fabs(off - r.ssb.off) > 25 ? off : r.ssb.off, r.chunk);
            r.ssb.pow = 0; r.ssb.cnt = 0;
            if (!c->active) continue;
            if (r.file && r.pcm.size() < (size_t)(kRecMaxSec * 8000)) { r.pcm.insert(r.pcm.end(), r.chunk.begin(), r.chunk.end()); r.dirty = true; }
            if (stt) {
                sttBuf[kv.first].insert(sttBuf[kv.first].end(), r.chunk.begin(), r.chunk.end());
            }
        }
    }
    std::map<int, std::vector<float>> sttBuf;

    void writeRecording(int id, Recorder& r) {
        if (!r.file || r.pcm.empty() || r.path.empty()) return;
        std::string err;
        if (!wb::writeWavMono16(r.path, r.pcm, 8000, err)) log(("QO-100: recording failed: " + err).c_str());
        r.dirty = false;
    }

    // conversations: listen following, auto listen, recorders, speech to text (called from publish)
    void conversationTasks(Qo100AutoTelemetry& at, double now) {
        const auto& list = convs.list();
        // auto listen: the loudest active conversation or the newest one, switched only when it has gone quiet or the other one clearly wins
        const int mode = autoListenReq.load();
        if (mode == 0) pick = 0;
        else {
            const Qo100Conversation* cur = nullptr; const Qo100Conversation* best = nullptr;
            for (const auto& c : list) {
                if (c.id == pick) cur = &c;
                if (!c.active) continue;
                if (!best || (mode == 1 ? c.levelDb > best->levelDb : c.startSec > best->startSec)) best = &c;
            }
            if (!cur || !cur->active) { if (best) { pick = best->id; pickSince = now; } }
            else if (best && best->id != pick && now - pickSince > 3.0 && (mode == 2 || best->levelDb > cur->levelDb + 6)) { pick = best->id; pickSince = now; }
        }
        int want = listenConvReq.load();
        if (mode != 0 && pick) want = pick;
        listenEff = want;
        if (want) {
            for (const auto& c : list) if (c.id == want && c.dialHz > 0) {
                if (std::fabs(c.dialHz - dialReq.load()) >= 40 && !(c.active == false && std::fabs(c.dialHz - dialReq.load()) < 400)) dialReq = c.dialHz;
                break;
            }
        }
        // recorders
        std::unique_lock<std::mutex> lk(recMu);
        for (const auto& w : recWant) {
            if (!w.second || recs.count(w.first)) continue;
            const Qo100Conversation* c = convs.find(w.first);
            if (!c) continue;
            Recorder r;
            r.ssb.configure(abRate);
            const auto tt = (std::time_t)c->startUtc; struct tm g {};
#ifdef _WIN32
            gmtime_s(&g, &tt);
#else
            gmtime_r(&tt, &g);
#endif
            char nm[96]; strftime(nm, sizeof nm, "qo100-conv%d-%Y%m%d-%H%M%S.wav", &g);
            char fn[160]; snprintf(fn, sizeof fn, nm, w.first);
            r.path = recDir.empty() ? std::string() : recDir + "/" + fn;
            r.file = !r.path.empty();
            recs[w.first] = std::move(r);
        }
        for (auto it = recs.begin(); it != recs.end();) {
            const int id = it->first;
            auto wi = recWant.find(id);
            const Qo100Conversation* c = convs.find(id);
            const bool stop = wi == recWant.end() || !wi->second;
            const bool over = !c || (!c->active && c->lastSec < now - 5 && now - c->lastSec > gapReq.load());
            if (it->second.dirty && now - it->second.lastWrite > 10) { writeRecording(id, it->second); it->second.lastWrite = now; }
            if (stop || over) { writeRecording(id, it->second); sttBuf.erase(id); it = recs.erase(it); if (over && wi != recWant.end()) wi->second = false; }
            else ++it;
        }
        // speech to text: 8 s of a conversation's audio at a time (not under the lock: the callback may take a while)
        lk.unlock();
        if (stt) for (auto& kv : sttBuf) if (kv.second.size() >= 64000) {
            const std::string txt = stt(kv.second.data(), kv.second.size(), 8000.0);
            kv.second.clear();
            if (!txt.empty()) convs.setSnippet(kv.first, txt);
        }
        lk.lock();
        for (auto& c : at.convs) {
            c.listening = c.id == want;
            auto r = recs.find(c.id);
            c.recording = r != recs.end() && r->second.file;
            if (r != recs.end()) c.recordPath = r->second.path;
        }
        at.listenConv = want; at.autoListen = mode;
    }

    void publish() {
        Qo100Telemetry t;
        const double c = corr();
        t.state = state; t.rate = rate; t.centreIfHz = centreIf; t.loHz = lo; t.inverted = inv; t.nbOk = nbOk;
        t.wbOk = std::min(rc + 0.45 * rate, kWbHiHz) - std::max(rc - 0.45 * rate, kWbLoHz) >= 2e6;   // at least 2 MHz of the wideband transponder
        t.corrApplied = autoCorr.load();
        t.corrValid = corrValid; t.lnbErrHz = lnbErr; t.lnbErrPpm = lnbErr / lo * 1e6;
        t.lockSec = state == 2 ? tSec() - lockStart : 0;
        t.measurements = meas;
        t.histT = histT; t.histErr = histErr;
        if (!histT.empty()) {
            std::vector<double> tt, vv;
            for (size_t i = 0; i < histT.size(); i++) if (histT[i] >= histT.back() - 30) { tt.push_back(histT[i]); vv.push_back(histErr[i]); }
            t.driftValid = tt.size() >= 8 && tt.back() - tt.front() > 10 && slopePerMin(tt, vv, t.driftHzPerMin);
        }
        const double nomB[3] = {kLowBeaconHz, kPskBeaconHz, kHighBeaconHz};
        for (int i = 0; i < 3; i++) t.beacons[i].nominalHz = nomB[i];
        if (corrValid) for (int i = 0; i < 3; i += 2) {
            const int k = i / 2;
            t.beacons[i].found = state == 2 && lastPk[k].ok && lastPk[k].snrDb >= kFineSnrDb;
            t.beacons[i].rfHz = kNbCentreHz + fineNco[k].hz() + lastPk[k].hz;
            t.beacons[i].snrDb = lastPk[k].snrDb;
        }
        // narrowband spectrum
        if (nbAccN) {
            if (nbAvg.size() != nbAcc.size()) { nbAvg.assign(nbAcc.size(), 0.0); for (size_t k = 0; k < nbAcc.size(); k++) nbAvg[k] = nbAcc[k] / nbAccN; }
            else for (size_t k = 0; k < nbAcc.size(); k++) nbAvg[k] = 0.5 * nbAvg[k] + 0.5 * nbAcc[k] / nbAccN;
            std::fill(nbAcc.begin(), nbAcc.end(), 0.0); nbAccN = 0;
        }
        if (nbOk && !nbAvg.empty()) {
            const int N = (int)nbAvg.size();
            const double bin = nbRate / N, f0 = kNbCentreHz - N / 2 * bin;   // apparent RF of bin 0
            const double lim = std::min(nbUse, 0.5 * nbRate);
            int ka = (int)std::ceil((std::max(kNbLoHz - 50e3 - c, kNbCentreHz - lim) - f0) / bin);
            int kb = (int)std::floor((std::min(kNbHiHz + 50e3 - c, kNbCentreHz + lim) - f0) / bin);
            ka = std::clamp(ka, 0, N - 1); kb = std::clamp(kb, ka, N - 1);
            t.nb.resize((size_t)(kb - ka + 1));
            for (int k = ka; k <= kb; k++) t.nb[(size_t)(k - ka)] = (float)(10 * std::log10(nbAvg[(size_t)k] + 1e-20));
            t.nbF0Hz = f0 + ka * bin + c; t.nbBinHz = bin;
            std::vector<float> s(t.nb);
            std::nth_element(s.begin(), s.begin() + (long)(s.size() / 2), s.end());
            t.nbFloorDb = s[s.size() / 2];
            t.nbPeakDb = *std::max_element(t.nb.begin(), t.nb.end());
            findSignals(t);
            if (corrValid) {   // the PSK beacon: its level near where it should be
                const double want = kPskBeaconHz;
                const int k0 = (int)std::lround((want - t.nbF0Hz) / bin), w = (int)(500 / bin) + 1;
                float m = -200; int km = -1;
                for (int k = std::max(0, k0 - w); k <= std::min((int)t.nb.size() - 1, k0 + w); k++) if (t.nb[(size_t)k] > m) { m = t.nb[(size_t)k]; km = k; }
                t.beacons[1].snrDb = m - t.nbFloorDb;
                t.beacons[1].found = km >= 0 && m - t.nbFloorDb >= 8;
                if (km >= 0) t.beacons[1].rfHz = t.nbF0Hz + km * bin - c;
            }
            // the tracker, the conversations and the decoders on the signals
            {
                if (clearReq.exchange(false)) autod.clearResults();
                autod.setEnabled(autoMask.load()); autod.setMaxSlots(maxSlotsReq.load()); autod.setCpuLimit(cpuReq.load());
                autod.setStartTime(startUtcReq.load());
                convs.setGapSec(gapReq.load());
                const double now = tSec();
                trk.update(t.nb.data(), t.nb.size(), t.nbF0Hz, t.nbBinHz, t.nbFloorDb, now);
                const double utc = (double)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;
                convs.update(trk.mutableTracks(), now, utc);
                autod.plan(trk.mutableTracks(), kNbCentreHz, corr(), now);
                { int id = 0, cls = 0; std::string nm; while (autod.takeIdentified(id, nm, cls)) trk.setIdentified(id, nm, cls); }
                autod.fill(t.autoDec);
                t.autoDec.tracks = trk.tracks();
                t.autoDec.convs = convs.list();
                conversationTasks(t.autoDec, now);
                for (auto& s : t.signals) {   // the signal list names what the tracker made of them
                    for (const auto& k : t.autoDec.tracks) if (std::fabs(k.rfHz - s.rfHz) < std::max(200.0, 0.5 * k.widthHz)) { s.cls = k.cls; s.track = k.id; break; }
                }
            }
        }
        // wideband spectrum and its DATV carriers
        if (wbAccN) {
            if (wbAvg.size() != wbAcc.size()) { wbAvg.assign(wbAcc.size(), 0.0); for (size_t k = 0; k < wbAcc.size(); k++) wbAvg[k] = wbAcc[k] / wbAccN; }
            else for (size_t k = 0; k < wbAcc.size(); k++) wbAvg[k] = 0.7 * wbAvg[k] + 0.3 * wbAcc[k] / wbAccN;
            std::fill(wbAcc.begin(), wbAcc.end(), 0.0); wbAccN = 0;
        }
        if (!wbAvg.empty()) {
            const int N = (int)wbAvg.size();
            t.wb.resize((size_t)N);
            for (int k = 0; k < N; k++) t.wb[(size_t)k] = (float)(10 * std::log10(wbAvg[(size_t)k] + 1e-20));
            t.wbBinHz = rate / N; t.wbF0Hz = rc - N / 2 * t.wbBinHz + c;
            std::vector<float> s(t.wb);
            std::nth_element(s.begin(), s.begin() + (long)(s.size() / 2), s.end());
            t.wbFloorDb = s[s.size() / 2];
            t.wbPeakDb = *std::max_element(t.wb.begin(), t.wb.end());
            if (t.wbOk) findDatv(t);
        }
        t.listening = listen.on.load(); t.dialHz = dialReq.load();
        if (audN) { audioDb = (float)(10 * std::log10(audPow / audN + 1e-20)); audPow = 0; audN = 0; }
        t.audioDb = t.listening ? audioDb : -200;
        t.powerDb = nPower ? 10 * std::log10(std::max(1e-20, power / nPower)) : -200;
        power = 0; nPower = 0;
        std::lock_guard<std::mutex> lk(mu);
        t.seq = pub.seq + 1;
        pub = std::move(t);
    }

    // runs of bins 6 dB over the floor (gaps under 600 Hz bridged): one signal each, its top found by a parabola
    static void findSignals(Qo100Telemetry& t) {
        const int n = (int)t.nb.size();
        const double bin = t.nbBinHz;
        const float thr = t.nbFloorDb + 6;
        const int gap = std::max(1, (int)(600 / bin));
        int k = 0;
        while (k < n && t.signals.size() < 40) {
            if (t.nb[(size_t)k] < thr) { k++; continue; }
            int a = k, b = k, quiet = 0;
            for (int j = k + 1; j < n; j++) {
                if (t.nb[(size_t)j] >= thr) { b = j; quiet = 0; }
                else if (++quiet > gap) break;
            }
            int top = a;
            for (int j = a; j <= b; j++) if (t.nb[(size_t)j] > t.nb[(size_t)top]) top = j;
            double d = 0;
            if (top > 0 && top < n - 1) {
                const double l = t.nb[(size_t)top - 1], m = t.nb[(size_t)top], r = t.nb[(size_t)top + 1], den = l - 2 * m + r;
                if (den < 0) d = std::clamp(0.5 * (l - r) / den, -0.5, 0.5);
            }
            Qo100Signal s;
            s.rfHz = t.nbF0Hz + (top + d) * bin;
            s.levelDb = t.nb[(size_t)top]; s.snrDb = s.levelDb - t.nbFloorDb;
            int wa = top, wb = top;
            while (wa > a && t.nb[(size_t)wa - 1] >= s.levelDb - 6) wa--;
            while (wb < b && t.nb[(size_t)wb + 1] >= s.levelDb - 6) wb++;
            s.widthHz = (wb - wa + 1) * bin;
            const double nomB[3] = {kLowBeaconHz, kPskBeaconHz, kHighBeaconHz};
            for (int i = 0; i < 3; i++) if (std::fabs(s.rfHz - nomB[i]) < 1000) s.beacon = i;
            t.signals.push_back(s);
            k = b + 1;
        }
    }

    void findDatv(Qo100Telemetry& t) {
        specx::SpecView v;
        v.db = t.wb.data(); v.n = (int)t.wb.size(); v.f0Hz = t.wbF0Hz; v.binHz = t.wbBinHz; v.enbw = 1.5;
        for (const specx::Measure& m : specx::detect(v, 6, 3)) {
            if (m.centreHz < kWbLoHz - 500e3 || m.centreHz > kDownHiHz || m.bw3Hz < 60e3) continue;
            if (std::fabs(m.centreHz - rc - corr()) < 2 * t.wbBinHz && m.bw3Hz < 10 * t.wbBinHz) continue;   // the radio's DC spike
            Qo100Datv d;
            d.rfHz = m.centreHz; d.ifHz = ifOf(m.centreHz - corr(), lo);
            d.bw3Hz = m.bw3Hz; d.symRate = m.symRateBest > 0 ? m.symRateBest : m.bw3Hz;
            d.snrDb = (float)m.snrDb; d.looksDvbs = m.looksDvbs || m.flatTop;
            d.beacon = std::fabs(d.rfHz - kDatvBeaconHz) < 150e3 && std::fabs(d.symRate - kDatvBeaconSym) < 0.25e6;
            t.datv.push_back(d);
            if (t.datv.size() >= 24) break;
        }
    }
};

Qo100Receiver::Qo100Receiver() : p_(new Impl) {}
Qo100Receiver::~Qo100Receiver() {
    std::lock_guard<std::mutex> lk(p_->recMu);
    for (auto& kv : p_->recs) p_->writeRecording(kv.first, kv.second);
}

void Qo100Receiver::configure(double r) {
    { std::lock_guard<std::mutex> lk(p_->mu); const uint64_t s = p_->pub.seq; p_->pub = Qo100Telemetry(); p_->pub.seq = s; }
    p_->rate = r;
    p_->resetReq = true;
}
void Qo100Receiver::setCenterHz(double hz) { if (hz != p_->centreReq.load()) { p_->centreReq = hz; p_->resetReq = true; } }
bool Qo100Receiver::ready() const { return p_->rate >= qo100Tuning().minSampleRate - 1; }
void Qo100Receiver::reset() { p_->resetReq = true; }
void Qo100Receiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->cb = std::move(cb); }
void Qo100Receiver::setLo(double hz) { if (hz > 0 && hz != p_->loReq.load()) { p_->loReq = hz; p_->resetReq = true; } }
void Qo100Receiver::setSearchHz(double hz) { p_->searchReq = std::clamp(hz, 5e3, 2e6); }
void Qo100Receiver::setDialHz(double hz) { p_->dialReq = hz; }
void Qo100Receiver::setAutoCorrection(bool on) { p_->autoCorr = on; }
void Qo100Receiver::setListen(bool on) { p_->listen.on = on; }
bool Qo100Receiver::listening() const { return p_->listen.on.load(); }
void Qo100Receiver::setVolume(float v) { p_->listen.volume = std::clamp(v, 0.f, 1.f); }
void Qo100Receiver::setMuted(bool m) { p_->listen.muted = m; }
void Qo100Receiver::setSilent(bool s) { p_->listen.silent = s; }
void Qo100Receiver::setAutoDecoders(unsigned mask) { p_->autoMask = mask; }
void Qo100Receiver::setAutoMaxSlots(int n) { p_->maxSlotsReq = n; }
void Qo100Receiver::setAutoCpuLimit(float c) { p_->cpuReq = c; }
void Qo100Receiver::setAutoStartTime(double utc) { p_->startUtcReq = utc; }
void Qo100Receiver::clearAutoResults() { p_->clearReq = true; }
void Qo100Receiver::setConversationGapSec(double s) { p_->gapReq = s; }
void Qo100Receiver::setListenConversation(int id) { p_->listenConvReq = id; }
void Qo100Receiver::setAutoListen(int mode) { p_->autoListenReq = std::clamp(mode, 0, 2); }
void Qo100Receiver::setSquelch(bool on) { p_->squelch = on; }
void Qo100Receiver::setRecordConversation(int id, bool on) { std::lock_guard<std::mutex> lk(p_->recMu); p_->recWant[id] = on; }
void Qo100Receiver::setRecordDir(const std::string& dir) { std::lock_guard<std::mutex> lk(p_->recMu); p_->recDir = dir; }
void Qo100Receiver::setSpeechToText(std::function<std::string(const float*, size_t, double)> fn) { std::lock_guard<std::mutex> lk(p_->recMu); p_->stt = std::move(fn); }
void Qo100Receiver::setListenTap(std::function<void(const float* x, size_t n)> cb) { p_->listen.setTap(std::move(cb)); }

void Qo100Receiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.rate <= 0) return;
    if (m.resetReq.exchange(false) || !m.ok) m.setup();
    const size_t kChunk = 8192;
    std::vector<cf32> in;
    for (size_t at = 0; at < n; at += kChunk) {
        const size_t c = std::min(kChunk, n - at);
        in.assign(x + at, x + at + c);
        for (cf32& v : in) {
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            if (m.inv) v = std::conj(v);
            m.power += std::norm(v); m.nPower++;
        }
        // wideband: one block of wbN every wbHop samples
        for (size_t k = 0; k < c; k++) {
            if (m.wbSkip) { m.wbSkip--; continue; }
            m.wbSpec.buf.push_back(in[k]);
            if ((int)m.wbSpec.buf.size() == m.wbN) {
                m.wbSpec.power(m.wbSpec.buf.data(), m.wbAcc); m.wbAccN++;
                m.wbSpec.buf.clear();
                m.wbSkip = m.wbHop - (uint64_t)m.wbN;
            }
        }
        // narrowband: mix its centre to 0 Hz, decimate
        if (m.nbOk) {
            if (m.mixed.size() < c) m.mixed.resize(c);
            for (size_t k = 0; k < c; k++) m.mixed[k] = m.nbNco.mix(in[k]);
            m.nbS.clear();
            m.nbDec.process(m.mixed.data(), c, m.nbS);
            std::vector<cf32> nb;
            nb.swap(m.nbS);
            m.nbFeed(nb.data(), nb.size());
            m.nbS.swap(nb);
        }
        m.inSamples += c;
        if (m.inSamples >= m.nextPub) { m.nextPub = m.inSamples + (uint64_t)(m.rate / 10); m.publish(); }
    }
}

bool Qo100Receiver::telemetry(Qo100Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

ModeTuning qo100Tuning() {
    ModeTuning t;
    t.stdMode = 55; t.id = "qo100"; t.name = "QO-100";
    t.minMhz = 50; t.maxMhz = 1200;             // the IF: 739.5 - 750 MHz with a 9750 MHz LNB, 100 - 110.5 with 10600 (inverted), 489.5 - 500 with 10000
    t.defMhz = ifOf(kWholeCentreHz, 9750e6) / 1e6;   // 744.0 MHz: the whole downlink at 10 Msps
    t.sampleRate = 10e6;
    t.basebandHz = 0;
    t.bandwidthMhz = 10;
    t.minSampleRate = 2e6;
    return t;
}

std::string qo100Summary(const Qo100Telemetry& t) {
    char b[240];
    static const char* const kState[] = {"narrowband transponder not in the band", "searching for the beacons", "locked", "beacons lost, correction held"};
    const char* st = kState[std::clamp(t.state, 0, 3)];
    if (t.rate <= 0) return "QO-100: waiting for samples";
    if (t.corrValid)
        snprintf(b, sizeof b, "QO-100 %s: LNB %+.3f kHz (%+.2f ppm)%s, %zu signals, %zu DATV, %.1f dBFS", st, t.lnbErrHz / 1e3, t.lnbErrPpm,
                 t.driftValid ? (", drift " + std::to_string((int)std::lround(t.driftHzPerMin)) + " Hz/min").c_str() : "", t.signals.size(), t.datv.size(), t.powerDb);
    else
        snprintf(b, sizeof b, "QO-100 %s: IF %.3f MHz, LO %.0f MHz, %zu DATV, %.1f dBFS", st, t.centreIfHz / 1e6, t.loHz / 1e6, t.datv.size(), t.powerDb);
    return b;
}

} // namespace dect2
