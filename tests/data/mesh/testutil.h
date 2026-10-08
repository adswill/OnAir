// Helpers shared by the mesh (LoRa) tests: a signal made of LoRa frames plus noise, and a channel + demodulator chain.
#pragma once
#include "dect2/gen_util.h"
#include "dect2/mesh_lora.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace meshtest {
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); meshtest::fails++; } } while (0)

// amplitude of a frame for an in-band SNR (dB) against complex noise of rms `noiseRms` over the whole sample rate
inline float ampFor(double snrDb, double bwHz, double rate, double noiseRms) {
    return (float)std::sqrt(std::pow(10.0, snrDb / 10) * noiseRms * noiseRms * bwHz / rate);
}

inline std::vector<uint8_t> randomBytes(std::mt19937& g, size_t n) {
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = (uint8_t)(g() & 0xFF);
    return v;
}

// renders frames into a buffer of `secs` at `rate`, with complex Gaussian noise of rms noiseRms (0 = none)
inline std::vector<cf32> render(const std::vector<lora::TxFrame>& frames, double secs, double rate, double noiseRms, uint32_t seed = 1) {
    std::vector<cf32> x((size_t)(secs * rate), cf32(0, 0));
    for (const auto& f : frames) f.render(x.data(), x.size(), 0.0, rate);
    if (noiseRms > 0) { genutil::NoiseSource ns(seed); ns.add(x.data(), x.size(), (float)(noiseRms / std::sqrt(2.0))); }
    return x;
}

// one channel with one demodulator, fed in chunks
struct Chain {
    lora::Channelizer ch;
    lora::ChanBuf buf;
    lora::Demod dm;
    std::vector<lora::RxFrame> frames;
    Chain(double rate, double offsetHz, const lora::Params& p) : ch(rate, offsetHz, p.bwHz), dm(p, rate / std::max(1, (int)std::floor(rate / (3.0 * p.bwHz) + 1e-9))) {
        dm.reset(0);
    }
    void feed(const cf32* x, size_t n) {
        ch.process(x, n, buf);
        dm.process(buf, frames);
        buf.trim(dm.oldestNeeded());
    }
    void feedAll(const std::vector<cf32>& x, size_t chunk) {
        for (size_t i = 0; i < x.size(); i += chunk) feed(x.data() + i, std::min(chunk, x.size() - i));
    }
};


// A run of frames of one LoRa setting through one channel: returns how many came out with the right payload and CRC.
struct Trial {
    lora::Params p;
    double rate = 1e6, off = 0, snrDb = 20, cfoHz = 0, sroPpm = 0;
    int frames = 10;
    size_t len = 20;
    size_t chunk = 8192;
    bool q8 = false;             // round to 8 bits like a HackRF
    double dc = 0;               // DC offset added (fraction of full scale)
    double gapAt = -1, gapSec = 0;   // zero the input for gapSec from gapAt
    double resetAt = -1;         // reset the demodulator at this time
    uint32_t seed = 1;
    double noiseRms = 0.05;
};
inline int runTrial(const Trial& t, std::vector<lora::RxFrame>* outFrames = nullptr, int* decoded = nullptr, lora::DemodStats* stats = nullptr) {
    std::mt19937 g(t.seed);
    std::vector<lora::TxFrame> fr;
    std::vector<std::vector<uint8_t>> pays;
    double at = 0.03 + 0.01 * (g() % 10);
    for (int i = 0; i < t.frames; i++) {
        lora::TxFrame f;
        f.p = t.p;
        pays.push_back(randomBytes(g, t.len));
        f.data = lora::encode(t.p, pays.back().data(), pays.back().size());
        f.startSec = at; f.freqHz = t.off + t.cfoHz; f.sroPpm = t.sroPpm;
        f.amp = ampFor(t.snrDb, t.p.bwHz, t.rate, t.noiseRms);
        f.phase0 = (g() % 1000) / 1000.0;
        at = f.endSec() + 0.005 + 0.001 * (g() % 40);
        fr.push_back(f);
    }
    auto x = render(fr, at + 0.05, t.rate, t.noiseRms, t.seed * 7 + 1);
    if (t.dc != 0) for (auto& v : x) v += cf32((float)t.dc, (float)(-0.5 * t.dc));
    if (t.gapAt >= 0) {
        const size_t a = (size_t)(t.gapAt * t.rate), b = std::min(x.size(), (size_t)((t.gapAt + t.gapSec) * t.rate));
        for (size_t i = a; i < b; i++) x[i] = cf32(0, 0);
    }
    if (t.q8) for (auto& v : x) v = cf32(std::max(-127.f, std::min(127.f, std::round(v.real() * 127.f))) / 127.f, std::max(-127.f, std::min(127.f, std::round(v.imag() * 127.f))) / 127.f);
    Chain ch(t.rate, t.off, t.p);
    const size_t rs = t.resetAt >= 0 ? (size_t)(t.resetAt * t.rate) : x.size() + 1;
    for (size_t i = 0; i < x.size();) {
        size_t n = std::min(t.chunk, x.size() - i);
        if (i < rs && i + n > rs) n = rs - i;
        ch.feed(x.data() + i, n);
        i += n;
        if (i == rs) ch.dm.reset(ch.buf.end());
    }
    int ok = 0;
    std::vector<bool> used(pays.size(), false);
    for (const auto& f : ch.frames)
        for (size_t k = 0; k < pays.size(); k++)
            if (!used[k] && f.crcOk && f.payload == pays[k]) { used[k] = true; ok++; break; }
    if (outFrames) *outFrames = ch.frames;
    if (decoded) *decoded = (int)ch.frames.size();
    if (stats) *stats = ch.dm.stats();
    return ok;
}

} // namespace meshtest
