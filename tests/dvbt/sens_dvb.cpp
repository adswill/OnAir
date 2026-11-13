// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sensitivity and time-to-lock sweep for DVB-T and DVB-T2 (not a ctest: run by hand, see docs/SENSITIVITY.md, DVB section).
//
//   sens_dvb t  <mode 0=2K 1=8K> <gi 0..3 = 1/32..1/4> <mod 0..2> <cr 0..4> [options]
//   sens_dvb t2 <fft code> <gi idx> <pp 0..7> <plpMod 0..3> <plpCod 0..7> [options]
//   options: --snr a:b:step  --trials n  --frames n  --chan awgn|echo0|tu6|rice|p1  --doppler Hz  --cfo Hz  --ppm x  --short  --norot  --ext
//
// SNR is C/N in the occupied bandwidth (signal power against the noise inside the K carriers), as in EN 300 744 Annex A.
// A trial passes when the stream decodes from 100 ms after the first good packet (DVB-T) / from the frame after the first good one (T2) to the end without a single loss
// and the first good one came within the first half of the signal. The threshold is the lowest SNR with >= 90 % of trials passing.
// Time to lock: seconds of signal from its start (after a random lead-in of noise) to the first good packet or frame.
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/dvbt/dvbt_rx.h"
#include "dect2/t2/t2.h"
#include "dect2/t2/t2gen.h"
#include "dect2/t2/t2rx.h"
#include "dect2/common/stable_random.h"
#include "../common/impair.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
using cd = std::complex<double>;

struct Opt {
    bool t2 = false;
    int a = 1, b = 2, c = 2, d = 2, e = 2;
    double s0 = 10, s1 = 20, ss = 0.5;
    int trials = 4, frames = 0;
    std::string chan = "awgn";
    double doppler = 0, cfo = 0, ppm = 0;
    bool shortFr = false, rot = true, ext = false;
    int threads = 2;
};

// ---- channel: tapped delay line with independent Rayleigh (sum of sinusoids, Jakes spectrum) or fixed taps
struct Tap { double delayUs, powDb; };
static std::vector<Tap> profile(const std::string& n) {
    if (n == "echo0") return {{0, 0}, {0, 0}};          // delay set from the guard below
    if (n == "tu6") return {{0, -3}, {0.2, 0}, {0.5, -2}, {1.6, -6}, {2.3, -8}, {5.0, -10}};   // COST 207 typical urban
    if (n == "rice" || n == "p1")                       // a short-delay six-path profile (Rician: a strong LOS on the first)
        return {{0, 0}, {0.5, -6}, {1.1, -9}, {1.9, -12}, {3.2, -15}, {5.1, -18}};
    return {{0, 0}};
}

