// All-modes QA sweep (3.9, docs/QA-SWEEP-3.9.md): every mode that has a built-in test signal goes generator -> file -> Engine -> decoder under
// five conditions, and the mode's own checked output is counted:
//   clean   the generator's signal as it comes (generator SNR 60 dB, 16-bit file)
//   pos     carrier +2 kHz and the sample clock +50 ppm (a cheap radio's crystal)
//   neg     carrier -2 kHz and the sample clock -50 ppm
//   snr     the clean signal with white noise at --snr dB (default 12) inside the signal's own 99 % band, measured over its active part
//   noise   the same noise with no signal: nothing may decode (false-decode check)
//   tx      the mode's own transmitter (TX registry: generator, output stage, limiter) instead of the test signal, at its rate
// "Decoded" is the receiver's count of blocks that passed the protocol's own check (CRC, FEC, parity: Engine telemetry blocksOk; DVB-T and
// ISDB-T: RS-clean + corrected packets; FM: RDS groups), so a decoded block carries the bits the generator sent. A mode that counts no blocks
// (text and slot decoders: RTTY, FT8, NAVTEX ...) is measured by what it saves (Engine::saveResults) beyond what it saves for silence (a
// sixth file, the baseline). A mode that only measures or locks (spectrum tools, audio) passes on its lock flag (dataValid). After the clean
// run the same engine is retuned away and back, stopped, started again on the file and stopped again, each step under a watchdog.
// Lengths: 3 s, 8 s (also when the clean run had fewer than 8 blocks), 32 s, then 32 s played at the radio's speed (decoders on their own threads skip slots when a file runs faster than real
// time); ONAIR_TEST_LONG=1 or --long: 8, 32, 120 s, then 32 s at real time.
// CPU: CPU seconds (all threads) the receiver used per second of signal in the clean run, steady state, as bench_modes measures it.
//
// Every mode runs in child processes (a crash or hang marks only that row). The ctest entries qa_sweep_1..N each take one shard of the modes.
// Known failures with a reason live in qa_known.inc; anything else that fails, fails the test. POSIX only (fork, getrusage).
//
//   test_qa_sweep [--long] [--shard k/N] [--mode id[,id..]] [--seconds S] [--snr dB] [--out prefix] [--list] [--strict]
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_registry.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/source/source.h"
#include "dect2/tx/tx.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace dect2;
using Clock = std::chrono::steady_clock;

