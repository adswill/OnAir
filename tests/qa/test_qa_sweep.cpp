// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// All-modes QA sweep (3.9, docs/QA-SWEEP-3.9.md): every mode that has a built-in test signal goes generator -> file -> Engine -> decoder under
// five conditions, and the mode's own checked output is counted:
//   clean   the generator's signal as it comes (generator SNR 60 dB, 16-bit file)
//   pos     carrier +2 kHz and the sample clock +50 ppm (a cheap radio's crystal)
//   neg     carrier -2 kHz and the sample clock -50 ppm
//   snr     the clean signal with white noise at --snr dB (default 12) inside the signal's own 99 % band, measured over its active part
//   noise   the same noise with no signal: nothing may decode (false-decode check)
//   rate    the clean signal at a non-default sample rate (1.37 x) and a custom bandwidth (1.3 x the mode's), as a user who sets both by hand
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
//   test_qa_sweep --cpu      CPU only (4.1, docs/CPU-4.1.md): each mode's test signal at its default rate, then noise alone, through the Engine;
//                            CPU s per s of signal for the whole process and for its busiest thread (steady state: after the first quarter)
//   test_qa_sweep --switch   one Engine, every mode in turn (start, 0.4 s, stop): no stale state, no hang, no thread or memory growth
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
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_info.h>
#endif
#include <dirent.h>
using namespace dect2;
using Clock = std::chrono::steady_clock;

namespace {

// "rate" is generated at its own sample rate: a second --gen run; "rpos" (+2 kHz, +50 ppm) and "rnoise" (noise only) are that run's other files
const char* const kConds[] = {"clean", "pos", "neg", "snr", "noise", "rate", "rpos", "rnoise"};
constexpr int kNC = 8, kCRate = 5;
inline bool isNoiseCond(int c) { return c == 4 || c == 7; }
static const double kRateMul = getenv("QA_RATEMUL") ? atof(getenv("QA_RATEMUL")) : 1.37, kBwMul = getenv("QA_BWMUL") ? atof(getenv("QA_BWMUL")) : 1.3;   // QA_RATEMUL / QA_BWMUL: for finding which of the two a mode fails on;   // "rate": the clean signal at a sample rate 1.37 x the default (a non-integer ratio) with a 1.3 x wider custom bandwidth

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

// CPU seconds of every live thread of this process, by thread id (with its name): the busiest thread of a run
struct ThreadCpu { std::string name; double secs = 0; };
std::map<uint64_t, ThreadCpu> threadCpus() {
    std::map<uint64_t, ThreadCpu> m;
#ifdef __APPLE__
    thread_act_array_t th; mach_msg_type_number_t nth = 0;
    if (task_threads(mach_task_self(), &th, &nth) != KERN_SUCCESS) return m;
    for (mach_msg_type_number_t i = 0; i < nth; i++) {
        thread_identifier_info_data_t id; mach_msg_type_number_t c1 = THREAD_IDENTIFIER_INFO_COUNT;
        thread_extended_info_data_t ex; mach_msg_type_number_t c2 = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &c1) == KERN_SUCCESS &&
            thread_info(th[i], THREAD_EXTENDED_INFO, (thread_info_t)&ex, &c2) == KERN_SUCCESS) {
            ThreadCpu& t = m[id.thread_id];
            t.name = ex.pth_name[0] ? ex.pth_name : "main";
            t.secs = (double)(ex.pth_user_time + ex.pth_system_time) * 1e-9;
        }
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, nth * sizeof(thread_t));
#else
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* de = readdir(d)) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
            const std::string base = std::string("/proc/self/task/") + de->d_name;
            unsigned long long ns = 0;
            if (FILE* f = fopen((base + "/schedstat").c_str(), "r")) { if (fscanf(f, "%llu", &ns) != 1) ns = 0; fclose(f); }   // time on the CPU, ns
            char nm[64] = "";
            if (FILE* f = fopen((base + "/comm").c_str(), "r")) { if (fgets(nm, sizeof nm, f)) nm[strcspn(nm, "\n")] = 0; fclose(f); }
            ThreadCpu& t = m[strtoull(de->d_name, nullptr, 10)];
            t.name = nm; t.secs = (double)ns * 1e-9;
        }
        closedir(d);
    }
