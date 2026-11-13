// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa receiver benchmark: packet success against SNR (90 % thresholds, with carrier / clock offsets) and CPU cost of the channel bank.
//   lora_bench sweep <sf> <bwKhz> <cr> [cfoFrac of bw/4... as Hz] [sroPpm] [len] [frames] [floorOffsetDb]
//   lora_bench cpu [seconds]
#include "../data/mesh/testutil.h"
#include "dect2/lorawan/lora_bank.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
using namespace dect2;
using namespace meshtest;

static double cpuSec() { timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }

static double gRateX = 4;
static double floorFor(int sf) { return -7.5 - 2.5 * (sf - 7); }

static int sweep(int argc, char** argv) {
    const int sf = atoi(argv[2]);
    const double bw = atof(argv[3]) * 1e3;
    const int cr = atoi(argv[4]);
    const double cfo = argc > 5 ? atof(argv[5]) : 0;
    const double sro = argc > 6 ? atof(argv[6]) : 0;
    const size_t len = argc > 7 ? (size_t)atoi(argv[7]) : 20;
    const int frames = argc > 8 ? atoi(argv[8]) : 30;
    const double fo = argc > 9 ? atof(argv[9]) : 0;
    lora::Params p; p.sf = sf; p.bwHz = bw; p.cr = cr; p.preamble = getenv("LORA_PRE") ? atoi(getenv("LORA_PRE")) : 8; p.ldro = lora::autoLdro(sf, bw);
    double prevSnr = 0, prevFrac = 1, thr = 99;
    const double start = floorFor(sf) + 4 + fo;
    int lowRuns = 0;
    printf("SF%d BW%.0f CR4/%d cfo %.0f sro %.0f len %zu:", sf, bw / 1e3, cr, cfo, sro, len);
    for (double snr = start; snr > start - 14; snr -= 0.5) {
        Trial t;
        t.p = p; t.rate = gRateX * bw; t.off = 0; t.cfoHz = cfo; t.sroPpm = sro; t.snrDb = snr; t.frames = frames; t.len = len;
        t.seed = (uint32_t)(sf * 1000 + (int)((snr + 40) * 2));
        if (const char* e = getenv("LORA_BURST")) sscanf(e, "%lf,%lf", &t.burstDuty, &t.burstDb);
        int dec = 0; lora::DemodStats stt;
        const int ok = runTrial(t, nullptr, &dec, &stt);
        const double fr = (double)ok / frames;
        if (getenv("LORA_DIAG")) printf(" %.1f:%d[p%llu s%llu h%llu c%llu]", snr, ok, (unsigned long long)stt.preambles, (unsigned long long)stt.syncBad, (unsigned long long)stt.headerBad, (unsigned long long)stt.crcBad);
        else printf(" %.1f:%d", snr, ok);
        fflush(stdout);
        if (fr < 0.9 && prevFrac >= 0.9 && thr > 90) thr = prevSnr + (prevFrac - 0.9) / (prevFrac - fr + 1e-9) * (snr - prevSnr);
        prevSnr = snr; prevFrac = fr;
        if (fr < 0.2 && ++lowRuns >= 2) break;
    }
    printf("\nTHRESH SF%d BW%.0f CR%d cfo%.0f sro%.0f: %.2f dB (datasheet floor %.1f)\n", sf, bw / 1e3, cr, cfo, sro, thr, floorFor(sf));
    return 0;
}

// noise (plus the unused channels' emptiness) through a bank: CPU seconds per second of signal
static double bankCpu(double rate, const std::vector<lorabank::Channel>& chans, double secs, size_t* decs) {
    std::vector<cf32> x((size_t)(secs * rate));
    genutil::NoiseSource ns(5);
    ns.add(x.data(), x.size(), 0.05f);
    lorabank::Bank b;
    b.configure(rate, chans);
    if (decs) *decs = b.decoders();
    std::vector<lorabank::Decoded> out;
    const double c0 = cpuSec();
    for (size_t i = 0; i < x.size(); i += 8192) b.feed(x.data() + i, std::min<size_t>(8192, x.size() - i), out);
    const double r = (cpuSec() - c0) / secs;
    if (getenv("LORA_DIAG")) { uint64_t pre = 0, sb = 0, hb = 0, fr = 0; for (auto& s : b.stats()) { pre += s.preambles; sb += s.syncBad; hb += s.headerBad; fr += s.frames; } printf("   [noise: preambles %llu syncBad %llu headerBad %llu frames %llu per s]\n", (unsigned long long)(pre / secs), (unsigned long long)(sb / secs), (unsigned long long)(hb / secs), (unsigned long long)(fr / secs)); }
    return r;
}

