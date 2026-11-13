// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cost of the shared sample path around a receiver (what every mode pays before its own decoder): sample conversion, the IQ ring, the
// engine's per-block clean-up, the spectrum analyser and the resamplers. CPU seconds per second of signal (getrusage, all threads), and a
// digest of every row's output so that a change can be checked to give the same bits. Not part of ctest; tools/bench/bench_core.sh builds it.
//
//   bench_core [--list] [--only a,b] [--seconds S] [--best N] [--md]
#include "dect2/common/ring.h"
#include "dect2/common/threadpool.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/resampler.h"
#include "dect2/dsp/sample_convert.h"
#include "dect2/dsp/spectrum.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <random>
#include <string>
#include <vector>
#include <sys/resource.h>

using namespace dect2;

namespace {

double cpuNow() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec * 1e-6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec * 1e-6;
}

// FNV-1a over raw bytes: the "same bits" check
struct Digest {
    uint64_t h = 1469598103934665603ull;
    void add(const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; } }
};

struct Row { std::string name, what; double rate = 0, sigSec = 0, cpu = 0; uint64_t digest = 0; long long items = 0; };
struct Bench { std::string name, what; double rate; std::function<Row(double)> run; };

std::vector<int8_t> cs8Sig(double rate, double secs) {
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 30.f);
    std::vector<int8_t> v((size_t)(rate * secs) * 2);
    for (auto& x : v) x = (int8_t)std::clamp((int)std::lround(nd(rng)), -128, 127);
    return v;
}
std::vector<cf32> cfSig(double rate, double secs) {
    auto b = cs8Sig(rate, secs);
    std::vector<cf32> v(b.size() / 2);
    for (size_t i = 0; i < v.size(); i++) v[i] = cf32(b[2 * i] / 128.f, b[2 * i + 1] / 128.f);
    return v;
}

int gRep = 1;   // --repeat: the timed part runs this many times over the same signal (for profiling)
constexpr size_t kUsb = 131072;   // samples per HackRF USB transfer (256 KiB)