namespace {

const char* const kConds[] = {"clean", "pos", "neg", "snr", "noise"};
constexpr int kNC = 5;

// ------------------------------------------------------------------ known failures: "id/cond" -> reason (the table in the doc says the same)
struct Known { const char* key; const char* why; };
const Known kKnown[] = {
#include "qa_known.inc"
    {nullptr, nullptr}
};
const char* knownWhy(const std::string& id, const char* cond) {
    const std::string k1 = id + "/" + cond, k2 = id + "/*";
    for (const Known* k = kKnown; k->key; k++) if (k1 == k->key || k2 == k->key) return k->why;
    return nullptr;
}

// ------------------------------------------------------------------ the mode list (as bench_modes)
struct Entry {
    std::string id, name;
    int stdMode = 0, synthMode = 0, kind = 0;   // kind: 0 table, 1 t2, 2 dvbt, 3 atsc, 4 dab, 5 atsc3, 6 isdbt, 7 fm
    const SubMode* sub = nullptr;
    const ModeTuning* mt = nullptr;
    double defMhz = 100, bwMhz = 1, rate = 2e6, offsetHz = 0;
};

std::vector<Entry> allEntries() {
    std::vector<Entry> v;
    auto core = [&](const char* id, const char* name, int std_, int kind, double bw, double rate, double mhz) {
        Entry e; e.id = id; e.name = name; e.stdMode = std_; e.synthMode = std_ >= 4 ? std_ : 0; e.kind = kind; e.bwMhz = bw; e.rate = rate; e.defMhz = mhz;
        v.push_back(e);
    };
    core("t2", "DVB-T2", 1, 1, 8, 0, 522);
    core("dvbt", "DVB-T", 2, 2, 8, 0, 522);
    core("atsc", "ATSC 1.0", 3, 3, 6, 8e6, 600);
    core("dab", "DAB", 4, 4, 8, 10e6, 225);
    core("atsc3", "ATSC 3.0", 5, 5, 8, 10e6, 600);
    core("isdbt", "ISDB-T", 6, 6, 6, 8e6, 600);
    core("fm", "FM", 7, 7, 0.25, 4e6, 93.9);
    std::set<std::string> seen;
    for (const ModeTuning& m : modeTunings()) {
        if (m.stdMode < 8 || seen.count(m.id)) continue;
        seen.insert(m.id);
        Entry e; e.id = m.id; e.name = m.name; e.stdMode = m.stdMode; e.synthMode = m.stdMode; e.mt = &m;
        e.defMhz = m.defMhz; e.bwMhz = m.bandwidthMhz; e.rate = m.sampleRate; e.offsetHz = m.tuneOffsetHz;
        v.push_back(e);
    }
    int n = 0;
    const SubMode* sm = subModes(n);
    for (int i = 0; i < n; i++) {
        const ModeTuning* m = modeTuning(sm[i].stdMode);
        if (!m || seen.count(sm[i].id)) continue;
        seen.insert(sm[i].id);
        Entry e; e.id = sm[i].id; e.name = sm[i].name; e.stdMode = sm[i].stdMode; e.synthMode = sm[i].stdMode; e.sub = &sm[i]; e.mt = m;
        e.defMhz = sm[i].defMhz; e.bwMhz = m->bandwidthMhz; e.rate = m->sampleRate; e.offsetHz = m->tuneOffsetHz;
        v.push_back(e);
    }
    return v;
}

void tuneFor(const Entry& e, double rate, TuneSettings& t) {
    t.bandwidthMhz = e.bwMhz;
    t.sampleRate = rate;
    t.centerHz = e.defMhz * 1e6;
    t.synth.mode = e.synthMode;
    t.synth.snrDb = 60;
    if (e.kind == 2) t.synth.dvbt = true;
    if (e.kind == 3) t.synth.atsc = true;
    if (e.kind == 6) { t.isdbtBwMhz = 6; t.synth.isdbtBwMhz = 6; }
    if (e.sub) applySubModeSynth(t.synth, *e.sub);
}

void muteAndConfigure(Engine& e, const Entry& en) {
    e.setComputeMode(0);
    e.fm().setSilent(true); e.vor().setSilent(true); e.m17().setSilent(true); e.cw().setSilent(true);
    e.psk().setListen(false); e.js8().setListen(false); e.ale().setListen(false); e.hfdig().setListen(false);
    e.specx().setSilent(true); e.uplink().setSilent(true); e.specan().setSilent(true); e.astro().setSilent(true); e.iono().setSilent(true);
    e.qo100().setSilent(true); e.general().setSilent(true); e.hfbeacon().setSilent(true); e.same().setSilent(true);
    e.dstar().setSilent(true); e.nxdn().setSilent(true); e.dpmr().setSilent(true); e.p25().setSilent(true); e.mac().setSilent(true);
    e.setWideMuted(true);
    e.setSpectrumEnabled(false);
    e.setStandard(en.stdMode);
    if (en.sub) applySubMode(e, en.sub);
    if (en.stdMode == 22) e.mesh().setTunedHz(en.defMhz * 1e6);
}

double cpuSeconds() {
    rusage u;
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1e-6;
}

// the receiver's count of checked blocks (see the top of the file)
uint64_t goodOf(const RxTelemetry& r) {
    switch (r.standard) {
    case 1: return r.dvbt.rsClean + r.dvbt.rsCorrected;
    case 5: { uint64_t s = 0; for (auto& l : r.isdbt.layer) s += l.rsClean + l.rsCorrected; return s; }
    case 6: return r.fm.rdsGroups;
    case 26: {   // HF digital: each decoder keeps its own count (keyboard modes and RTTY: characters, Hellschreiber: columns; FT8 & co: decodes; SSTV: picture rows; FreeDV: speech frames with sync)
        const auto& h = r.hfdig;
        return std::max<uint64_t>({r.blocksOk, h.kbd.chars, h.rtty.chars, (uint64_t)h.ftx.decodes.size(), (uint64_t)std::max(0, h.sstv.lines), h.freedv.speechFrames, (uint64_t)std::max(0, h.kbd.hellCols)});
    }
    default: return r.blocksOk;
    }
}
bool lockOf(const RxTelemetry& r) {
    switch (r.standard) {
    case 1: return r.dvbt.tpsOk;
    case 5: return r.isdbt.tmccOk;
    case 6: return r.fm.carrier || r.fm.rdsSync;
    default: return r.dataValid;
    }
}

// ------------------------------------------------------------------ stage 1: the generator once, five files from it
struct Resampler {   // streaming 4-point Lagrange: output n is input position n * step
    double step = 1, pos = 1;
    std::vector<cf32> in;
    void push(const cf32* x, size_t n, std::vector<cf32>& out) {
        in.insert(in.end(), x, x + n);
        while (pos + 2 < (double)in.size()) {
            const size_t i0 = (size_t)pos;
            const float f = (float)(pos - i0);
            const cf32 a = in[i0 - 1], b = in[i0], c = in[i0 + 1], d = in[i0 + 2];
            const float w0 = -f * (f - 1) * (f - 2) / 6, w1 = (f + 1) * (f - 1) * (f - 2) / 2, w2 = -(f + 1) * f * (f - 2) / 2, w3 = (f + 1) * f * (f - 1) / 6;
            out.push_back(a * w0 + b * w1 + c * w2 + d * w3);
            pos += step;
        }
        const size_t drop = (size_t)pos > 2 ? (size_t)pos - 2 : 0;
        if (drop > 0) { in.erase(in.begin(), in.begin() + (long)drop); pos -= (double)drop; }
    }
};

struct Writer {
    FILE* f = nullptr;
    std::vector<int16_t> buf;
    float gain = 1;
    uint64_t n = 0, clipped = 0;
    void put(const std::vector<cf32>& v) {
        buf.resize(v.size() * 2);
        for (size_t i = 0; i < v.size(); i++) {
            float re = v[i].real() * gain * 32767.f, im = v[i].imag() * gain * 32767.f;
            if (std::fabs(re) > 32767.f || std::fabs(im) > 32767.f) clipped++;
            buf[2 * i] = (int16_t)std::lround(std::min(32767.f, std::max(-32767.f, re)));
            buf[2 * i + 1] = (int16_t)std::lround(std::min(32767.f, std::max(-32767.f, im)));
        }
        fwrite(buf.data(), 2, buf.size(), f);
        n += v.size();
    }
};

// the transmitter of the mode, if it has one (the TX registry's ids are the mode ids, DVB-T2 is "dvbt2")
const TxModeSpec* txFor(const Entry& en) {
    if (en.id == "t2") return txModeById("dvbt2");
    return txModeById(en.id);
}

// the transmitter's own samples (generator, output stage, limiter, as a TX session makes them) into tx.ci16, at the rate it asks for, with
// the channel moved to the centre as a recording has it. Returns the rate, 0 when there is no transmitter
double writeTx(const Entry& en, double secs, const std::string& dir) {
    const TxModeSpec* m = txFor(en);
    if (!m) return 0;
    TxRequest r;
    r.modeId = m->id; r.callsign = "N0ABC"; r.grid = "KO24"; r.text = "CQ TEST"; r.name = "TEST";
    const double rate = txRequestRate(*m, r, {});
    std::string err;
    auto g = rate > 0 ? makeTxGenerator(*m, r, rate, err) : nullptr;
    if (!g) { fprintf(stderr, "tx %s: %s\n", m->id, err.c_str()); return 0; }
    Writer w, wz;   // the transmitter's signal, and silence of the same length and rate (the baseline of its saved results)
    w.f = fopen((dir + "/tx.ci16").c_str(), "wb");
    wz.f = fopen((dir + "/txzero.ci16").c_str(), "wb");
    if (!w.f || !wz.f) return 0;
    w.gain = 0.5f;
    std::vector<cf32> z;
    const ModeTuning* mt = modeTuning(m->stdMode);
    const double dph = 2 * M_PI * (mt ? mt->tuneOffsetHz : 0.0) / rate;
    double ph = 0;
    const size_t n = (size_t)(secs * rate);
    std::vector<cf32> x;
    for (size_t made = 0; made < n;) {
        const size_t k = std::min<size_t>(1 << 15, n - made);
        x.assign(k, cf32(0, 0));
        if (!g->finished()) { g->generate(x.data(), k); txOutputStage(r, x.data(), k, made, rate); txSoftLimit(x.data(), k); }
        if (dph != 0) for (auto& v : x) { v *= cf32((float)cos(ph), (float)sin(ph)); ph += dph; if (ph > M_PI) ph -= 2 * M_PI; if (ph < -M_PI) ph += 2 * M_PI; }
        w.put(x);
        z.assign(k, cf32(0, 0)); wz.put(z);
        made += k;
    }
    fclose(w.f); fclose(wz.f);
    return rate;
}

int genChild(const Entry& en, double rate, double secs, double snrDb, const std::string& dir) {
    setenv("DECT2_NOPACE", "1", 1);
    TuneSettings t;
    tuneFor(en, rate, t);
    t.centerHz = (en.defMhz * 1e6) + en.offsetHz;
    if (en.kind == 0 && !makeModeSynth(en.synthMode, t.synth, rate)) {   // no test signal: the transmitter alone, if there is one
        const double txRate = writeTx(en, secs, dir);
        printf("NOGEN TXRATE %.4f\n", txRate);
        return 0;
    }
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "synthetic";
    auto src = makeSource(dev);
    IqRing ring(1u << 22);
    std::string err;
    if (!src || !src->start(t, ring, err)) { fprintf(stderr, "synthetic source: %s\n", err.c_str()); return 3; }
    const double r = src->sampleRate();
    const uint64_t total = (uint64_t)(r * secs);
    const double dph = en.offsetHz != 0 && en.kind == 0 ? (getenv("QA_MIX") ? atof(getenv("QA_MIX")) : 1.0) * 2 * M_PI * en.offsetHz / r : 0;   // as the engine mixes out the tune offset of a live radio (the test signal sits at -offset)
    double ph = 0;
    std::vector<cf32> all;   // the first stretch, kept to measure the signal before anything is written
    const uint64_t warmN = std::min<uint64_t>(total, std::max<uint64_t>((uint64_t)(r * 1.0), 1u << 18));
    std::vector<cf32> buf(1 << 16);
    auto lastProgress = Clock::now();
    uint64_t done = 0;
    auto readSome = [&](size_t want, std::vector<cf32>& out) -> bool {
        out.clear();
        while (out.size() < want) {
            const size_t n = ring.read(buf.data(), std::min(buf.size(), want - out.size()));
            if (!n) {
                if (Clock::now() - lastProgress > std::chrono::seconds(60)) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            lastProgress = Clock::now();
            for (size_t i = 0; i < n; i++) {
                cf32 s = buf[i];
                if (dph != 0) { s *= cf32((float)cos(ph), (float)sin(ph)); ph += dph; if (ph < -M_PI) ph += 2 * M_PI; }
                out.push_back(s);
            }
        }
        return true;
    };
    if (!readSome((size_t)warmN, all)) { fprintf(stderr, "no samples for 60 s\n"); return 5; }
    done = all.size();
    // the signal: power over its active part (1 ms blocks above a tenth of the strongest) and its 99 % band from the active blocks' spectrum
    const size_t blk = std::max<size_t>(64, (size_t)(r / 1000));
    std::vector<double> bp;
    for (size_t i = 0; i + blk <= all.size(); i += blk) { double p = 0; for (size_t k = 0; k < blk; k++) p += std::norm(all[i + k]); bp.push_back(p / blk); }
    const double maxP = bp.empty() ? 0 : *std::max_element(bp.begin(), bp.end());
    double psig = 0; int act = 0;
    for (double p : bp) if (p > 0.1 * maxP) { psig += p; act++; }
    psig = act ? psig / act : 0;
    const int nfft = 4096;
    std::vector<double> psd(nfft, 0.0);
    Fft fft(nfft);
    std::vector<cf32> fb(nfft);
    int segs = 0;
    for (size_t i = 0; i + nfft <= all.size() && segs < 400; i += nfft) {
        double p = 0; for (int k = 0; k < nfft; k++) p += std::norm(all[i + k]);
        if (p / nfft < 0.1 * maxP) continue;
        for (int k = 0; k < nfft; k++) { const float w = 0.5f - 0.5f * (float)cos(2 * M_PI * k / nfft); fb[k] = all[i + k] * w; }
        fft.forward(fb.data());
        for (int k = 0; k < nfft; k++) psd[k] += std::norm(fb[k]);
        segs++;
    }
    double obw = r;
    {
        std::vector<double> s(nfft);   // fftshifted
        double tot = 0;
        for (int k = 0; k < nfft; k++) { s[k] = psd[(k + nfft / 2) % nfft]; tot += s[k]; }
        if (tot > 0) {
            double acc = 0; int lo = 0, hi = nfft - 1;
            for (lo = 0; lo < nfft; lo++) { acc += s[lo]; if (acc > 0.005 * tot) break; }
            acc = 0;
            for (hi = nfft - 1; hi > 0; hi--) { acc += s[hi]; if (acc > 0.005 * tot) break; }
            obw = std::max(r / nfft, (hi - lo + 1) * r / nfft);
        }
    }
    // noise for the snr / noise files: total power = psig / snr * (rate / obw)
    const double pn = psig > 0 ? psig / std::pow(10.0, snrDb / 10) * (r / obw) : 1e-4;
    const float sigma = (float)std::sqrt(pn / 2);
    const float clean0 = 1.0f;
    const float g = (float)std::min(1.0, 0.25 / std::sqrt(std::max(1e-12, psig + pn)));   // the noisy files keep a total rms of 0.25 at most
    Writer w[kNC + 1];   // the five conditions and silence (the baseline of the saved results)
    for (int c = 0; c <= kNC; c++) {
        w[c].f = fopen((dir + "/" + (c < kNC ? kConds[c] : "zero") + ".ci16").c_str(), "wb");
        if (!w[c].f) return 4;
        w[c].gain = c >= 3 ? g : clean0;
    }
    Resampler rp, rn; rp.step = 1.0 + 50e-6; rn.step = 1.0 - 50e-6;
    double php = 0, phn = 0;
    const double dcp = 2 * M_PI * 2000.0 / r;
    genutil::NoiseSource ns1(7), ns2(11);
    std::vector<cf32> o, tmp;
    auto emit = [&](const std::vector<cf32>& x) {
        w[0].put(x);
        tmp.clear(); rp.push(x.data(), x.size(), tmp);
        for (auto& s : tmp) { s *= cf32((float)cos(php), (float)sin(php)); php += dcp; if (php > M_PI) php -= 2 * M_PI; }
        w[1].put(tmp);
        tmp.clear(); rn.push(x.data(), x.size(), tmp);
        for (auto& s : tmp) { s *= cf32((float)cos(phn), (float)sin(phn)); phn -= dcp; if (phn < -M_PI) phn += 2 * M_PI; }
        w[2].put(tmp);
        o = x; ns1.add(o.data(), o.size(), sigma); w[3].put(o);
        o.assign(x.size(), cf32(0, 0)); ns2.add(o.data(), o.size(), sigma); w[4].put(o);
        o.assign(x.size(), cf32(0, 0)); w[5].put(o);
    };
    emit(all);
    all.clear(); all.shrink_to_fit();
    std::vector<cf32> chunk;
    while (done < total) {
        if (!readSome((size_t)std::min<uint64_t>(1u << 18, total - done), chunk)) { fprintf(stderr, "no samples for 60 s\n"); return 5; }
        done += chunk.size();
        emit(chunk);
    }
    for (auto& x : w) fclose(x.f);
    src->stop();
    const double txRate = writeTx(en, secs, dir);
    printf("RATE %.6f PSIG %.6g OBW %.1f ACTIVE %.3f CLIP %llu TXRATE %.4f\n", r, psig, obw, bp.empty() ? 0.0 : (double)act / bp.size(), (unsigned long long)(w[0].clipped + w[3].clipped), txRate);
    fflush(stdout);
    _exit(0);
}

// ------------------------------------------------------------------ stage 2: one file through the Engine
[[noreturn]] void hangExit(const char* stage) { printf("HANG %s\n", stage); fflush(stdout); _exit(42); }

struct Watchdog {   // _exit(42) when a step takes longer than its limit
    std::atomic<int> gen{0};
    std::atomic<bool> quit{false};
    std::atomic<const char*> stage{""};
    std::atomic<double> deadline{0};
    std::thread th;
    Watchdog() {
        th = std::thread([this] {
            while (!quit) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                const double d = deadline.load();
                if (d > 0 && std::chrono::duration<double>(Clock::now().time_since_epoch()).count() > d) hangExit(stage.load());
            }
        });
    }
    void arm(const char* s, double secs) { stage = s; deadline = std::chrono::duration<double>(Clock::now().time_since_epoch()).count() + secs; }
    void disarm() { deadline = 0; }
    ~Watchdog() { quit = true; th.join(); }
};

int runChild(const Entry& en, double rate, double secs, const std::string& path, bool lifecycle) {
    if (!getenv("QA_PACE")) setenv("DECT2_NOPACE", "1", 1);
    TuneSettings t;
    tuneFor(en, rate, t);
    const bool direct = getenv("QA_DIRECT") != nullptr;   // debugging: the engine on the synthetic source itself, no file
    if (!direct) t.synth = SynthConfig();   // the engine reads a file: no generator
    else t.centerHz += en.offsetHz;
    Watchdog wd;
    Engine e;
    muteAndConfigure(e, en);
    DeviceInfo dev; dev.kind = direct ? DeviceInfo::Synthetic : DeviceInfo::File; dev.name = direct ? "synthetic" : path;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CI16; fo.sampleRate = rate; fo.loop = false;
    const double cpu0 = cpuSeconds();
    wd.arm("start", 60);
    if (!e.start(dev, t, fo)) {
        size_t n; std::string last; for (auto& l : e.logSnapshot(n)) last = l;
        printf("STARTFAIL %s\n", last.c_str());
        fflush(stdout);
        _exit(3);
    }
    wd.disarm();
    auto processed = [&] {
        double rx = 0, sig = 0;
        const std::string lp = e.loadProfile();
        const char* p = strstr(lp.c_str(), "receiver ");
        if (p && sscanf(p, "receiver %lf s for %lf s", &rx, &sig) == 2) return sig;
        return 0.0;
    };
    struct Pt { double cpu, proc; };
    std::vector<Pt> pts;
    double proc = 0, lastProc = 0, firstGood = -1;
    uint64_t good = 0, bad = 0, seq = 0;
    bool locked = false;
    int standard = -1;
    RxTelemetry rx;
    auto tStart = Clock::now(), lastMove = tStart;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        proc = processed();
        pts.push_back({cpuSeconds() - cpu0, proc});
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            standard = rx.standard;
            const uint64_t g = goodOf(rx);
            if (g > good) { good = g; if (firstGood < 0) firstGood = proc; }
            bad = std::max<uint64_t>(bad, rx.blocksBad);
            locked = locked || lockOf(rx);
        }
        if (proc > lastProc + 1e-6) { lastProc = proc; lastMove = Clock::now(); }
        if (proc >= secs - 0.05) break;
        if (Clock::now() - lastMove > std::chrono::seconds(5) && proc >= 0.9 * secs) break;   // the file ended a block short
        if (Clock::now() - lastMove > std::chrono::seconds(90)) break;                         // stuck: report what there is
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // the last report
    if (e.latestRx(rx, seq)) { standard = rx.standard; const uint64_t g = goodOf(rx); if (g > good) { good = g; if (firstGood < 0) firstGood = proc; } locked = locked || lockOf(rx); bad = std::max<uint64_t>(bad, rx.blocksBad); }
    if (getenv("QA_SWEEP_VERBOSE")) {   // the engine's log and the last report, for a mode that does not decode
        size_t n = 0;
        for (auto& l : e.logSnapshot(n)) fprintf(stderr, "  log: %s\n", l.c_str());
        fprintf(stderr, "  last report: std %d state %d valid %d ok %llu bad %llu cfo %.1f snr %.1f processed %.2f s\n", rx.standard, rx.state, rx.dataValid ? 1 : 0,
                (unsigned long long)rx.blocksOk, (unsigned long long)rx.blocksBad, rx.cfoHz, rx.dataSnrDb, proc);
        if (rx.standard == 26) fprintf(stderr, "  hfdig: rtty chars %llu text %zu, ftx decodes %zu, audio %.1f dB, level %.1f dB\n", (unsigned long long)rx.hfdig.rtty.chars, rx.hfdig.rtty.text.size(), rx.hfdig.ftx.decodes.size(), rx.hfdig.audioDb, rx.hfdig.levelDb);
    }
    // what the receiver saved (its decoded lists, text, pictures): the measure of the modes that count no blocks
    uint64_t resBytes = 0;
    {
        char rdir[] = "/tmp/qa_sweep_res_XXXXXX";
        if (mkdtemp(rdir)) {
            e.saveResults(rdir);
            std::error_code ec;
            for (auto& f : std::filesystem::recursive_directory_iterator(rdir, ec)) if (f.is_regular_file(ec)) resBytes += f.file_size(ec);
            std::filesystem::remove_all(rdir, ec);
        }
    }
    const double cpuEnd = cpuSeconds() - cpu0;
    const double wall = std::chrono::duration<double>(Clock::now() - tStart).count();
    double cost = proc > 0 ? cpuEnd / proc : 0;
    if (secs >= 4 && proc > 0) {
        for (auto& p : pts) if (p.proc >= 0.25 * proc) {
            if (proc - p.proc > 0.1) cost = (cpuEnd - p.cpu) / (proc - p.proc);
            break;
        }
    }
    std::string life = "-";
    if (lifecycle) {
        // retune away and back, stop, start again on the file, let it run, stop: each step under the watchdog
        TuneSettings t2 = t; t2.centerHz += 100e3;
        wd.arm("retune", 30); e.retune(t2); std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wd.arm("retune back", 30); e.retune(t); std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wd.arm("stop", 30); e.stop();
        wd.arm("restart", 60);
        const bool ok = e.start(dev, t, fo);
        double p2 = 0;   // the widest modes (X band, up to 300 Msps) take a few seconds to set up
        for (int i = 0; i < 100 && ok && (p2 = processed()) <= 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wd.arm("stop after restart", 30); e.stop();
        wd.disarm();
        life = !ok ? "restart refused" : p2 <= 0 ? "no samples after restart" : "ok";
    } else {
        wd.arm("stop", 30); e.stop(); wd.disarm();
    }
    printf("{\"cost\":%.6f,\"cpu_total\":%.4f,\"processed\":%.4f,\"good\":%llu,\"bad\":%llu,\"locked\":%d,\"std\":%d,\"first\":%.3f,\"wall\":%.3f,\"dropped\":%llu,\"res\":%llu,\"life\":\"%s\"}\n",
           cost, cpuEnd, proc, (unsigned long long)good, (unsigned long long)bad, locked ? 1 : 0, standard, firstGood, wall, (unsigned long long)e.droppedSamples(), (unsigned long long)resBytes, life.c_str());
    fflush(stdout);
    _exit(0);   // the engine's destructor adds nothing and some receivers are slow to tear down
}

// ------------------------------------------------------------------ parent
struct ChildResult { int status = 0; bool timedOut = false; std::string out; };

ChildResult spawn(const std::vector<std::string>& args, double cpuLimit, double wallLimit) {
    ChildResult r;
    int fd[2];
    if (pipe(fd) != 0) { r.status = -1; return r; }
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1);
        close(fd[0]); close(fd[1]);
        const int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0 && !getenv("QA_SWEEP_VERBOSE")) dup2(dn, 2);
        rlimit rl; rl.rlim_cur = (rlim_t)cpuLimit; rl.rlim_max = (rlim_t)cpuLimit + 5;
        setrlimit(RLIMIT_CPU, &rl);
        std::vector<char*> av;
        for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execv(av[0], av.data());
        _exit(127);
    }
    close(fd[1]);
    fcntl(fd[0], F_SETFL, O_NONBLOCK);
    const auto t0 = Clock::now();
    int st = 0;
    char buf[4096];
    for (;;) {
        ssize_t n;
        while ((n = read(fd[0], buf, sizeof buf)) > 0) r.out.append(buf, (size_t)n);
        const pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) break;
        if (std::chrono::duration<double>(Clock::now() - t0).count() > wallLimit) { kill(pid, SIGKILL); waitpid(pid, &st, 0); r.timedOut = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ssize_t n;
    while ((n = read(fd[0], buf, sizeof buf)) > 0) r.out.append(buf, (size_t)n);
    close(fd[0]);
    if (WIFSIGNALED(st)) r.status = 128 + WTERMSIG(st);
    else r.status = WEXITSTATUS(st);
    return r;
}