static int cpu(int argc, char** argv) {
    const double secs = argc > 2 ? atof(argv[2]) : 10;
    // raw LoRa auto mode: 3 bandwidths x SF7..12 x 3 sync words, one channel each at the centre
    {
        std::vector<lorabank::Channel> ch;
        for (double bw : {125e3, 250e3, 500e3}) {
            lorabank::Channel c; c.bwHz = bw; c.sfs = {7, 8, 9, 10, 11, 12}; c.syncs = {0x12, 0x34, 0x2B}; c.preamble = 8; ch.push_back(c);
        }
        size_t d; const double v = bankCpu(2e6, ch, secs, &d);
        printf("CPU raw-auto 2 Msps 3bw x 6sf x 3sync (%zu dec): %.4f\n", d, v);
    }
    for (int sf : {7, 9, 12}) {
        lorabank::Channel c; c.bwHz = 125e3; c.sfs = {sf}; c.syncs = {0x34}; c.preamble = 8;
        size_t d; const double v = bankCpu(1e6, {c}, secs, &d);
        printf("CPU 1 channel SF%d 125k 1 Msps: %.4f\n", sf, v);
    }
    // LoRaWAN-like: 8 channels x SF7..12, one sync
    {
        std::vector<lorabank::Channel> ch;
        for (int i = 0; i < 8; i++) {
            lorabank::Channel c; c.bwHz = 125e3; c.offsetHz = (i - 3.5) * 200e3; c.sfs = {7, 8, 9, 10, 11, 12}; c.syncs = {0x34}; c.preamble = 8; ch.push_back(c);
        }
        size_t d; const double v = bankCpu(2e6, ch, secs, &d);
        printf("CPU 8ch x 6sf 2 Msps (%zu dec): %.4f\n", d, v);
        std::vector<lorabank::Channel> c2(ch.begin(), ch.begin() + 2);
        const double v2 = bankCpu(2e6, c2, secs, &d);
        printf("CPU 2ch x 6sf 2 Msps (%zu dec): %.4f\n", d, v2);
    }
    return 0;
}


#ifdef LORA_NEW
// 8 LoRaWAN-style channels (200 kHz apart, 125 kHz) in a 2 Msps capture, frames of different SFs and carrier offsets on all of them:
// how many come out right, and the CPU time of the bank. LORA_NOFD=1 uses one channel filter per channel instead of the shared FFT.
static int multi(int argc, char** argv) {
    const double snr = argc > 2 ? atof(argv[2]) : 0;
    const int trials = argc > 3 ? atoi(argv[3]) : 3;
    const double rate = 2e6;
    if (getenv("LORA_NOFD")) lora::FdChannelizer::kEnabled = false;
    int total = 0, good = 0;
    double cpuAcc = 0, secAcc = 0;
    for (int tr = 0; tr < trials; tr++) {
        std::mt19937 g(100 + tr);
        std::vector<lora::TxFrame> fr;
        std::vector<std::pair<int, std::vector<uint8_t>>> pays;
        const double noiseRms = 0.05;
        double tEnd = 0;
        for (int ch = 0; ch < 8; ch++) {
            double at = 0.05 + 0.02 * (g() % 10);
            for (int k = 0; k < 2; k++) {
                lora::TxFrame f;
                f.p.sf = 7 + (int)((ch + k * 3) % 6); f.p.bwHz = 125e3; f.p.cr = 5; f.p.preamble = 8; f.p.syncWord = 0x34; f.p.ldro = lora::autoLdro(f.p.sf, 125e3);
                pays.push_back({ch, randomBytes(g, 16)});
                f.data = lora::encode(f.p, pays.back().second.data(), 16);
                f.startSec = at;
                f.freqHz = (ch - 3.5) * 200e3 + ((int)(g() % 2000) - 1000) * 5.0;
                f.sroPpm = ((int)(g() % 40) - 20);
                f.amp = ampFor(snr, 125e3, rate, noiseRms);
                f.phase0 = (g() % 1000) / 1000.0;
                at = f.endSec() + 0.02;
                tEnd = std::max(tEnd, at);
                fr.push_back(f);
            }
        }
        auto x = render(fr, tEnd + 0.1, rate, noiseRms, 7 + tr);
        std::vector<lorabank::Channel> chans;
        for (int ch = 0; ch < 8; ch++) {
            lorabank::Channel c; c.bwHz = 125e3; c.offsetHz = (ch - 3.5) * 200e3; c.sfs = {7, 8, 9, 10, 11, 12}; c.syncs = {0x34}; c.preamble = 8; chans.push_back(c);
        }
        lorabank::Bank b;
        b.configure(rate, chans);
        std::vector<lorabank::Decoded> out;
        const double c0 = cpuSec();
        for (size_t i = 0; i < x.size(); i += 8192) b.feed(x.data() + i, std::min<size_t>(8192, x.size() - i), out);
        cpuAcc += cpuSec() - c0; secAcc += (double)x.size() / rate;
        std::vector<bool> used(pays.size(), false);
        for (const auto& d : out)
            for (size_t k = 0; k < pays.size(); k++)
                if (!used[k] && d.f.crcOk && (int)d.chan == pays[k].first && d.f.payload == pays[k].second) { used[k] = true; break; }
        for (size_t k = 0; k < used.size(); k++) { good += used[k]; if (!used[k] && getenv("LORA_DIAG")) printf("  missed: channel %d frame %zu (SF %d)\n", pays[k].first, k, fr[k].p.sf); }
        total += (int)pays.size();
    }
    printf("MULTI snr %.1f: %d/%d frames right, bank CPU %.4f s per s of signal (%s)\n", snr, good, total, cpuAcc / secAcc, getenv("LORA_NOFD") ? "channel filters" : "shared FFT");
    return 0;
}