std::vector<Bench> benches() {
    std::vector<Bench> b;
    b.push_back({"conv_cs8", "cs8 -> cf32 (HackRF transfer) and the ring write, 10 Msps", 10e6, [](double secs) {
        Row r; r.rate = 10e6; r.sigSec = secs;
        auto raw = cs8Sig(10e6, secs);
        IqRingCpp ring(1u << 24);
        std::vector<cf32> drain(kUsb);
        Digest d;
        const double c0 = cpuNow();
        for (size_t i = 0; i + 2 * kUsb <= raw.size(); i += 2 * kUsb) {
            convertCs8Ring(raw.data() + i, kUsb, ring);
            r.cpu -= cpuNow();
            ring.read(drain.data(), kUsb);   // the reader's side is its own row
            if (i == 0) d.add(drain.data(), kUsb * sizeof(cf32));
            r.cpu += cpuNow();
        }
        r.cpu += cpuNow() - c0; r.digest = d.h;
        return r;
    }});
    b.push_back({"conv_cu8", "cu8 -> cf32 (RTL-SDR: (x - 127.4) / 128), 10 Msps", 10e6, [](double secs) {
        Row r; r.rate = 10e6; r.sigSec = secs;
        auto raw = cs8Sig(10e6, secs);
        std::vector<cf32> out(kUsb);
        Digest d;
        const double c0 = cpuNow();
        for (size_t i = 0; i + 2 * kUsb <= raw.size(); i += 2 * kUsb) {
            convertCu8(reinterpret_cast<const uint8_t*>(raw.data() + i), kUsb, out.data(), 127.4f, 128.f);
            if (i == 0) d.add(out.data(), kUsb * sizeof(cf32));
        }
        r.cpu = cpuNow() - c0; r.digest = d.h;
        return r;
    }});
    b.push_back({"conv_cs16", "cs16 -> cf32 (x / 32768), 10 Msps", 10e6, [](double secs) {
        Row r; r.rate = 10e6; r.sigSec = secs;
        auto raw8 = cs8Sig(10e6, secs);
        std::vector<int16_t> raw(raw8.size());
        for (size_t i = 0; i < raw.size(); i++) raw[i] = (int16_t)(raw8[i] * 211);
        std::vector<cf32> out(kUsb);
        Digest d;
        const double c0 = cpuNow();
        for (size_t i = 0; i + 2 * kUsb <= raw.size(); i += 2 * kUsb) {
            convertCs16(raw.data() + i, kUsb, out.data(), 1.f / 32768);
            if (i == 0) d.add(out.data(), kUsb * sizeof(cf32));
        }
        r.cpu = cpuNow() - c0; r.digest = d.h;
        return r;
    }});
    b.push_back({"ring_read", "IqRing read in 64k pieces (the analysis thread), 10 Msps", 10e6, [](double secs) {
        Row r; r.rate = 10e6; r.sigSec = secs;
        auto sig = cfSig(10e6, 0.2);
        IqRingCpp ring(1u << 24);
        std::vector<cf32> buf(1 << 16);
        const size_t total = (size_t)(secs * 10e6);
        for (size_t done = 0; done < total;) {
            const size_t m = std::min(sig.size(), total - done);
            ring.write(sig.data(), m);
            const double c0 = cpuNow();
            size_t n;
            while ((n = ring.read(buf.data(), buf.size())) > 0) r.items += (long long)n;
            r.cpu += cpuNow() - c0;
            done += m;
        }
        return r;
    }});
    b.push_back({"clean", "the engine's per-block clean-up: non-finite check (dropNonFinite), 10 Msps", 10e6, [](double secs) {
        Row r; r.rate = 10e6; r.sigSec = secs;
        auto sig = cfSig(10e6, secs);
        Digest d;
        const double c0 = cpuNow();
        for (size_t i = 0; i < sig.size(); i += 65536) dropNonFinite(sig.data() + i, std::min<size_t>(65536, sig.size() - i));
        r.cpu = cpuNow() - c0;
        d.add(sig.data(), 65536 * sizeof(cf32)); r.digest = d.h;
        return r;
    }});
    // the engine's spectrum path: a copy of each 64k block into the queue, the analyser (32768 bins at 10 Msps, autoSize), a frame every 33 ms
    b.push_back({"spectrum", "engine spectrum path at 10 Msps: queue copy + SpectrumAnalyzer 32768 + a frame every 33 ms", 10e6, [](double secs) {
        Row r; r.rate = 10e6; r.sigSec = secs;
        auto sig = cfSig(10e6, secs);
        SpectrumAnalyzer sa(SpectrumAnalyzer::autoSize(10e6));
        SpectrumFrame fr;
        Digest d;
        std::deque<std::vector<cf32>> q;
        const size_t per = 330000;
        const double c0 = cpuNow();
        for (int rep = 0; rep < gRep; rep++) for (size_t i = 0; i < sig.size(); i += 65536) {
            const size_t n = std::min<size_t>(65536, sig.size() - i);
            q.emplace_back(sig.data() + i, sig.data() + i + n);
            std::vector<cf32> chunk = std::move(q.front()); q.pop_front();
            sa.feed(chunk.data(), chunk.size());
            if ((i + n) / per != i / per && sa.takeFrame(fr)) { r.items++; if (rep == 0) d.add(fr.dbfs.data(), fr.dbfs.size() * 4); }
        }
        r.cpu = (cpuNow() - c0) / gRep; r.digest = d.h; r.items /= gRep;
        return r;
    }});
    b.push_back({"spec_stats", "SpectrumAnalyzer with the transform off (level statistics only), 10 Msps", 10e6, [](double secs) {
        Row r; r.rate = 10e6; r.sigSec = secs;
        auto sig = cfSig(10e6, secs);
        SpectrumAnalyzer sa(SpectrumAnalyzer::autoSize(10e6));
        sa.setTransform(false);
        SpectrumFrame fr;
        Digest d;
        const size_t per = 330000;
        const double c0 = cpuNow();
        for (int rep = 0; rep < gRep; rep++) for (size_t i = 0; i < sig.size(); i += 65536) {
            const size_t n = std::min<size_t>(65536, sig.size() - i);
            sa.feed(sig.data() + i, n);
            if ((i + n) / per != i / per && sa.takeFrame(fr)) {
                r.items++;
                if (rep == 0) { d.add(&fr.stats.peak, 4); d.add(&fr.stats.rmsDbfs, 4); d.add(&fr.stats.dcI, 8); d.add(fr.stats.hist, sizeof fr.stats.hist); d.add(&fr.stats.clipFraction, 4); }
            }
        }
        r.cpu = (cpuNow() - c0) / gRep; r.digest = d.h; r.items /= gRep;
        return r;
    }});
    b.push_back({"spectrum20", "SpectrumAnalyzer at 20 Msps (autoSize), a frame every 33 ms", 20e6, [](double secs) {
        Row r; r.rate = 20e6; r.sigSec = secs;
        auto sig = cfSig(20e6, secs);
        SpectrumAnalyzer sa(SpectrumAnalyzer::autoSize(20e6));
        SpectrumFrame fr;
        Digest d;
        const size_t per = 660000;
        const double c0 = cpuNow();
        for (int rep = 0; rep < gRep; rep++) for (size_t i = 0; i < sig.size(); i += 65536) {
            const size_t n = std::min<size_t>(65536, sig.size() - i);
            sa.feed(sig.data() + i, n);
            if ((i + n) / per != i / per && sa.takeFrame(fr)) { r.items++; if (rep == 0) d.add(fr.dbfs.data(), fr.dbfs.size() * 4); }
        }
        r.cpu = (cpuNow() - c0) / gRep; r.digest = d.h; r.items /= gRep;
        return r;
    }});
    for (int lg : {10, 13, 15, 16}) {
        const int n = 1 << lg;
        b.push_back({"fft" + std::to_string(n), "fftSplit " + std::to_string(n) + " points, back to back at 10 Msps", 10e6, [lg, n](double secs) {
            Row r; r.rate = 10e6; r.sigSec = secs;
            std::vector<float> re(n), im(n), r0(n), i0(n);
            for (int i = 0; i < n; i++) { r0[i] = std::sin(0.01f * i) + 0.001f * (i % 7); i0[i] = std::cos(0.013f * i); }
            Digest d;
            re = r0; im = i0; fftSplit(re.data(), im.data(), lg, false); d.add(re.data(), n * 4); d.add(im.data(), n * 4);
            fftSplit(re.data(), im.data(), lg, true); d.add(re.data(), n * 4); d.add(im.data(), n * 4);
            const long cnt = (long)(secs * 10e6 / n);
            const double c0 = cpuNow();
            for (long k = 0; k < cnt; k++) { if ((k & 63) == 0) { re = r0; im = i0; } fftSplit(re.data(), im.data(), lg, false); }
            r.cpu = cpuNow() - c0; r.digest = d.h; r.items = cnt;
            return r;
        }});
    }
    b.push_back({"rs_dvbt", "RationalResampler 10 -> 9.142857 Msps, 7.61 MHz occupied (what DVB-T 8 MHz runs)", 10e6, [](double secs) {
        Row r; r.rate = 10e6; r.sigSec = secs;
        auto sig = cfSig(10e6, secs);
        RationalResampler rs; rs.configure(10e6, 64e6 / 7, nullptr, 6817 * (64e6 / 7) / 8192.0);
        std::vector<cf32> out; out.reserve(1 << 17);
        Digest d;
        const double c0 = cpuNow();
        for (int rep = 0; rep < gRep; rep++) for (size_t i = 0; i < sig.size(); i += 65536) {
            out.clear(); rs.process(sig.data() + i, std::min<size_t>(65536, sig.size() - i), out); r.items += (long long)out.size();
            if (i < 4 * 65536 && rep == 0) d.add(out.data(), out.size() * sizeof(cf32));
        }
        r.cpu = (cpuNow() - c0) / gRep; r.digest = d.h; r.items /= gRep;
        return r;
    }});
    b.push_back({"rs_wide", "RationalResampler 10 -> 9.142857 Msps, full band (DVB-T2 native)", 10e6, [](double secs) {
        Row r; r.rate = 10e6; r.sigSec = secs;
        auto sig = cfSig(10e6, secs);
        RationalResampler rs; rs.configure(10e6, 64e6 / 7);
        std::vector<cf32> out; out.reserve(1 << 17);
        Digest d;
        const double c0 = cpuNow();
        for (int rep = 0; rep < gRep; rep++) for (size_t i = 0; i < sig.size(); i += 65536) {
            out.clear(); rs.process(sig.data() + i, std::min<size_t>(65536, sig.size() - i), out); r.items += (long long)out.size();
            if (i < 4 * 65536 && rep == 0) d.add(out.data(), out.size() * sizeof(cf32));
        }
        r.cpu = (cpuNow() - c0) / gRep; r.digest = d.h; r.items /= gRep;
        return r;
    }});
    b.push_back({"rs_off20", "ExactResampler 20 -> 10 Msps (offset tuning on a HackRF)", 20e6, [](double secs) {
        Row r; r.rate = 20e6; r.sigSec = secs;
        auto sig = cfSig(20e6, secs);
        ExactResampler rs; rs.configure(20e6, 10e6);
        std::vector<cf32> out; out.reserve(1 << 17);
        Digest d;
        const double c0 = cpuNow();
        for (int rep = 0; rep < gRep; rep++) for (size_t i = 0; i < sig.size(); i += 65536) {
            out.clear(); rs.process(sig.data() + i, std::min<size_t>(65536, sig.size() - i), out); r.items += (long long)out.size();
            if (i < 4 * 65536 && rep == 0) d.add(out.data(), out.size() * sizeof(cf32));
        }
        r.cpu = (cpuNow() - c0) / gRep; r.digest = d.h; r.items /= gRep;
        return r;
    }});
    b.push_back({"pool_for", "ThreadPool::parallelFor of 4 tiny jobs, 2000 a second (scheduling overhead only)", 1, [](double secs) {
        Row r; r.rate = 1; r.sigSec = secs;
        std::vector<double> slot(4);
        const long cnt = (long)(secs * 2000);
        const double c0 = cpuNow();
        for (long k = 0; k < cnt; k++) ThreadPool::shared().parallelFor(4, [&](size_t i) { slot[i] += (double)i; }, 3);
        r.cpu = cpuNow() - c0; r.items = cnt;
        return r;
    }});
    return b;
}

} // namespace

