// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DTMB LDPC on the GPU (GpuLdpc::decodeQc) against the CPU decoder: the same words, the same result (ok flag, iteration count, information bits),
// then the whole channel decoder with the GPU against the CPU: the same transport stream. Skipped when this machine has no GPU kernel for it.
#include "dect2/dtmb/dtmb_chain.h"
#include "dect2/common/stable_random.h"
#include "dect2/dtmb/dtmb_gen.h"
#include "dect2/dtmb/dtmb_map.h"
#include "dect2/dsp/gpu_ldpc.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// the stream a chain with `workers` threads delivers for a fixed noisy signal, in the given compute mode (0 CPU, 1 GPU)
static std::vector<uint8_t> streamOf(const Profile& p, double snrDb, int frames, int mode, uint64_t* gpuWords) {
    const Header h = Header::Pn595;
    TxConfig tc; tc.header = h; tc.profile = p;
    FrameTx tx(tc, testPacketSource(3));
    FecChain chain(p, h, 3);
    chain.setComputeMode(mode);
    std::vector<cf32> fr((size_t)tx.frameLength()), bins((size_t)kBody), si(36), data((size_t)kDataSymbols);
    std::vector<float> var((size_t)kDataSymbols, (float)std::pow(10.0, -snrDb / 10.0));
    MixedFft fft(kBody);
    std::mt19937 rng(21);
    dect2::normal_distribution<float> nd;
    const float sd = (float)std::sqrt(std::pow(10.0, -snrDb / 10.0) / 2.0);
    std::vector<uint8_t> out;
    auto cb = [&](const uint8_t* pk, size_t n, double) { out.insert(out.end(), pk, pk + n * 188); };
    for (int f = 0; f < frames; f++) {
        tx.nextFrame(fr.data());
        std::copy(fr.begin() + headerInfo(h).length, fr.end(), bins.begin());
        fft.forward(bins.data());
        for (auto& b : bins) b *= 1.f / std::sqrt((float)kBody);
        splitBody(bins.data(), si.data(), data.data());
        for (auto& d : data) d += cf32{sd * nd(rng), sd * nd(rng)};
        chain.pushFrame(data.data(), var.data());
        chain.poll(cb);
    }
    chain.flush(cb);
    *gpuWords = chain.stats().gpuWords;
    return out;
}