// the shared-FFT channeliser on a pure tone: amplitude and phase must stay smooth across the blocks
using cd_ = std::complex<double>;
static int fdcheck(int, char**) {
    const double rate = 2e6;
    std::vector<lora::FdChannelizer::Spec> sp;
    for (int ch = 0; ch < 4; ch++) sp.push_back({(ch - 1.5) * 200e3 + 100.0 * ch, 125e3});
    lora::FdChannelizer fd(rate, sp);
    const size_t n = 400000;
    std::vector<cf32> x(n);
    for (int ch = 0; ch < 4; ch++) {
        const double f0 = 9000.0 + 3000.0 * ch;
        for (size_t i = 0; i < n; i++) {
            const double ph = 2 * M_PI * (sp[ch].offsetHz + f0) * (double)i / rate;
            x[i] = cf32((float)std::cos(ph), (float)std::sin(ph)) * 0.f;
        }
        // single tone per run
        for (size_t i = 0; i < n; i++) {
            const double ph = 2 * M_PI * (sp[ch].offsetHz + f0) * (double)i / rate;
            x[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        fd.reset();
        std::vector<lora::ChanBuf> bufs(4);
        std::vector<lora::ChanBuf*> ptrs;
        for (auto& bb : bufs) ptrs.push_back(&bb);
        for (size_t i = 0; i < n; i += 4096) fd.process(x.data() + i, std::min<size_t>(4096, n - i), ptrs);
        const lora::ChanBuf& o = bufs[ch];
        const double r = fd.outRate(ch);
        double minA = 1e9, maxA = 0, maxDph = 0;
        for (int64_t k = 200; k + 1 < o.end(); k++) {
            const cf32 v = o.at(k)[0], w = o.at(k + 1)[0];
            const double a = std::abs(v);
            minA = std::min(minA, a); maxA = std::max(maxA, a);
            const double dph = std::arg(w * std::conj(v)) - 2 * M_PI * f0 / r;
            maxDph = std::max(maxDph, std::fabs(std::remainder(dph, 2 * M_PI)));
        }
        { cd_ acc(0, 0); for (int64_t k = 200; k + 1 < o.end(); k++) acc += std::complex<double>(o.at(k + 1)[0]) * std::conj(std::complex<double>(o.at(k)[0])); printf("  mean tone %.1f Hz (wanted %.1f), first samples %.3f %.3f %.3f\n", std::arg(acc) / (2 * M_PI) * r, f0, o.at(300)[0].real(), o.at(301)[0].real(), o.at(302)[0].real()); }
        printf("fdcheck ch %d: out rate %.0f, %lld samples, amplitude %.4f..%.4f, max phase step error %.5f rad\n", ch, r, (long long)o.end(), minA, maxA, maxDph);
    }
    return 0;
}
#endif

int main(int argc, char** argv) {
#ifdef LORA_NEW
    if (const char* e = getenv("LORA_RATIO")) lora::Channelizer::kMinRatio = atof(e);
    if (const char* e = getenv("LORA_SOFT")) lora::Demod::softOn = atoi(e) != 0;
    if (const char* e = getenv("LORA_STRIDE")) lora::Demod::searchStride = atoi(e);
#endif
    if (const char* e = getenv("LORA_RATE_X")) gRateX = atof(e);
    if (argc >= 5 && !strcmp(argv[1], "sweep")) return sweep(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "cpu")) return cpu(argc, argv);
#ifdef LORA_NEW
    if (argc >= 2 && !strcmp(argv[1], "fdcheck")) return fdcheck(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "multi")) return multi(argc, argv);
#endif
    printf("usage: lora_bench sweep sf bwKhz cr [cfoHz sroPpm len frames floorOffsetDb] | cpu [secs]\n");
    return 1;
}