static void channel(std::vector<cf32>& x, const Opt& o, double fn, int G, uint32_t seed) {
    if (o.chan == "awgn") return;
    std::vector<Tap> taps = profile(o.chan);
    if (o.chan == "echo0") {   // 0 dB echo at 90 % of the guard interval (SENS_ECHO="fraction:dB" for others)
        double fr = 0.9, db = 0;
        if (const char* e = getenv("SENS_ECHO")) sscanf(e, "%lf:%lf", &fr, &db);
        taps[1].delayUs = fr * G / fn * 1e6; taps[1].powDb = db;
    }
    std::mt19937 rng(seed * 7919 + 3);
    std::uniform_real_distribution<double> U(0, 2 * M_PI);
    const int M = 12;
    struct T { int d; double p; std::vector<double> w, ph; cd fixed; };
    std::vector<T> ts;
    double tot = 0;
    const bool rice = o.chan == "rice";
    for (auto& t : taps) tot += std::pow(10.0, t.powDb / 10);
    if (rice) tot += 10 * std::pow(10.0, taps[0].powDb / 10);   // K = 10 dB on the first path
    for (size_t i = 0; i < taps.size(); i++) {
        T t; t.d = (int)std::lround(taps[i].delayUs * 1e-6 * fn); t.p = std::pow(10.0, taps[i].powDb / 10) / tot;
        for (int m = 0; m < M; m++) {
            const double alpha = (2 * M_PI * m + U(rng) - M_PI) / M;
            t.w.push_back(2 * M_PI * o.doppler * std::cos(alpha) / fn); t.ph.push_back(U(rng));
        }
        t.fixed = std::polar(1.0, U(rng));
        ts.push_back(t);
    }
    const double los = rice ? std::sqrt(10 * std::pow(10.0, taps[0].powDb / 10) / tot) : 0;
    const bool staticCh = o.doppler == 0;
    const std::vector<cf32> src = x;
    for (auto& v : x) v = cf32(0, 0);
    for (auto& t : ts) {
        const double amp = std::sqrt(t.p);
        for (size_t n = (size_t)t.d; n < x.size(); n++) {
            cd g;
            if (staticCh) g = t.fixed * amp;
            else if ((n & 63) == 0 || n == (size_t)t.d) {
                cd s = 0;
                for (int m = 0; m < M; m++) s += std::polar(1.0, t.w[m] * (double)n + t.ph[m]);
                g = s * (amp / std::sqrt((double)M));
                t.fixed = g;   // held for 64 samples (7 us: far below the coherence time at any Doppler tested)
            } else g = t.fixed;
            x[n] += src[n - (size_t)t.d] * cf32((float)g.real(), (float)g.imag());
        }
    }
    if (los > 0) for (size_t n = 0; n < x.size(); n++) x[n] += src[n] * (float)los;
}

static void impairAndNoise(std::vector<cf32>& x, const Opt& o, double fn, int N, int K, int G, double cn, uint32_t seed, size_t lead) {
    double pw = 0;
    for (auto& v : x) pw += std::norm(v);
    pw /= (double)x.size();
    channel(x, o, fn, G, seed);
    if (o.ppm != 0) x = impair::clock(x, o.ppm);
    if (o.cfo != 0) impair::shift(x, o.cfo, fn);
    const double var = pw * (double)N / ((double)K * std::pow(10.0, cn / 10));   // whole-band noise for that in-band C/N
    std::mt19937 rng(seed);
    dect2::normal_distribution<float> nd(0, 1);
    const float s = (float)std::sqrt(var / 2);
    std::vector<cf32> y(lead + x.size());
    for (size_t i = 0; i < y.size(); i++) y[i] = (i >= lead ? x[i - lead] : cf32(0, 0)) + cf32(nd(rng), nd(rng)) * s;
    x.swap(y);
}

struct Trial { bool pass = false; double lockSec = -1; size_t good = 0, bad = 0, early = 0; };