double jnum(const std::string& s, const char* key, double def = 0) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = s.find(k);
    return p == std::string::npos ? def : atof(s.c_str() + p + k.size());
}
std::string jstrv(const std::string& s, const char* key) {
    const std::string k = std::string("\"") + key + "\":\"";
    const size_t p = s.find(k);
    if (p == std::string::npos) return "";
    const size_t q = s.find('"', p + k.size());
    return s.substr(p + k.size(), q == std::string::npos ? std::string::npos : q - p - k.size());
}

struct CondRes { std::string verdict = "-"; uint64_t good = 0, bad = 0, res = 0; bool locked = false; double first = -1, proc = 0; };
struct Row {
    std::string id, name, note, life = "-";
    int stdMode = 0;
    double rate = 0, secs = 0, cost = 0, obw = 0, active = 0;
    bool gen = true, byRes = false, paced = false;   // byRes: the mode counts no blocks; its measure is what it saved beyond what it saves for silence
    uint64_t resZero = 0;
    CondRes c[kNC];
    double txRate = 0;   // the transmitter's signal through the receiver (0: the mode has no transmitter)
    CondRes tx;
    // the decoded measure of a condition: checked blocks, or saved bytes beyond silence (more than a line's worth)
    uint64_t m(int k) const { return byRes ? (c[k].res > resZero + 16 ? c[k].res - resZero : 0) : c[k].good; }
    std::vector<std::string> unexpected;
};