#endif
    return m;
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
        if (!f) return;   // a condition that is not written (--cpu: clean and noise only)
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
    printf("TXT0 %.3f\n", std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());   // the wall clock the slot modes (JS8) start from
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
    const double dph = 2 * M_PI * txModeTuneOffsetHz(*m) / rate;   // as a TX session tunes the radio (the modes with their own frequency plan: none)
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
        {   // no test signal: the receiver still gets noise alone (fresh start, no crash, no false decode, no hang)
            const double nr = en.rate > 0 ? en.rate : 2e6;
            Writer wn; wn.f = fopen((dir + "/noise.ci16").c_str(), "wb");
            if (wn.f) {
                genutil::NoiseSource nz(5);
                std::vector<cf32> z;
                const size_t n = (size_t)(secs * nr);
                for (size_t made = 0; made < n;) {
                    const size_t k = std::min<size_t>(1 << 16, n - made);
                    z.assign(k, cf32(0, 0)); nz.add(z.data(), k, 0.1f); wn.put(z);
                    made += k;
                }
                fclose(wn.f);
            }
            printf("NOISERATE %.4f\n", nr);
        }
        printf("NOGEN TXRATE %.4f\n", txRate);
        return 0;
    }
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "synthetic";
    auto src = makeSource(dev);
    IqRing ring(1u << 22);
    printf("T0 %.3f\n", std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());   // the recording's start on the wall clock
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
        // the floor (the 10th percentile of the bins: a generator's own noise, or an 8-bit radio's dither) is not the signal: without this a
        // bursty narrow signal over a floor of 1 % of its power got a 99 % band of half the rate, and its 12 dB test was 30 dB in its own band
        std::vector<double> srt(psd);
        std::nth_element(srt.begin(), srt.begin() + nfft / 10, srt.end());
        const double floorP = srt[(size_t)(nfft / 10)];
        for (int k = 0; k < nfft; k++) { s[k] = std::max(0.0, psd[(k + nfft / 2) % nfft] - floorP); tot += s[k]; }
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
    // tools/sens/qa_sens.py (4.1): QA_SEED varies the noise of one trial; QA_IMP_NOISE=1 puts the same noise on the pos / neg files too (impairment
    // and noise together); without them the files are the same as before
    const uint32_t seedAdd = getenv("QA_SEED") ? (uint32_t)atoi(getenv("QA_SEED")) * 101u : 0u;
    const bool impNoise = getenv("QA_IMP_NOISE") != nullptr;
    Writer w[kNC + 1];   // the conditions and silence (the baseline of the saved results)
    for (int c = 0; c <= kNC; c++) {
        if (getenv("QA_CPUGEN") && c != 0 && c != 4) continue;   // --cpu: the clean file and noise alone
        if (c >= kCRate && c < kNC) continue;   // "rate" and its variants are their own generator run (below), not a resampled copy: a copy has no noise outside the old band and no image-free signal
        w[c].f = fopen((dir + "/" + (c < kNC ? kConds[c] : "zero") + ".ci16").c_str(), "wb");
        if (!w[c].f) return 4;
        w[c].gain = (c == 3 || c == 4 || (impNoise && (c == 1 || c == 2))) ? g : clean0;
    }
    Resampler rp, rn; rp.step = 1.0 + 50e-6; rn.step = 1.0 - 50e-6;
    double php = 0, phn = 0;
    const double dcp = 2 * M_PI * 2000.0 / r;
    // the neg file's carrier offset (QA_NEG_HZ, default 2 kHz): the 4.1 sensitivity driver (tools/sens/qa_sens.py) moves it off the channel centre
    const double dcn = 2 * M_PI * (getenv("QA_NEG_HZ") ? atof(getenv("QA_NEG_HZ")) : 2000.0) / r;
    genutil::NoiseSource ns1(7 + seedAdd), ns2(11 + seedAdd), ns3(13 + seedAdd), ns4(17 + seedAdd);
    std::vector<cf32> o, tmp;
    auto emit = [&](const std::vector<cf32>& x) {
        w[0].put(x);
        if (!w[1].f) { o.assign(x.size(), cf32(0, 0)); ns2.add(o.data(), o.size(), sigma); w[4].put(o); return; }
        tmp.clear(); rp.push(x.data(), x.size(), tmp);
        for (auto& s : tmp) { s *= cf32((float)cos(php), (float)sin(php)); php += dcp; if (php > M_PI) php -= 2 * M_PI; }
        if (impNoise) ns3.add(tmp.data(), tmp.size(), sigma);
        w[1].put(tmp);
        tmp.clear(); rn.push(x.data(), x.size(), tmp);
        for (auto& s : tmp) { s *= cf32((float)cos(phn), (float)sin(phn)); phn -= dcn; if (phn < -M_PI) phn += 2 * M_PI; }
        if (impNoise) ns4.add(tmp.data(), tmp.size(), sigma);
        w[2].put(tmp);
        o = x; ns1.add(o.data(), o.size(), sigma); w[3].put(o);
        o.assign(x.size(), cf32(0, 0)); ns2.add(o.data(), o.size(), sigma); w[4].put(o);
        o.assign(x.size(), cf32(0, 0)); w[kNC].put(o);
    };
    emit(all);
    all.clear(); all.shrink_to_fit();
    std::vector<cf32> chunk;
    while (done < total) {
        if (!readSome((size_t)std::min<uint64_t>(1u << 18, total - done), chunk)) { fprintf(stderr, "no samples for 60 s\n"); return 5; }
        done += chunk.size();
        emit(chunk);
    }
    for (auto& x : w) if (x.f) fclose(x.f);
    src->stop();
    const double txRate = getenv("QA_NOTX") || getenv("QA_CPUGEN") ? 0.0 : writeTx(en, secs, dir);
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