static Trial runT(const Opt& o, double cn, uint32_t seed) {
    dvbt::Params p;
    p.mode = o.a ? dvbt::k8K : dvbt::k2K;
    const int gis[] = {dvbt::kGi32, dvbt::kGi16, dvbt::kGi8, dvbt::kGi4};
    p.guard = gis[o.b & 3];
    const int mods[] = {dvbt::kQpsk, dvbt::k16Qam, dvbt::k64Qam};
    p.mod = mods[std::min(2, o.c)];
    const int crs[] = {dvbt::kR12, dvbt::kR23, dvbt::kR34, dvbt::kR56, dvbt::kR78};
    p.crHp = p.crLp = crs[std::min(4, o.d)];
    uint32_t counter = 0;
    dvbt::Generator gen(p, [&](uint8_t* pkt) {
        pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0x00; pkt[3] = 0x10;
        memcpy(pkt + 4, &counter, 4);
        for (int i = 8; i < 188; i++) pkt[i] = (uint8_t)(counter * 31 + i * 7);
        counter++;
    }, seed);
    const double fn = nativeRateHz(8);
    const int frames = o.frames ? o.frames : (o.a ? 6 : 16);
    std::vector<cf32> x, sym;
    for (int s = 0; s < frames * 68; s++) { gen.nextSymbol(sym); x.insert(x.end(), sym.begin(), sym.end()); }
    std::mt19937 lr(seed * 31 + 1);
    const size_t lead = 20000 + lr() % 30000;
    impairAndNoise(x, o, fn, gen.fftN(), dvbt::carriersK(p.mode), gen.guard(), cn, seed, lead);
    if (getenv("SENS_VERBOSE")) fprintf(stderr, "  lead %zu (mod P %zu)\n", lead, lead % (size_t)(gen.fftN() + gen.guard()));
    DvbtReceiver rx;
    rx.configure(fn, 8);
    Trial t;
    size_t fed = 0; uint32_t last = 0; size_t lost = 0;
    // the steady state: from 100 ms of signal after the first good packet (losses before that count to the lock, not to the threshold)
    auto settled = [&]() { return t.lockSec >= 0 && (double)(fed > lead ? fed - lead : 0) / fn > t.lockSec + 0.1; };
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* q = pk + i * 188;
            uint32_t c; memcpy(&c, q + 4, 4);
            bool ok = !(q[1] & 0x80) && q[0] == 0x47 && q[1] == 0x01 && q[2] == 0x00;
            for (int j = 8; j < 188 && ok; j++) ok = q[j] == (uint8_t)(c * 31 + j * 7);
            if (ok) {
                if (t.good && c != last + 1 && settled()) lost += c > last ? c - last - 1 : 1;
                if (!t.good) t.lockSec = (double)((fed > lead ? fed - lead : 0)) / fn;
                last = c; t.good++;
            } else if (t.good) { if (settled()) t.bad++; else t.early++; }
        }
    });
    for (size_t i = 0; i < x.size(); i += 1 << 13) {
        const size_t n = std::min<size_t>(1 << 13, x.size() - i);
        fed = i + n;
        rx.feed(x.data() + i, n);
    }
    rx.flush();
    if (getenv("SENS_VERBOSE")) { RxTelemetry tl; rx.telemetry(tl, 0); fprintf(stderr, "  vit %.2f state %d tps %d fecSync %d mode %d gi %d mod %d cr %d snr %.1f cfo %.0f packets %llu rsFailed %llu\n", tl.dvbt.viterbiMargin, tl.state, tl.dvbt.tpsOk, tl.dvbt.fecSync, tl.dvbt.mode, tl.dvbt.guard, tl.dvbt.mod, tl.dvbt.crHp, tl.dataSnrDb, tl.cfoHz, (unsigned long long)tl.dvbt.packets, (unsigned long long)tl.dvbt.rsFailed);
        std::vector<std::pair<float,int>> pk; for (int i = 1; i + 1 < (int)tl.irDb.size(); i++) if (tl.irDb[i] > -25 && tl.irDb[i] >= tl.irDb[i-1] && tl.irDb[i] >= tl.irDb[i+1]) pk.push_back({tl.irDb[i], i + tl.irTauMin});
        {   // MER of the equalised data cells shown (nearest constellation point)
            const int M = tl.dvbt.mod == 0 ? 2 : tl.dvbt.mod == 1 ? 4 : 8;
            const double sc = tl.dvbt.mod == 0 ? std::sqrt(2.0) : tl.dvbt.mod == 1 ? std::sqrt(10.0) : std::sqrt(42.0);
            double es = 0, ee = 0;
            for (auto& v : tl.eqData) {
                auto dec = [&](double x) { double q = 2 * std::floor(x / 2) + 1; return std::max(-(double)M + 1, std::min((double)M - 1, q)); };
                const double xr = v.real() * sc, xi = v.imag() * sc, dr = dec(xr), di = dec(xi);
                es += dr * dr + di * di; ee += (xr - dr) * (xr - dr) + (xi - di) * (xi - di);
            }
            fprintf(stderr, "  data MER %.1f dB over %zu cells\n", ee > 0 ? 10 * std::log10(es / ee) : 99.0, tl.eqData.size());
        }
        fprintf(stderr, "  IR peaks:"); for (auto& q : pk) fprintf(stderr, " %d:%.1f", q.second, q.first); fprintf(stderr, "\n"); }
    const double dur = (double)(x.size() - lead) / fn;
    t.pass = t.good > 0 && t.bad == 0 && lost == 0 && t.lockSec < 0.5 * dur;
    t.bad += lost;
    return t;
}