std::string md(const std::string& s) { std::string o; for (char c : s) { if (c == '|') o += "\\|"; else o += c; } return o; }

} // namespace

int main(int argc, char** argv) {
    std::string self = argv[0];
    { std::error_code ec; const auto c = std::filesystem::canonical(self, ec); if (!ec) self = c.string(); }
    // ctest runs the short sweep (3 s of signal, one longer retry); ONAIR_TEST_LONG=1 (or --long) runs 8 s with retries up to 120 s
    bool longRun = getenv("ONAIR_TEST_LONG") && atoi(getenv("ONAIR_TEST_LONG")) > 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--long")) longRun = true;
    double secs = longRun ? 8 : 3, snrDb = 12, maxSamples = 70e6;   // 70e6: 35 s at 2 Msps, 280 MB per file
    int shardK = 1, shardN = 1;
    std::string modes, outPrefix, stage, stId, stFile, stCond;
    double stRate = 0, stSecs = 0;
    bool list = false, strict = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--list") list = true;
        else if (a == "--long") {}
        else if (a == "--strict") strict = true;
        else if (a == "--mode") modes = next();
        else if (a == "--seconds") secs = atof(next().c_str());
        else if (a == "--snr") snrDb = atof(next().c_str());
        else if (a == "--max-samples") maxSamples = atof(next().c_str());
        else if (a == "--out") outPrefix = next();
        else if (a == "--shard") { const std::string s = next(); sscanf(s.c_str(), "%d/%d", &shardK, &shardN); }
        else if (a == "--gen") { stage = a; stId = next(); stRate = atof(next().c_str()); stSecs = atof(next().c_str()); snrDb = atof(next().c_str()); stFile = next(); }
        else if (a == "--gentx") { stage = a; stId = next(); stSecs = atof(next().c_str()); stFile = next(); }
        else if (a == "--run") { stage = a; stId = next(); stRate = atof(next().c_str()); stSecs = atof(next().c_str()); stFile = next(); stCond = next(); }
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    const std::vector<Entry> all = allEntries();
    auto find = [&](const std::string& id) -> const Entry* { for (auto& e : all) if (e.id == id) return &e; return nullptr; };
    if (!stage.empty()) {
        const Entry* en = find(stId);
        if (!en) return 2;
        if (stage == "--gentx") { printf("TXRATE %.4f\n", writeTx(*en, stSecs, stFile)); fflush(stdout); _exit(0); }
        return stage == "--gen" ? genChild(*en, stRate, stSecs, snrDb, stFile) : runChild(*en, stRate, stSecs, stFile, stCond == "clean");
    }
    std::vector<const Entry*> sel;
    if (modes.empty()) { for (size_t i = 0; i < all.size(); i++) if ((int)(i % (size_t)shardN) == shardK - 1) sel.push_back(&all[i]); }
    else {
        size_t p = 0;
        while (p < modes.size()) {
            size_t q = modes.find(',', p);
            if (q == std::string::npos) q = modes.size();
            const std::string id = modes.substr(p, q - p);
            if (const Entry* e = find(id)) sel.push_back(e); else { fprintf(stderr, "no mode '%s' (--list shows them)\n", id.c_str()); return 2; }
            p = q + 1;
        }
    }
    if (list) {
        for (auto* e : sel) printf("%-16s %-36s std %3d  %.4g Msps  %.4f MHz\n", e->id.c_str(), e->name.c_str(), e->stdMode, e->rate / 1e6, e->defMhz);
        printf("%zu modes\n", sel.size());
        return 0;
    }
    const std::string tmpDir = (std::filesystem::temp_directory_path() / ("qa_sweep_" + std::to_string(getpid()))).string();
    std::filesystem::create_directories(tmpDir);
    std::vector<Row> rows;
    int unexpected = 0;
    for (auto* e : sel) {
        Row r; r.id = e->id; r.name = e->name; r.stdMode = e->stdMode; r.rate = e->rate;
        fprintf(stderr, "[%s] ", e->id.c_str()); fflush(stderr);
        // the signal length: --seconds, longer (x4, then 120 s) when the clean run decoded nothing, within --max-samples per file
        // (32 s holds two 15 s FT8 slots and a NAVTEX message; the long run reaches the 2 min WSPR slot). The engine reads the file as fast as
        // it can; the last try plays it at the radio's speed, for the decoders that work on their own threads and skip what they cannot keep
        // up with (FT8 / WSPR slots: in the app a file plays at its own speed)
        std::vector<double> lens = longRun ? std::vector<double>{secs, secs * 4, 120, 32} : std::vector<double>{secs, 8, 32, 32};
        double actual = e->rate;
        bool done = false;
        for (size_t li = 0; li < lens.size(); li++) {
            const double want = lens[li];
            if (done) break;
            const bool paced = li + 1 == lens.size();
            if (paced) setenv("QA_PACE", "1", 1);
            r.paced = paced;
            const double rr = e->rate > 0 ? e->rate : 9.142857e6;
            double s = std::min(want, maxSamples / rr);
            if (!paced && want > secs && s <= r.secs + 0.5) continue;   // no longer run possible
            r.secs = s;
            char rs[64], ss[64], sn[32];
            snprintf(rs, sizeof rs, "%.4f", e->rate); snprintf(ss, sizeof ss, "%.3f", s); snprintf(sn, sizeof sn, "%.2f", snrDb);
            const ChildResult g = spawn({self, "--gen", e->id, rs, ss, sn, tmpDir}, 600, 900);
            if (g.out.find("NOGEN") != std::string::npos) {
                r.gen = false; r.note = "no test signal"; done = true;
                const size_t tp = g.out.find("TXRATE ");
                if (tp != std::string::npos) r.txRate = atof(g.out.c_str() + tp + 7);
                break;
            }
            const size_t rp = g.out.find("RATE ");
            if (g.status != 0 || rp == std::string::npos) { r.note = g.timedOut ? "generator hang" : "generator failed (" + std::to_string(g.status) + ")"; r.c[0].verdict = "FAIL"; done = true; break; }
            double psig = 0, obw = 0, active = 0; unsigned long long clip = 0;
            sscanf(g.out.c_str() + rp, "RATE %lf PSIG %lf OBW %lf ACTIVE %lf CLIP %llu", &actual, &psig, &obw, &active, &clip);
            r.rate = actual; r.obw = obw; r.active = active;
            { const size_t tp = g.out.find("TXRATE "); r.txRate = tp == std::string::npos ? 0 : atof(g.out.c_str() + tp + 7); }
            if (psig <= 0) r.note = "generator sends silence";
            snprintf(rs, sizeof rs, "%.4f", actual);
            {   // silence first: what the receiver saves with nothing to decode (read as fast as it goes, also on the real-time try)
                unsetenv("QA_PACE");
                const ChildResult zr = spawn({self, "--run", e->id, rs, ss, tmpDir + "/zero.ci16", "zero"}, std::max(300.0, s * 20), std::max(600.0, s * 30));
                if (paced) setenv("QA_PACE", "1", 1);
                const size_t jp = zr.out.rfind('{');
                r.resZero = jp == std::string::npos ? 0 : (uint64_t)jnum(zr.out.substr(jp), "res");
            }
            for (int c = 0; c < kNC; c++) {
                if (c == 1) r.byRes = r.c[0].good == 0 && r.c[0].res > r.resZero + 16;
                if (c > 0 && r.m(0) == 0 && !r.c[0].locked) break;   // nothing in the clean run: the others say nothing more
                const ChildResult cr = spawn({self, "--run", e->id, rs, ss, tmpDir + "/" + kConds[c] + ".ci16", kConds[c]}, std::max(300.0, s * 20), std::max(600.0, s * 30));
                CondRes& cc = r.c[c];
                const size_t jp = cr.out.rfind('{');
                const size_t hp = cr.out.find("HANG ");
                if (hp != std::string::npos) { cc.verdict = "HANG"; r.life = "hang: " + cr.out.substr(hp + 5, cr.out.find('\n', hp) - hp - 5); continue; }
                if (cr.timedOut) { cc.verdict = "HANG"; continue; }
                if (cr.status != 0 || jp == std::string::npos) { cc.verdict = cr.out.find("STARTFAIL") != std::string::npos ? "NOSTART" : "CRASH(" + std::to_string(cr.status) + ")"; continue; }
                const std::string j = cr.out.substr(jp);
                cc.good = (uint64_t)jnum(j, "good"); cc.bad = (uint64_t)jnum(j, "bad"); cc.locked = jnum(j, "locked") > 0; cc.first = jnum(j, "first", -1); cc.proc = jnum(j, "processed"); cc.res = (uint64_t)jnum(j, "res");
                if (c == 0) { r.cost = jnum(j, "cost"); r.life = jstrv(j, "life"); }
            }
            // a clean run with only a few blocks is too short to judge the other conditions by: the next length (unless this was the last)
            const bool few = !r.byRes && r.c[0].good > 0 && r.c[0].good < 8 && li + 2 < lens.size();
            // (a lock with a few blocks is not enough either: PMR446's calls end after its 3 s, so +-2 kHz was judged on calls cut by the file end)
            if ((r.c[0].good > 0 && !few) || r.byRes || (r.c[0].locked && r.c[0].good == 0) || r.c[0].verdict != "-") done = true;
            if (!done && !paced) for (auto& c : r.c) c = CondRes();   // the next try starts afresh
        }
        // the transmitter (tx.ci16 from the last generator run), with its own silence as the baseline; when it decodes nothing in a short run,
        // once more with 32 s (a keyboard mode's preamble and text take longer than a test signal's first frames)
        for (int txTry = 0; txTry < 2 && r.txRate > 0; txTry++) {
            const double s = txTry == 0 ? (r.secs > 0 ? r.secs : secs) : std::min(32.0, maxSamples / r.txRate);
            if (txTry == 1) {
                if (r.tx.verdict != "FAIL" || s <= (r.secs > 0 ? r.secs : secs) + 0.5) break;
                char ss0[64]; snprintf(ss0, sizeof ss0, "%.3f", s);
                spawn({self, "--gentx", e->id, ss0, tmpDir}, 600, 900);
            }
            char rs[64], ss[64];
            snprintf(rs, sizeof rs, "%.4f", r.txRate); snprintf(ss, sizeof ss, "%.3f", s);
            unsetenv("QA_PACE");
            const ChildResult zr = spawn({self, "--run", e->id, rs, ss, tmpDir + "/txzero.ci16", "zero"}, std::max(300.0, s * 20), std::max(600.0, s * 30));
            if (r.paced) setenv("QA_PACE", "1", 1);
            const size_t zp = zr.out.rfind('{');
            const uint64_t z = zp == std::string::npos ? 0 : (uint64_t)jnum(zr.out.substr(zp), "res");
            const ChildResult cr = spawn({self, "--run", e->id, rs, ss, tmpDir + "/tx.ci16", "tx"}, std::max(300.0, s * 20), std::max(600.0, s * 30));
            const size_t jp = cr.out.rfind('{');
            if (cr.out.find("HANG ") != std::string::npos || cr.timedOut) r.tx.verdict = "HANG";
            else if (cr.status != 0 || jp == std::string::npos) r.tx.verdict = cr.out.find("STARTFAIL") != std::string::npos ? "NOSTART" : "CRASH(" + std::to_string(cr.status) + ")";
            else {
                const std::string j = cr.out.substr(jp);
                r.tx.good = (uint64_t)jnum(j, "good"); r.tx.res = (uint64_t)jnum(j, "res"); r.tx.locked = jnum(j, "locked") > 0;
                r.tx.verdict = r.tx.good > 0 || r.tx.res > z + 16 ? "pass" : r.tx.locked ? "lock" : "FAIL";
            }
        }
        unsetenv("QA_PACE");
        for (const char* c : kConds) std::filesystem::remove(tmpDir + "/" + c + ".ci16");
        for (const char* c : {"zero", "tx", "txzero"}) std::filesystem::remove(tmpDir + "/" + std::string(c) + ".ci16");
        // verdicts
        if (r.gen && r.c[0].verdict == "-") {
            const bool blocks = r.m(0) > 0;
            for (int c = 0; c < kNC; c++) {
                CondRes& cc = r.c[c];
                if (cc.verdict != "-") continue;
                // noise: a decoded block is a false decode; a mode that only locks (audio, spectrum tools) may well lock on noise: shown, not a failure
                if (c == 4) cc.verdict = blocks ? (r.m(4) == 0 ? "pass" : "FALSE") : (cc.locked ? "lock" : "pass");
                else if (c > 0 && !blocks && !r.c[0].locked) cc.verdict = "-";
                else if (blocks) cc.verdict = r.m(c) > 0 ? (c > 0 && r.m(c) * 2 < r.m(0) ? "weak" : "pass") : "FAIL";
                else cc.verdict = cc.locked ? "lock" : "FAIL";
            }
            if (!blocks && !r.c[0].locked) r.c[0].verdict = "FAIL";
        }
        if (r.gen && r.life != "ok" && r.life != "-") r.unexpected.push_back("lifecycle: " + r.life);
        if (r.gen) for (int c = 0; c < kNC; c++) {
            const std::string& v = r.c[c].verdict;
            const bool bad = v == "FAIL" || v == "FALSE" || v == "HANG" || v == "NOSTART" || v.rfind("CRASH", 0) == 0;
            const char* why = knownWhy(r.id, kConds[c]);
            if (bad && !why) r.unexpected.push_back(std::string(kConds[c]) + " " + v);
            if (!bad && why && v != "-" && strict) r.unexpected.push_back(std::string(kConds[c]) + " passes now: drop it from qa_known.inc");
        }
        {
            const std::string& v = r.tx.verdict;
            const bool bad = v == "FAIL" || v == "HANG" || v == "NOSTART" || v.rfind("CRASH", 0) == 0;
            const char* why = knownWhy(r.id, "tx");
            if (bad && !why) r.unexpected.push_back("tx " + v);
            if (!bad && why && v != "-" && strict) r.unexpected.push_back("tx passes now: drop it from qa_known.inc");
        }
        unexpected += (int)r.unexpected.size();
        fprintf(stderr, "%s %s %s %s %s tx %s cpu %.4f %s%s\n", r.c[0].verdict.c_str(), r.c[1].verdict.c_str(), r.c[2].verdict.c_str(), r.c[3].verdict.c_str(), r.c[4].verdict.c_str(), r.tx.verdict.c_str(), r.cost,
                r.note.c_str(), r.unexpected.empty() ? "" : "  <-- UNEXPECTED");
        rows.push_back(r);
    }
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    printf("\n%-16s %4s %9s %6s | %-6s %-6s %-6s %-6s %-6s %-6s | %8s %9s %s\n", "mode", "std", "rate Msps", "secs", "clean", "pos", "neg", "snr", "noise", "tx", "cpu", "clean blk", "notes");
    for (auto& r : rows) {
        printf("%-16s %4d %9.4g %6.1f | %-6s %-6s %-6s %-6s %-6s %-6s | %8.4f %9llu %s%s", r.id.c_str(), r.stdMode, r.rate / 1e6, r.secs, r.c[0].verdict.c_str(), r.c[1].verdict.c_str(), r.c[2].verdict.c_str(),
               r.c[3].verdict.c_str(), r.c[4].verdict.c_str(), r.tx.verdict.c_str(), r.cost, (unsigned long long)r.m(0), ((r.byRes ? "(saved bytes) " : "") + std::string(r.paced ? "(real time) " : "") + r.note).c_str(), r.life != "ok" && r.life != "-" ? (" life:" + r.life).c_str() : "");
        for (auto& u : r.unexpected) printf("  [UNEXPECTED %s]", u.c_str());
        printf("\n");
    }
    if (!outPrefix.empty()) {
        if (FILE* f = fopen((outPrefix + ".tsv").c_str(), "w")) {
            fprintf(f, "id\tname\tstd\trate\tsecs\tobw\tactive\tcpu\tlife\tnote");
            for (const char* c : kConds) fprintf(f, "\t%s\t%s_good\t%s_bad\t%s_first", c, c, c, c);
            fprintf(f, "\ttx\ttx_rate\tunexpected\n");
            for (auto& r : rows) {
                fprintf(f, "%s\t%s\t%d\t%.0f\t%.2f\t%.0f\t%.3f\t%.6f\t%s\t%s", r.id.c_str(), md(r.name).c_str(), r.stdMode, r.rate, r.secs, r.obw, r.active, r.cost, r.life.c_str(), r.note.c_str());
                for (auto& c : r.c) fprintf(f, "\t%s\t%llu\t%llu\t%.2f", c.verdict.c_str(), (unsigned long long)(r.byRes ? c.res : c.good), (unsigned long long)c.bad, c.first);
                std::string u; for (auto& x : r.unexpected) u += (u.empty() ? "" : "; ") + x;
                fprintf(f, "\t%s\t%.0f\t%s\n", r.tx.verdict.c_str(), r.txRate, u.c_str());
            }
            fclose(f);
        }
    }
    printf("%zu modes, %d unexpected failures\n", rows.size(), unexpected);
    return unexpected ? 1 : 0;
}