int runChild(const Entry& en, double rate, double secs, const std::string& path, bool lifecycle, bool custom = false) {
    if (!getenv("QA_PACE")) setenv("DECT2_NOPACE", "1", 1);
    TuneSettings t;
    tuneFor(en, rate, t);
    if (custom && en.kind != 1 && en.kind != 2) t.bandwidthMhz *= kBwMul;   // DVB-T / T2: the bandwidth is the channel raster (symbol rate), not a filter
    const bool direct = getenv("QA_DIRECT") != nullptr;   // debugging: the engine on the synthetic source itself, no file
    if (!direct) t.synth = SynthConfig();   // the engine reads a file: no generator
    else t.centerHz += en.offsetHz;
    Watchdog wd;
    Engine e;
    muteAndConfigure(e, en);
    // JS8 takes its 15 s slots from UTC: a file's start time, as a recording's metadata gives it (the generator and the transmitter put their
    // frames on the wall clock's slots when they made the file); without it the slots of the file and of the receiver fall anywhere
    if (en.stdMode == 39 && !direct && getenv("QA_T0")) e.js8().setStartTime(atof(getenv("QA_T0")));
    DeviceInfo dev; dev.kind = direct ? DeviceInfo::Synthetic : DeviceInfo::File; dev.name = direct ? "synthetic" : path;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CI16; fo.sampleRate = rate; fo.loop = false;
    struct TapStat { std::atomic<uint64_t> n{0}, nz{0}; std::atomic<double> p{0}; std::vector<double> psd; int segs = 0; std::vector<cf32> pend; } tap;   // QA_SWEEP_VERBOSE: what the engine took in, before any processing
    if (getenv("QA_SWEEP_VERBOSE")) e.setIqTap([&tap](const std::complex<float>* x, size_t n, double) {
        double p = 0; uint64_t nz = 0;
        for (size_t i = 0; i < n; i++) { const float m = std::norm(x[i]); p += m; if (m > 0) nz++; }
        tap.n += n; tap.nz += nz; tap.p = tap.p.load() + p;
        constexpr int nf = 4096;   // the spectrum of the first 64 segments: where the energy is
        static Fft fft(nf);
        if (tap.psd.empty()) tap.psd.assign(nf, 0.0);
        tap.pend.insert(tap.pend.end(), x, x + n);
        while (tap.pend.size() >= (size_t)nf && tap.segs < 64) {
            std::vector<cf32> fb(nf);
            for (int k = 0; k < nf; k++) fb[k] = tap.pend[(size_t)k] * (0.5f - 0.5f * (float)cos(2 * M_PI * k / nf));
            fft.forward(fb.data());
            for (int k = 0; k < nf; k++) tap.psd[(size_t)k] += std::norm(fb[(size_t)k]);
            tap.pend.erase(tap.pend.begin(), tap.pend.begin() + nf);
            tap.segs++;
        }
        if (tap.segs >= 64) tap.pend.clear();
    });
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
    // --cpu: the whole process and each thread at a quarter of the signal and at its end (the steady state between them)
    const bool cpuMode = getenv("QA_CPUMODE") != nullptr;
    std::map<uint64_t, ThreadCpu> thQ;
    double cpuQ = -1, procQ = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(cpuMode ? 5 : 20));
        proc = processed();
        pts.push_back({cpuSeconds() - cpu0, proc});
        if (cpuMode && cpuQ < 0 && proc >= 0.25 * secs) { thQ = threadCpus(); cpuQ = cpuSeconds(); procQ = proc; }
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
    if (en.stdMode == 39) { e.js8().flush(); e.js8().waitIdle(); }   // JS8 decodes its slots on its own thread: the slot in progress and the queued ones, as at the end of a recording
    if (en.stdMode == 133)   // KAIT decodes a captured burst on its own thread (several seconds): let a decode that started before the file ended finish
        for (int i = 0; i < 600 && rx.standard == 132 && rx.kait.state == 3; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (e.latestRx(rx, seq)) { seq = rx.seq; const uint64_t g = goodOf(rx); if (g > good) { good = g; if (firstGood < 0) firstGood = proc; } }
        }
    if (en.stdMode == 34)   // the cell search of the last block runs on its own thread, 1 to 5 s on a CI runner at 20 Msps: let a search that started before the file ended finish
        for (int i = 0; i < 1500 && e.cell().busy(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (e.latestRx(rx, seq)) { seq = rx.seq; const uint64_t g = goodOf(rx); if (g > good) { good = g; if (firstGood < 0) firstGood = proc; } locked = locked || lockOf(rx); }
        }
    std::string cpuJson;
    if (cpuMode) {
        const std::map<uint64_t, ThreadCpu> thE = threadCpus();
        const double cpuE = cpuSeconds(), dp = proc - procQ;
        if (cpuQ < 0 || dp < 0.05) { cpuQ = cpu0; procQ = 0; thQ.clear(); }   // too short for a steady state: the whole run
        const double d = std::max(1e-6, proc - procQ);
        std::vector<std::pair<double, std::string>> per;
        std::map<std::string, double> byName;   // threads of the same name (a pool) summed
        for (auto& [id, t] : thE) {
            auto q = thQ.find(id);
            const double c = (t.secs - (q != thQ.end() ? q->second.secs : 0)) / d;
            if (c <= 0) continue;
            per.push_back({c, t.name});
            byName[t.name] += c;
        }
        std::sort(per.rbegin(), per.rend());
        std::vector<std::pair<double, std::string>> grp;
        for (auto& [n, c] : byName) grp.push_back({c, n});
        std::sort(grp.rbegin(), grp.rend());
        std::string top;
        for (size_t i = 0; i < grp.size() && i < 4; i++) { char b[96]; snprintf(b, sizeof b, "%s%s %.3f", i ? ", " : "", grp[i].second.c_str(), grp[i].first); top += b; }
        for (char& ch : top) if (ch == '"' || ch == '\\') ch = '_';
        std::string bn = per.empty() ? "" : per[0].second;
        for (char& ch : bn) if (ch == '"' || ch == '\\') ch = '_';
        char b[640];
        snprintf(b, sizeof b, ",\"scost\":%.6f,\"tcost\":%.6f,\"tname\":\"%s\",\"top\":\"%s\",\"nthreads\":%zu", (cpuE - cpuQ) / d, per.empty() ? 0.0 : per[0].first, bn.c_str(), top.c_str(), thE.size());
        cpuJson = b;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // the last report
    if (e.latestRx(rx, seq)) { standard = rx.standard; const uint64_t g = goodOf(rx); if (g > good) { good = g; if (firstGood < 0) firstGood = proc; } locked = locked || lockOf(rx); bad = std::max<uint64_t>(bad, rx.blocksBad); }
    if (getenv("QA_SWEEP_VERBOSE")) {   // the engine's log and the last report, for a mode that does not decode
        size_t n = 0;
        for (auto& l : e.logSnapshot(n)) fprintf(stderr, "  log: %s\n", l.c_str());
        fprintf(stderr, "  last report: std %d state %d valid %d ok %llu bad %llu cfo %.1f snr %.1f processed %.2f s\n", rx.standard, rx.state, rx.dataValid ? 1 : 0,
                (unsigned long long)rx.blocksOk, (unsigned long long)rx.blocksBad, rx.cfoHz, rx.dataSnrDb, proc);
        fprintf(stderr, "  ingest: %llu samples, %.1f %% non-zero, rms %.6g\n", (unsigned long long)tap.n.load(), tap.n ? 100.0 * (double)tap.nz.load() / (double)tap.n.load() : 0.0, tap.n ? std::sqrt(tap.p.load() / (double)tap.n.load()) : 0.0);
        if (!tap.psd.empty() && tap.segs > 0) {   // the three strongest 1/64 slices of the input band
            const int nf = (int)tap.psd.size(), sl = nf / 64;
            std::vector<std::pair<double, int>> pw;
            double tot = 0;
            for (int j = 0; j < 64; j++) { double q = 0; for (int k = j * sl; k < (j + 1) * sl; k++) q += tap.psd[(size_t)k]; pw.push_back({q, j}); tot += q; }
            std::sort(pw.rbegin(), pw.rend());
            fprintf(stderr, "  input spectrum (first %d segments), strongest slices of %.0f Hz:", tap.segs, rate / 64);
            for (int q = 0; q < 3; q++) { const int j = pw[(size_t)q].second; fprintf(stderr, " %+.0f Hz %.0f %%", ((double)((j * sl + nf / 2) % nf) - nf / 2 + sl / 2.0) * rate / nf, tot > 0 ? 100.0 * pw[(size_t)q].first / tot : 0.0); }
            fprintf(stderr, "\n");
        }
        if (rx.standard == 641) fprintf(stderr, "  vdl3: syncs %llu noise %.1f dB level %.1f dB rate %.0f ok %d center %.0f now %.2f s\n", (unsigned long long)rx.vdl3.syncs, rx.vdl3.noiseDb, rx.vdl3.levelDb, rx.vdl3.inputRate, rx.vdl3.rateOk ? 1 : 0, rx.vdl3.centerHz, rx.vdl3.nowSec);
        if (rx.standard == 791) fprintf(stderr, "  ltpos: state %d pulses %llu floor %.1f dB peak %.1f dB threshold %.1f rate %.0f time %.2f s\n", rx.ltpos.state, (unsigned long long)rx.ltpos.pulses, rx.ltpos.floorDb, rx.ltpos.snrDb, rx.ltpos.thresholdDb, rx.ltpos.inputRate, rx.ltpos.timeSec);
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
    printf("{\"cost\":%.6f,\"cpu_total\":%.4f,\"processed\":%.4f,\"good\":%llu,\"bad\":%llu,\"locked\":%d,\"std\":%d,\"first\":%.3f,\"wall\":%.3f,\"dropped\":%llu,\"res\":%llu,\"life\":\"%s\"%s}\n",
           cost, cpuEnd, proc, (unsigned long long)good, (unsigned long long)bad, locked ? 1 : 0, standard, firstGood, wall, (unsigned long long)e.droppedSamples(), (unsigned long long)resBytes, life.c_str(), cpuJson.c_str());
    fflush(stdout);
    _exit(0);   // the engine's destructor adds nothing and some receivers are slow to tear down
}


// ------------------------------------------------------------------ mode switching: one Engine, every mode in turn (a user clicking through the mode list)
// Each step: setStandard / sub-mode, start on the mode's own test signal, run until 0.4 s of signal was processed, check the first reports
// are the new mode's (no state of the previous mode: standard, results), stop. Any step that hangs ends the run (watchdog). After the
// forward pass the list is run again backwards; threads and resident memory at the end of the second pass must not have grown past the end
// of the first (leaked receivers or threads that never stop).
void procStats(double& rssMb, int& threads) {
    rssMb = 0; threads = 0;
#ifdef __APPLE__
    mach_task_basic_info_data_t info; mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &cnt) == KERN_SUCCESS) rssMb = info.resident_size / 1048576.0;
    thread_act_array_t th; mach_msg_type_number_t nth = 0;
    if (task_threads(mach_task_self(), &th, &nth) == KERN_SUCCESS) {
        threads = (int)nth;
        for (mach_msg_type_number_t i = 0; i < nth; i++) mach_port_deallocate(mach_task_self(), th[i]);
        vm_deallocate(mach_task_self(), (vm_address_t)th, nth * sizeof(thread_t));
    }
#endif
    if (FILE* f = fopen("/proc/self/status", "r")) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            long v;
            if (sscanf(line, "VmRSS: %ld kB", &v) == 1) rssMb = v / 1024.0;
            if (sscanf(line, "Threads: %ld", &v) == 1) threads = (int)v;
        }
        fclose(f);
    }
}