int main() {
    GpuLdpc& g = GpuLdpc::instance();
    if (!g.qcAvailable()) { printf("dtmb_gpu: SKIPPED (no GPU kernel for DTMB on this machine: %s)\n", g.available() ? "backend has no table-driven kernel" : "no GPU"); return 0; }
    printf("GPU: %s\n", g.deviceName());
    std::mt19937 rng(11);
    dect2::normal_distribution<float> nd(0.f, 1.f);

    // ---- words: clean, noisy around the threshold (some decode, some fail, some stop early), mostly erased
    const int nb = 150;
    for (int r = 0; r < 3; r++) {
        const dtmb::LdpcCode& code = ldpcCode((Rate)r);
        const int infoN = code.infoBits();
        // Eb/N0 of the information bits; thresholds are about 1.9 / 2.0 / 3.0 dB (test_dtmb_fec)
        const double ebn0[4] = {100.0, 3.0 + r * 0.2, (r == 2 ? 3.4 : 2.4), (r == 2 ? 2.6 : 1.6)};
        long wordsDone = 0, mism = 0, okMism = 0, itMism = 0, gpuOk = 0, cpuOk = 0;
        double tCpu = 0, tGpu = 0;
        for (int pt = 0; pt < 4; pt++) {
            const double rateInfo = (double)infoN / (double)kLdpcSent;
            const double sigma2 = 1.0 / (2.0 * rateInfo * std::pow(10.0, ebn0[pt] / 10.0));
            const float sigma = (float)std::sqrt(sigma2);
            std::vector<float> llr((size_t)nb * kLdpcSent);
            for (int b = 0; b < nb; b++) {
                std::vector<uint8_t> info((size_t)infoN), sent(kLdpcSent);
                for (auto& x : info) x = (uint8_t)(rng() & 1);
                code.encode(info.data(), sent.data());
                for (int i = 0; i < kLdpcSent; i++) {
                    const float x = (sent[(size_t)i] ? -1.f : 1.f) + sigma * nd(rng);
                    llr[(size_t)b * kLdpcSent + i] = (float)(2.0 * x / sigma2);
                }
                if (pt == 3 && (b % 5) == 0) for (int i = 0; i < kLdpcSent; i++) if (rng() % 3) llr[(size_t)b * kLdpcSent + i] = 0.f;   // a heavily erased word
            }
            std::vector<uint8_t> infoG((size_t)nb * infoN), okG((size_t)nb);
            std::vector<int> itG((size_t)nb);
            double t0 = now();
            const bool called = g.decodeQc(code.qcTable(), llr.data(), nb, 60, infoG.data(), okG.data(), itG.data(), 0.6f);
            tGpu += now() - t0;
            CHECK(called, "rate %d point %d: decodeQc failed", r, pt);
            if (!called) continue;
            dtmb::LdpcCode::Decoder dec(code);
            std::vector<uint8_t> infoC((size_t)infoN);
            for (int b = 0; b < nb; b++) {
                const float* w = &llr[(size_t)b * kLdpcSent];
                int zeros = 0;
                for (int i = 0; i < kLdpcSent; i++) zeros += w[i] == 0.f;
                if (zeros > kLdpcSent / 3) continue;   // the receiver decodes these on the CPU (they need no iteration)
                t0 = now();
                const dtmb::LdpcCode::Result cr = dec.decode(w, infoC.data(), 60);
                tCpu += now() - t0;
                wordsDone++;
                gpuOk += okG[(size_t)b]; cpuOk += cr.ok;
                if ((bool)okG[(size_t)b] != cr.ok) okMism++;
                if (itG[(size_t)b] != cr.iterations) itMism++;
                if (std::memcmp(&infoG[(size_t)b * infoN], infoC.data(), (size_t)infoN) != 0) mism++;
            }
        }
        printf("  rate %d: %ld words, ok CPU %ld GPU %ld; info mismatches %ld, ok-flag mismatches %ld, iteration mismatches %ld; CPU %.0f ms, GPU %.0f ms (one thread against one dispatch)\n",
               r, wordsDone, cpuOk, gpuOk, mism, okMism, itMism, tCpu * 1e3, tGpu * 1e3);
        CHECK(okMism == 0, "rate %d: the GPU and the CPU disagree on whether %ld words decoded", r, okMism);
        CHECK(mism == 0, "rate %d: %ld words with different information bits", r, mism);
        CHECK(itMism == 0, "rate %d: %ld words with a different iteration count", r, itMism);
        CHECK(cpuOk > wordsDone / 3, "rate %d: too few words decode for the comparison to mean much (%ld of %ld)", r, cpuOk, wordsDone);
    }

    // ---- the channel decoder: same bytes out on the GPU and on the CPU (a noisy signal, so the LDPC iterates)
    for (const int si : {10, 6}) {
        Profile p;
        profileFromSi(si, p);
        const int frames = (int)((51L * 52 * interleaverDelay(p) + kDataSymbols - 1) / kDataSymbols) + 40;
        for (const double snr : {40.0, 11.0}) {
            uint64_t gw0 = 0, gw1 = 0;
            const std::vector<uint8_t> a = streamOf(p, snr, frames, 0, &gw0), b = streamOf(p, snr, frames, 1, &gw1);
            printf("  SI %d at %.0f dB: CPU %zu bytes, GPU %zu bytes (%llu words on the GPU)\n", si, snr, a.size(), b.size(), (unsigned long long)gw1);
            CHECK(!a.empty() && a == b, "SI %d at %.0f dB: the output on the GPU differs from the CPU's", si, snr);
            CHECK(gw0 == 0, "the CPU run used the GPU");
            CHECK(gw1 > 0, "the GPU run decoded nothing on the GPU");
        }
    }
    printf(failures ? "dtmb_gpu: %d FAILED\n" : "dtmb_gpu: all passed\n", failures);
    return failures ? 1 : 0;
}