static Trial runT2(const Opt& o, double cn, uint32_t seed) {
    TxParams tp;
    tp.s1 = o.shortFr ? 3 : 0; tp.s2field1 = o.a; tp.giIdx = o.b; tp.pp = o.c; tp.ext = o.ext;
    tp.payload = true; tp.plpShort = o.shortFr; tp.plpMod = o.d; tp.plpCod = o.e; tp.plpRot = o.rot; tp.plpTi = 3;
    T2Generator gen(tp);
    const double fn = nativeRateHz(8);
    const int nFrames = o.frames ? o.frames : 12;
    std::map<int, std::vector<std::vector<uint8_t>>> sent;
    std::vector<cf32> x, fr;
    std::vector<size_t> frameEnd;
    for (int i = 0; i < nFrames; i++) { gen.nextFrame(fr); sent[i & 0xff] = gen.lastBbFrames(); x.insert(x.end(), fr.begin(), fr.end()); frameEnd.push_back(x.size()); }
    const double frameSec = (double)frameEnd[0] / fn;
    std::mt19937 lr(seed * 31 + 1);
    const size_t lead = 20000 + lr() % 30000;
    impairAndNoise(x, o, fn, gen.fftN(), gen.carriers(), gen.guard(), cn, seed, lead);
    T2Receiver rx;
    rx.configure(fn, 8);
    std::mutex mu;
    std::vector<PlpResult> res;
    rx.setPlpCallback([&](const PlpResult& r) { std::lock_guard<std::mutex> lk(mu); res.push_back(r); });
    RxTelemetry tel; uint64_t sq = 0;
    for (size_t i = 0; i < x.size(); i += 1 << 15) {
        rx.feed(x.data() + i, std::min<size_t>(1 << 15, x.size() - i));
        rx.telemetry(tel, sq);
        // keep the data decoder (a thread that drops what it is still busy for) within a few frames of the input
        for (int w = 0; w < 300; w++) {
            size_t n; { std::lock_guard<std::mutex> lk(mu); n = res.size(); }
            const double fedFrames = (double)(i > lead ? i - lead : 0) / (double)frameEnd[0];
            if ((double)n + 3 >= fedFrames || tel.l1postGood == 0) break;   // (nothing to wait for while the L1 does not decode)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            rx.telemetry(tel, sq);
        }
    }
    for (int i = 0, still = 0; i < 1000 && still < 100; i++) {
        size_t n0; { std::lock_guard<std::mutex> lk(mu); n0 = res.size(); }
        rx.telemetry(tel, sq);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        size_t n1; { std::lock_guard<std::mutex> lk(mu); n1 = res.size(); }
        still = n1 == n0 ? still + 1 : 0;
    }
    if (getenv("SENS_VERBOSE")) {
        int blocks = 0, ok = 0; double mer = 0, pre = 0; int nm = 0;
        { std::lock_guard<std::mutex> lk(mu); for (auto& r : res) { blocks += r.blocks; ok += r.blocksOk; if (r.blocksOk) { mer += r.merDb; pre += r.preBer; nm++; } fprintf(stderr, " [f%d %d/%d]", r.t2Frame, r.blocksOk, r.blocks); } fprintf(stderr, "\n"); }
        if (nm) fprintf(stderr, "  T2 data MER %.2f dB, pre-LDPC BER %.4f\n", mer / nm, pre / nm);
        fprintf(stderr, "  T2 state %d L1-pre %llu/%llu L1-post %llu/%llu results %zu blocks %d ok %d snr %.1f\n", tel.state, (unsigned long long)tel.l1preGood,
                (unsigned long long)(tel.l1preGood + tel.l1preBad), (unsigned long long)tel.l1postGood, (unsigned long long)(tel.l1postGood + tel.l1postBad), res.size(), blocks, ok, tel.dataSnrDb);
    }
    Trial t;
    int first = -1; int lastOk = -1;
    std::map<int, bool> okFrame;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& r : res) {
            auto it = sent.find(r.t2Frame);
            if (it == sent.end() || r.blocks == 0) continue;
            bool all = (int)r.frames.size() >= (int)it->second.size();
            for (size_t b = 0; b < it->second.size() && all; b++) all = b < r.frames.size() && r.frames[b].bits == it->second[b];
            okFrame[r.t2Frame] = all;
            if (all) { t.good++; if (first < 0 || r.t2Frame < first) first = r.t2Frame; lastOk = std::max(lastOk, r.t2Frame); }
        }
    }
    if (first >= 0) {
        t.lockSec = (first + 1) * frameSec;
        for (int f = first + 1; f < nFrames - 1; f++) if (!okFrame.count(f) || !okFrame[f]) t.bad++;   // the last frame may still be in flight
    }
    t.pass = first >= 0 && first < nFrames / 2 && t.bad == 0;
    return t;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: see the head of tests/dvbt/sens_dvb.cpp\n"); return 2; }
    Opt o;
    o.t2 = std::string(argv[1]) == "t2";
    int ai = 2;
    int* pos[] = {&o.a, &o.b, &o.c, &o.d, &o.e};
    const int npos = o.t2 ? 5 : 4;
    for (int i = 0; i < npos && ai < argc && argv[ai][0] != '-'; i++) *pos[i] = atoi(argv[ai++]);
    for (; ai < argc; ai++) {
        const std::string a = argv[ai];
        auto nx = [&]() { return ai + 1 < argc ? argv[++ai] : (char*)"0"; };
        if (a == "--snr") { sscanf(nx(), "%lf:%lf:%lf", &o.s0, &o.s1, &o.ss); }
        else if (a == "--trials") o.trials = atoi(nx());
        else if (a == "--frames") o.frames = atoi(nx());
        else if (a == "--chan") o.chan = nx();
        else if (a == "--doppler") o.doppler = atof(nx());
        else if (a == "--cfo") o.cfo = atof(nx());
        else if (a == "--ppm") o.ppm = atof(nx());
        else if (a == "--threads") o.threads = atoi(nx());
        else if (a == "--short") o.shortFr = true;
        else if (a == "--norot") o.rot = false;
        else if (a == "--ext") o.ext = true;
    }
    double thr = NAN;
    std::vector<double> allLock;
    for (double cn = o.s0; cn <= o.s1 + 1e-9; cn += o.ss) {
        std::vector<Trial> tr((size_t)o.trials);
        std::atomic<int> next{0};
        std::vector<std::thread> th;
        for (int w = 0; w < std::max(1, o.threads); w++) th.emplace_back([&]() {
            for (int i; (i = next++) < o.trials;) tr[(size_t)i] = o.t2 ? runT2(o, cn, 1000 + i) : runT(o, cn, 1000 + i);
        });
        for (auto& t : th) t.join();
        int pass = 0; std::vector<double> lk;
        size_t good = 0, bad = 0;
        for (auto& t : tr) { pass += t.pass; if (t.lockSec >= 0) lk.push_back(t.lockSec); good += t.good; bad += t.bad; }
        std::sort(lk.begin(), lk.end());
        const double med = lk.empty() ? -1 : lk[lk.size() / 2], p90 = lk.empty() ? -1 : lk[std::min(lk.size() - 1, (size_t)std::ceil(0.9 * lk.size()) - 1)];
        printf("C/N %5.2f dB: pass %d/%d  lock median %.3f s p90 %.3f s  good %zu bad %zu\n", cn, pass, o.trials, med, p90, good, bad);
        fflush(stdout);
        if (std::isnan(thr) && pass * 10 >= 9 * o.trials) { thr = cn; allLock = lk; }
    }
    if (!std::isnan(thr)) printf("THRESHOLD %.2f dB  lock median %.3f s\n", thr, allLock.empty() ? -1 : allLock[allLock.size() / 2]);
    else printf("THRESHOLD none in range\n");
    return 0;
}