int switchTest(const std::vector<const Entry*>& sel) {
    setenv("DECT2_NOPACE", "1", 1);
    Watchdog wd;
    Engine e;
    std::vector<const Entry*> order;
    for (auto* en : sel) {
        if (en->kind == 0) { SynthConfig sc; if (!makeModeSynth(en->synthMode, sc, en->rate > 0 ? en->rate : 2e6)) continue; }
        order.push_back(en);
    }
    int fails = 0;
    double rss1 = 0, rss2 = 0; int th1 = 0, th2 = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < order.size(); i++) {
            const Entry& en = *order[pass == 0 ? i : order.size() - 1 - i];
            const double rate = en.rate > 0 ? en.rate : 9.142857e6;
            TuneSettings t;
            tuneFor(en, rate, t);
            t.centerHz += en.offsetHz;
            muteAndConfigure(e, en);
            DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "synthetic";
            FileOptions fo;
            wd.arm(en.id.c_str(), 90);
            const auto t0 = Clock::now();
            if (!e.start(dev, t, fo)) { printf("SWITCH %s: start refused\n", en.id.c_str()); fails++; wd.disarm(); continue; }
            double proc = 0; RxTelemetry rx; uint64_t seq = 0; bool have = false;
            int firstStd = -1;
            while (Clock::now() - t0 < std::chrono::seconds(30)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                const std::string lp = e.loadProfile();
                const char* p = strstr(lp.c_str(), "receiver ");
                double rxs = 0, sig = 0;
                if (p && sscanf(p, "receiver %lf s for %lf s", &rxs, &sig) == 2) proc = sig;
                if (e.latestRx(rx, seq)) { seq = rx.seq; if (!have) firstStd = rx.standard; have = true; }
                if (proc >= 0.4) break;
            }
            if (have && firstStd != rx.standard) { printf("SWITCH %s: first report is standard %d, later ones %d (state of the previous mode)\n", en.id.c_str(), firstStd, rx.standard); fails++; }
            if (proc <= 0) { printf("SWITCH %s: no samples processed\n", en.id.c_str()); fails++; }
            e.stop();
            wd.disarm();
        }
        double rss; int th; procStats(rss, th);
        if (pass == 0) { rss1 = rss; th1 = th; } else { rss2 = rss; th2 = th; }
        printf("SWITCH pass %d done: %zu modes, rss %.0f MB, threads %d\n", pass + 1, order.size(), rss, th);
        fflush(stdout);
    }
    if (th1 > 0 && th2 > th1 + 4) { printf("SWITCH threads grew %d -> %d over the second pass\n", th1, th2); fails++; }
    if (rss1 > 0 && rss2 > rss1 * 1.25 + 200) { printf("SWITCH resident memory grew %.0f -> %.0f MB over the second pass\n", rss1, rss2); fails++; }
    printf("%zu modes switched through, %d problems\n", order.size(), fails);
    return fails ? 1 : 0;
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