int main(int argc, char** argv) {
    double secs = 4; int best = 1; bool md = false, list = false;
    std::string only;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) secs = atof(argv[++i]);
        else if (a == "--best" && i + 1 < argc) best = std::max(1, atoi(argv[++i]));
        else if (a == "--only" && i + 1 < argc) only = "," + std::string(argv[++i]) + ",";
        else if (a == "--repeat" && i + 1 < argc) gRep = std::max(1, atoi(argv[++i]));
        else if (a == "--md") md = true;
        else if (a == "--list") list = true;
    }
    auto bs = benches();
    if (list) { for (auto& x : bs) printf("%-12s %s\n", x.name.c_str(), x.what.c_str()); return 0; }
    if (md) printf("| Item | What | Rate (Msps) | CPU s / s | ns / sample | digest |\n|---|---|--:|--:|--:|---|\n");
    for (auto& x : bs) {
        if (!only.empty() && only.find("," + x.name + ",") == std::string::npos) continue;
        Row bestR; bestR.cpu = 1e30;
        for (int k = 0; k < best; k++) { Row r = x.run(secs); if (r.cpu < bestR.cpu) bestR = r; }
        const double cost = bestR.cpu / bestR.sigSec, ns = bestR.cpu / (bestR.sigSec * x.rate) * 1e9;
        if (md) printf("| `%s` | %s | %.3g | %.4f | %.3f | %016llx |\n", x.name.c_str(), x.what.c_str(), x.rate / 1e6, cost, ns, (unsigned long long)bestR.digest);
        else printf("%-12s cost %.4f CPU s/s  %.3f ns/sample  items %lld  digest %016llx\n", x.name.c_str(), cost, ns, bestR.items, (unsigned long long)bestR.digest);
        fflush(stdout);
    }
    return 0;
}