// ------------------------------------------------------------------ --cpu: CPU per second of signal, every selected mode (docs/CPU-4.1.md)
// The mode's test signal at its default sample rate (60 dB, 16-bit file; a mode without a test signal: its transmitter's signal, if it has
// one), then noise alone at the same rate, each through the Engine as fast as it goes. CPU seconds per second of signal after the first
// quarter: the whole process (all threads: what the computer pays) and the busiest single thread (what one core must carry).
struct CpuRow { std::string id, name, src, tname, top, ntname; int stdMode = 0; double rate = 0, secs = 0, scost = 0, tcost = 0, nscost = -1, ntcost = -1; uint64_t good = 0; bool locked = false; std::string err; };

bool cpuRun(const std::string& self, const std::string& id, double rate, double s, const std::string& file, CpuRow& r, bool noise) {
    char rs[64], ss[64];
    snprintf(rs, sizeof rs, "%.4f", rate); snprintf(ss, sizeof ss, "%.3f", s);
    setenv("QA_CPUMODE", "1", 1);
    const ChildResult cr = spawn({self, "--run", id, rs, ss, file, noise ? "noise" : "cpu"}, std::max(300.0, s * 60), std::max(600.0, s * 60));
    unsetenv("QA_CPUMODE");
    const size_t jp = cr.out.rfind('{');
    if (cr.timedOut || cr.out.find("HANG ") != std::string::npos) { r.err += noise ? " noise:HANG" : " HANG"; return false; }
    if (cr.status != 0 || jp == std::string::npos) { r.err += (noise ? " noise:" : " ") + (cr.out.find("STARTFAIL") != std::string::npos ? std::string("NOSTART") : "CRASH(" + std::to_string(cr.status) + ")"); return false; }
    const std::string j = cr.out.substr(jp);
    if (noise) { r.nscost = jnum(j, "scost"); r.ntcost = jnum(j, "tcost"); r.ntname = jstrv(j, "tname"); }
    else { r.scost = jnum(j, "scost"); r.tcost = jnum(j, "tcost"); r.tname = jstrv(j, "tname"); r.top = jstrv(j, "top"); r.good = (uint64_t)jnum(j, "good"); r.locked = jnum(j, "locked") > 0; }
    return true;
}

int cpuSweep(const std::string& self, const std::vector<const Entry*>& sel, double secs, double maxSamples, const std::string& outPrefix) {
    const std::string tmpDir = (std::filesystem::temp_directory_path() / ("qa_cpu_" + std::to_string(getpid()))).string();
    std::filesystem::create_directories(tmpDir);
    const bool withNoise = !getenv("QA_CPU_NONOISE");
    std::vector<CpuRow> rows;
    for (auto* e : sel) {
        CpuRow r; r.id = e->id; r.name = e->name; r.stdMode = e->stdMode;
        const double rr = e->rate > 0 ? e->rate : 9.142857e6;
        const double s = std::min(secs, maxSamples / rr);
        r.secs = s;
        char rs[64], ss[64], sn[32];
        snprintf(rs, sizeof rs, "%.4f", e->rate); snprintf(ss, sizeof ss, "%.3f", s); snprintf(sn, sizeof sn, "%.2f", 12.0);
        setenv("QA_CPUGEN", "1", 1);
        const ChildResult g = spawn({self, "--gen", e->id, rs, ss, sn, tmpDir}, 900, 1200);
        unsetenv("QA_CPUGEN");
        const size_t p0 = g.out.find("T0 ");
        if (p0 != std::string::npos && (p0 == 0 || g.out[p0 - 1] == '\n')) setenv("QA_T0", std::to_string(atof(g.out.c_str() + p0 + 3)).c_str(), 1); else unsetenv("QA_T0");
        if (g.out.find("NOGEN") != std::string::npos) {
            const size_t tp = g.out.find("TXRATE "), np = g.out.find("NOISERATE ");
            const double txr = tp == std::string::npos ? 0 : atof(g.out.c_str() + tp + 7), nr = np == std::string::npos ? rr : atof(g.out.c_str() + np + 10);
            r.rate = txr > 0 ? txr : nr;
            if (txr > 0) {
                const size_t p1 = g.out.find("TXT0 ");
                if (p1 != std::string::npos) setenv("QA_T0", std::to_string(atof(g.out.c_str() + p1 + 5)).c_str(), 1);
                r.src = "tx"; cpuRun(self, e->id, txr, s, tmpDir + "/tx.ci16", r, false);
            } else r.src = "noise only";
            if (withNoise || txr <= 0) { cpuRun(self, e->id, nr, s, tmpDir + "/noise.ci16", r, true); if (txr <= 0) { r.scost = r.nscost; r.tcost = r.ntcost; r.tname = r.ntname; } }
        } else {
            const size_t rp = g.out.find("RATE ");
            if (g.status != 0 || rp == std::string::npos) { r.err = g.timedOut ? " generator hang" : " generator failed"; }
            else {
                r.rate = atof(g.out.c_str() + rp + 5);
                r.src = "test signal";
                cpuRun(self, e->id, r.rate, s, tmpDir + "/clean.ci16", r, false);
                if (withNoise) cpuRun(self, e->id, r.rate, s, tmpDir + "/noise.ci16", r, true);
            }
        }
        std::error_code ec;
        for (auto& f : std::filesystem::directory_iterator(tmpDir, ec)) std::filesystem::remove_all(f.path(), ec);
        fprintf(stderr, "[%s] %.4g Msps %.2f s: all threads %.3f, busiest %.3f (%s), noise %.3f / %.3f, blocks %llu%s\n", r.id.c_str(), r.rate / 1e6, r.secs, r.scost, r.tcost, r.tname.c_str(), r.nscost, r.ntcost,
                (unsigned long long)r.good, r.err.c_str());
        rows.push_back(r);
    }
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::vector<CpuRow> srt = rows;
    std::sort(srt.begin(), srt.end(), [](const CpuRow& a, const CpuRow& b) { return a.scost > b.scost; });
    printf("\n| mode | name | Msps | s | signal | all threads | busiest thread | noise: all | noise: busiest | blocks | threads by CPU |\n|---|---|--:|--:|---|--:|--:|--:|--:|--:|---|\n");
    for (auto& r : srt)
        printf("| %s | %s | %.4g | %.2f | %s | %.3f | %.3f %s | %.3f | %.3f | %llu%s | %s%s |\n", r.id.c_str(), md(r.name).c_str(), r.rate / 1e6, r.secs, r.src.c_str(), r.scost, r.tcost, r.tname.c_str(), r.nscost, r.ntcost,
               (unsigned long long)r.good, r.locked ? "" : " (no lock)", md(r.top).c_str(), r.err.c_str());
    int over = 0;
    for (auto& r : srt) if (r.scost > 1.0 || r.tcost > 1.0) over++;
    printf("\n%zu modes, %d above 1.0 CPU s per s (all threads or busiest)\n", rows.size(), over);
    if (!outPrefix.empty())
        if (FILE* f = fopen((outPrefix + ".tsv").c_str(), "w")) {
            fprintf(f, "id\tname\tstd\trate\tsecs\tsignal\tall\tbusiest\tbusiest_name\tnoise_all\tnoise_busiest\tblocks\tlocked\ttop\terr\n");
            for (auto& r : rows)
                fprintf(f, "%s\t%s\t%d\t%.0f\t%.3f\t%s\t%.4f\t%.4f\t%s\t%.4f\t%.4f\t%llu\t%d\t%s\t%s\n", r.id.c_str(), md(r.name).c_str(), r.stdMode, r.rate, r.secs, r.src.c_str(), r.scost, r.tcost, r.tname.c_str(), r.nscost, r.ntcost,
                        (unsigned long long)r.good, r.locked ? 1 : 0, r.top.c_str(), r.err.c_str());
            fclose(f);
        }
    return 0;
}

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
    bool list = false, strict = false, doSwitch = false, doCpu = false, secsSet = false, maxSet = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--list") list = true;
        else if (a == "--long") {}
        else if (a == "--strict") strict = true;
        else if (a == "--switch") doSwitch = true;
        else if (a == "--cpu") doCpu = true;
        else if (a == "--sel") { if (!getenv("QA_MODES")) { printf("qa_sweep_sel: set QA_MODES to the mode ids to run\n"); return 0; } }
        else if (a == "--mode") modes = next();
        else if (a == "--seconds") { secs = atof(next().c_str()); secsSet = true; }
        else if (a == "--snr") snrDb = atof(next().c_str());
        else if (a == "--max-samples") { maxSamples = atof(next().c_str()); maxSet = true; }
        else if (a == "--out") outPrefix = next();
        else if (a == "--shard") { const std::string s = next(); sscanf(s.c_str(), "%d/%d", &shardK, &shardN); }
        else if (a == "--gen") { stage = a; stId = next(); stRate = atof(next().c_str()); stSecs = atof(next().c_str()); snrDb = atof(next().c_str()); stFile = next(); }
        else if (a == "--gentx") { stage = a; stId = next(); stSecs = atof(next().c_str()); stFile = next(); }
        else if (a == "--run") { stage = a; stId = next(); stRate = atof(next().c_str()); stSecs = atof(next().c_str()); stFile = next(); stCond = next(); }
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (modes.empty() && getenv("QA_MODES")) modes = getenv("QA_MODES");   // ctest qa_sweep_sel: the modes to run, set in the environment
    const std::vector<Entry> all = allEntries();
    auto find = [&](const std::string& id) -> const Entry* { for (auto& e : all) if (e.id == id) return &e; return nullptr; };
    if (!stage.empty()) {
        const Entry* en = find(stId);
        if (!en) return 2;
        if (stage == "--gen" && stRate <= 0) stRate = en->rate;   // 0: the mode's default rate
        if (stage == "--gentx") { printf("TXRATE %.4f\n", writeTx(*en, stSecs, stFile)); fflush(stdout); _exit(0); }
        return stage == "--gen" ? genChild(*en, stRate, stSecs, snrDb, stFile) : runChild(*en, stRate, stSecs, stFile, stCond == "clean", stCond == "rate" || stCond == "rpos" || stCond == "rnoise");
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
        for (auto* e : sel) printf("%-16s %-36s std %3d  %.4g Msps  %.4f MHz  bw %.6g MHz\n", e->id.c_str(), e->name.c_str(), e->stdMode, e->rate / 1e6, e->defMhz, e->bwMhz);
        printf("%zu modes\n", sel.size());
        return 0;
    }
    if (doSwitch) return switchTest(sel);
    if (doCpu) return cpuSweep(self, sel, secsSet ? secs : 6, maxSet ? maxSamples : 160e6, outPrefix);
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
        std::string txT0;
        unsetenv("QA_T0");
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
            {   // the wall-clock starts of the test signal and of the transmitter's file (JS8 needs them: see runChild)
                const size_t p0 = g.out.find("T0 ");
                if (p0 != std::string::npos && (p0 == 0 || g.out[p0 - 1] == '\n')) setenv("QA_T0", std::to_string(atof(g.out.c_str() + p0 + 3)).c_str(), 1);
                const size_t p1 = g.out.find("TXT0 ");
                txT0 = p1 == std::string::npos ? "" : std::to_string(atof(g.out.c_str() + p1 + 5));
            }
            if (g.out.find("NOGEN") != std::string::npos) {
                r.gen = false; r.note = "no test signal"; done = true;
                const size_t tp = g.out.find("TXRATE ");
                if (tp != std::string::npos) r.txRate = atof(g.out.c_str() + tp + 7);
                const size_t np = g.out.find("NOISERATE ");
                if (np != std::string::npos) {   // noise alone through the engine
                    const double nr = atof(g.out.c_str() + np + 10);
                    char nrs[64], nss[64];
                    snprintf(nrs, sizeof nrs, "%.4f", nr); snprintf(nss, sizeof nss, "%.3f", s);
                    const ChildResult cr = spawn({self, "--run", e->id, nrs, nss, tmpDir + "/noise.ci16", "noise"}, std::max(300.0, s * 20), std::max(600.0, s * 30));
                    const size_t jq = cr.out.rfind('{');
                    CondRes& cc = r.c[4];
                    if (cr.out.find("HANG ") != std::string::npos || cr.timedOut) cc.verdict = "HANG";
                    else if (cr.status != 0 || jq == std::string::npos) cc.verdict = cr.out.find("STARTFAIL") != std::string::npos ? "NOSTART" : "CRASH(" + std::to_string(cr.status) + ")";
                    else { const std::string j = cr.out.substr(jq); cc.good = (uint64_t)jnum(j, "good"); r.life = jstrv(j, "life"); cc.verdict = cc.good == 0 ? "pass" : "FALSE"; }
                    r.note = "no test signal (noise only)";
                }
                break;
            }
            const size_t rp = g.out.find("RATE ");
            if (g.status != 0 || rp == std::string::npos) { r.note = g.timedOut ? "generator hang" : "generator failed (" + std::to_string(g.status) + ")"; r.c[0].verdict = "FAIL"; done = true; break; }
            double psig = 0, obw = 0, active = 0; unsigned long long clip = 0;
            sscanf(g.out.c_str() + rp, "RATE %lf PSIG %lf OBW %lf ACTIVE %lf CLIP %llu", &actual, &psig, &obw, &active, &clip);
            r.rate = actual; r.obw = obw; r.active = active;
            { const size_t tp = g.out.find("TXRATE "); r.txRate = tp == std::string::npos ? 0 : atof(g.out.c_str() + tp + 7); }
            if (getenv("QA_SWEEP_VERBOSE")) fprintf(stderr, "  signal: rate %.0f psig %.4g obw %.0f Hz active %.3f\n", actual, psig, obw, active);
            if (psig <= 0) r.note = "generator sends silence";
            snprintf(rs, sizeof rs, "%.4f", actual);
            {   // silence first: what the receiver saves with nothing to decode (read as fast as it goes, also on the real-time try)
                unsetenv("QA_PACE");
                const ChildResult zr = spawn({self, "--run", e->id, rs, ss, tmpDir + "/zero.ci16", "zero"}, std::max(300.0, s * 20), std::max(600.0, s * 30));
                if (paced) setenv("QA_PACE", "1", 1);
                const size_t jp = zr.out.rfind('{');
                r.resZero = jp == std::string::npos ? 0 : (uint64_t)jnum(zr.out.substr(jp), "res");
            }
            double newRate = 0; std::string newT0, rateDir = tmpDir + "/rate";
            for (int c = 0; c < kNC; c++) {
                if (c == 1) r.byRes = r.c[0].good == 0 && r.c[0].res > r.resZero + 16;
                if (c > 0 && r.m(0) == 0 && !r.c[0].locked) break;   // nothing in the clean run: the others say nothing more
                std::string runFile = tmpDir + "/" + kConds[c] + ".ci16", savedT0;
                double runRate = actual;
                const char* oldT0 = getenv("QA_T0"); if (oldT0) savedT0 = oldT0;
                if (c == kCRate) {   // a non-default sample rate: the generator again at 1.37 x the rate, the radio's own noise over the whole new band
                    std::filesystem::create_directories(rateDir);
                    char rq[64]; snprintf(rq, sizeof rq, "%.4f", actual * kRateMul);
                    setenv("QA_NOTX", "1", 1);
                    const ChildResult g2 = spawn({self, "--gen", e->id, rq, ss, sn, rateDir}, 600, 900);
                    unsetenv("QA_NOTX");
                    const size_t rp2 = g2.out.find("RATE ");
                    if (g2.status != 0 || rp2 == std::string::npos) { r.c[c].verdict = g2.timedOut ? "HANG" : "NOGEN2"; newRate = 0; continue; }
                    newRate = atof(g2.out.c_str() + rp2 + 5);
                    const size_t p0 = g2.out.find("T0 ");
                    newT0 = (p0 != std::string::npos && (p0 == 0 || g2.out[p0 - 1] == '\n')) ? std::to_string(atof(g2.out.c_str() + p0 + 3)) : "";
                }
                if (c >= kCRate) {   // the rate run's files: the clean one, +2 kHz / +50 ppm, noise only
                    if (newRate <= 0) continue;
                    runRate = newRate;
                    runFile = rateDir + "/" + (c == kCRate ? "clean" : c == 6 ? "pos" : "noise") + ".ci16";
                    if (!newT0.empty()) setenv("QA_T0", newT0.c_str(), 1);
                }
                char rsc[64]; snprintf(rsc, sizeof rsc, "%.4f", runRate);
                const ChildResult cr = spawn({self, "--run", e->id, rsc, ss, runFile, kConds[c]}, std::max(300.0, s * 20), std::max(600.0, s * 30));
                if (c >= kCRate) { if (oldT0) setenv("QA_T0", savedT0.c_str(), 1); else unsetenv("QA_T0"); if (c == kNC - 1) { std::error_code ec2; std::filesystem::remove_all(rateDir, ec2); } }
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
            // (a lock with a few blocks is not enough either: PMR446's calls end after its 3 s, so +-2 kHz was judged on calls cut by the file end)
            // or one whose first block came late (in the last quarter of the signal): an offset or noise delays the lock and the other conditions
            // fail by time, not by the condition (Inmarsat: the first SU at 7.6 s of 8 s clean, 9.4 s with the offset)
            const bool few = !r.byRes && r.c[0].good > 0 && (r.c[0].good < 8 || r.c[0].first > 0.75 * s) && li + 2 < lens.size();
            // a lock without a block in a 3 s file may only be a track that has not reached its minimum length (the satellite carrier trackers want 2.5 s of line): the 8 s try
            const bool lockShort = !r.byRes && r.c[0].good == 0 && r.c[0].locked && r.secs < 7.5 && li + 2 < lens.size() && maxSamples / rr >= 7.5;
            if (((r.c[0].good > 0 && !few) || r.byRes || (r.c[0].locked && r.c[0].good == 0) || r.c[0].verdict != "-") && !lockShort) done = true;   // a lock alone ends it only for a mode that counts no blocks
            if (!done && !paced) for (auto& c : r.c) c = CondRes();   // the next try starts afresh
        }
        // the transmitter (tx.ci16 from the last generator run), with its own silence as the baseline; when it decodes nothing in a short run,
        // once more with 32 s (a keyboard mode's preamble and text take longer than a test signal's first frames)
        for (int txTry = 0; txTry < 2 && r.txRate > 0; txTry++) {
            const double s = txTry == 0 ? (r.secs > 0 ? r.secs : secs) : std::min(32.0, maxSamples / r.txRate);
            if (txTry == 1) {
                if (r.tx.verdict != "FAIL" || s <= (r.secs > 0 ? r.secs : secs) + 0.5) break;
                char ss0[64]; snprintf(ss0, sizeof ss0, "%.3f", s);
                const ChildResult gt = spawn({self, "--gentx", e->id, ss0, tmpDir}, 600, 900);
                const size_t p1 = gt.out.find("TXT0 ");
                if (p1 != std::string::npos) txT0 = std::to_string(atof(gt.out.c_str() + p1 + 5));
            }
            char rs[64], ss[64];
            snprintf(rs, sizeof rs, "%.4f", r.txRate); snprintf(ss, sizeof ss, "%.3f", s);
            unsetenv("QA_PACE");
            const ChildResult zr = spawn({self, "--run", e->id, rs, ss, tmpDir + "/txzero.ci16", "zero"}, std::max(300.0, s * 20), std::max(600.0, s * 30));
            if (r.paced) setenv("QA_PACE", "1", 1);
            const size_t zp = zr.out.rfind('{');
            const uint64_t z = zp == std::string::npos ? 0 : (uint64_t)jnum(zr.out.substr(zp), "res");
            if (!txT0.empty()) setenv("QA_T0", txT0.c_str(), 1); else unsetenv("QA_T0");
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
                if (isNoiseCond(c)) cc.verdict = blocks ? (r.m(c) == 0 ? "pass" : "FALSE") : (cc.locked ? "lock" : "pass");
                else if (c > 0 && !blocks && !r.c[0].locked) cc.verdict = "-";
                else if (blocks) cc.verdict = r.m(c) > 0 ? (c > 0 && r.m(c) * 2 < r.m(0) ? "weak" : "pass") : "FAIL";
                else cc.verdict = cc.locked ? "lock" : "FAIL";
            }
            if (!blocks && !r.c[0].locked) r.c[0].verdict = "FAIL";
        }
        if (!r.gen) {
            const std::string& v = r.c[4].verdict;
            if ((v == "FALSE" || v == "HANG" || v == "NOSTART" || v.rfind("CRASH", 0) == 0) && !knownWhy(r.id, "noise")) r.unexpected.push_back("noise " + v);
        }
        if (r.gen && r.life != "ok" && r.life != "-") r.unexpected.push_back("lifecycle: " + r.life);
        if (r.gen) for (int c = 0; c < kNC; c++) {
            const std::string& v = r.c[c].verdict;
            const bool bad = v == "FAIL" || v == "FALSE" || v == "HANG" || v == "NOGEN2" || v == "NOSTART" || v.rfind("CRASH", 0) == 0;
            const char* why = knownWhy(r.id, kConds[c]);
            if (!why && c == 6) why = knownWhy(r.id, "pos");     // the variants at the new rate share their parent's known failure
            if (!why && c == 7) why = knownWhy(r.id, "noise");
            if (!why && c > kCRate) why = knownWhy(r.id, "rate");   // and a failure of the new rate itself
            if (bad && !why) r.unexpected.push_back(std::string(kConds[c]) + " " + v);
            if (!bad && why && v != "-" && strict && c <= kCRate) r.unexpected.push_back(std::string(kConds[c]) + " passes now: drop it from qa_known.inc");
        }
        {
            const std::string& v = r.tx.verdict;
            const bool bad = v == "FAIL" || v == "HANG" || v == "NOSTART" || v.rfind("CRASH", 0) == 0;
            const char* why = knownWhy(r.id, "tx");
            if (bad && !why) r.unexpected.push_back("tx " + v);
            if (!bad && why && v != "-" && strict) r.unexpected.push_back("tx passes now: drop it from qa_known.inc");
        }
        unexpected += (int)r.unexpected.size();
        fprintf(stderr, "%s %s %s %s %s rate %s %s %s tx %s cpu %.4f %s%s\n", r.c[0].verdict.c_str(), r.c[1].verdict.c_str(), r.c[2].verdict.c_str(), r.c[3].verdict.c_str(), r.c[4].verdict.c_str(), r.c[5].verdict.c_str(), r.c[6].verdict.c_str(), r.c[7].verdict.c_str(), r.tx.verdict.c_str(), r.cost,
                r.note.c_str(), r.unexpected.empty() ? "" : "  <-- UNEXPECTED");
        rows.push_back(r);
    }
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    printf("\n%-16s %4s %9s %6s | %-6s %-6s %-6s %-6s %-6s %-6s %-6s %-6s %-6s | %8s %9s %s\n", "mode", "std", "rate Msps", "secs", "clean", "pos", "neg", "snr", "noise", "rate", "rpos", "rnoise", "tx", "cpu", "clean blk", "notes");
    for (auto& r : rows) {
        printf("%-16s %4d %9.4g %6.1f | %-6s %-6s %-6s %-6s %-6s %-6s %-6s %-6s %-6s | %8.4f %9llu %s%s", r.id.c_str(), r.stdMode, r.rate / 1e6, r.secs, r.c[0].verdict.c_str(), r.c[1].verdict.c_str(), r.c[2].verdict.c_str(),
               r.c[3].verdict.c_str(), r.c[4].verdict.c_str(), r.c[5].verdict.c_str(), r.c[6].verdict.c_str(), r.c[7].verdict.c_str(), r.tx.verdict.c_str(), r.cost, (unsigned long long)r.m(0), ((r.byRes ? "(saved bytes) " : "") + std::string(r.paced ? "(real time) " : "") + r.note).c_str(), r.life != "ok" && r.life != "-" ? (" life:" + r.life).c_str() : "");
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
