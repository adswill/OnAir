// DMR receiver on real bursts through a separate transmitter. The bursts are the ones captured off the air and published by the ok-dmrlib project
// (the same hex strings as in test_dmr_burst.cpp: two base station control bursts of colour code 5, and a mobile station voice superframe of colour
// code 1 with its embedded link control). They are modulated here by a small transmitter that shares no code with the test signal generator
// (dmr_gen.cpp): the dibit table and the pulse shape (root raised cosine, 0.2, from its frequency response) are written out again, and the frequency
// modulator integrates the phase exactly. If the receiver and the generator had the same wrong idea of the modulation, this is where it shows.
// A CACH is added to the base station signal with the bit positions and the Hamming (7,4) words of the OP25 project (dmr_const.h), also written
// out again here.
#include "dect2/dmr_rx.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<int> bitsOf(const char* hex) {
    std::vector<int> b;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
        const int v = (int)strtol(std::string(hex + i, 2).c_str(), nullptr, 16);
        for (int k = 7; k >= 0; k--) b.push_back((v >> k) & 1);
    }
    return b;
}

// dibit -> level (TS 102 361-1 table 9.x): 01 +3, 00 +1, 10 -1, 11 -3
static double level(int hi, int lo) { return hi == 0 ? (lo ? 3.0 : 1.0) : (lo ? -3.0 : -1.0); }
static std::vector<double> symbolsOf(const std::vector<int>& bits) {
    std::vector<double> s;
    for (size_t i = 0; i + 1 < bits.size(); i += 2) s.push_back(level(bits[i], bits[i + 1]));
    return s;
}

// the CACH: OP25's tables
static std::vector<int> cachBits(int at, int tc, int lcss) {
    static const int ham[16] = {0, 11, 22, 29, 39, 44, 49, 58, 69, 78, 83, 88, 98, 105, 116, 127};
    static const int tactPos[7] = {0, 4, 8, 12, 14, 18, 22};
    const int tact = ham[(at << 3) | (tc << 2) | lcss];
    std::vector<int> c(24, 0);
    for (int i = 0; i < 7; i++) c[(size_t)tactPos[i]] = (tact >> (6 - i)) & 1;
    return c;                                    // payload bits stay zero (no short LC)
}

struct Tx {
    std::vector<double> h;                       // root raised cosine, sum 1, 10 samples per symbol, 17 symbols long
    // shape: 0 root raised cosine (the standard), 1 raised cosine (a transmitter that put the whole Nyquist filter in the transmit side), 2 Gaussian BT 0.5
    explicit Tx(int shape = 0) {
        const int sps = 10, span = 8, n = 2 * span * sps + 1;
        // frequency response sqrt(RC), alpha 0.2, sampled at n points; the impulse response is its inverse transform
        std::vector<double> H(n);
        const double a = 0.2;
        for (int k = 0; k < n; k++) {
            const double f = (double)(k <= n / 2 ? k : k - n) / n * sps;        // in symbol rates
            const double af = std::fabs(f);
            const double rrc = af <= (1 - a) / 2 ? 1.0 : af <= (1 + a) / 2 ? std::cos(M_PI / (2 * a) * (af - (1 - a) / 2)) : 0.0;
            H[(size_t)k] = shape == 0 ? rrc : shape == 1 ? rrc * rrc : std::exp(-std::log(2.0) / 2.0 * std::pow(af / 0.5, 2));      // Gaussian, 3 dB at BT/2 with BT = 0.5
        }
        h.assign((size_t)n, 0.0);
        for (int t = 0; t < n; t++) {
            double v = 0;
            for (int k = 0; k < n; k++) v += H[(size_t)k] * std::cos(2 * M_PI * k * (t - n / 2) / n);
            h[(size_t)t] = v;
        }
        double sum = 0;
        for (double v : h) sum += v;
        for (double& v : h) v /= sum;
    }
    // symbols (0 where the carrier is off, with `on` per symbol) -> complex baseband at `rate`
    std::vector<cf32> modulate(const std::vector<double>& sym, const std::vector<char>& on, double rate, double cfoHz, double snrDb, double dc, unsigned seed, double innerScale = 1.0, double driftHzPerSec = 0.0) const {
        const int sps = 10;
        const size_t ns = sym.size() * sps;
        std::vector<double> f(ns, 0.0), g(ns, 0.0);
        const int half = (int)h.size() / 2;
        for (size_t i = 0; i < sym.size(); i++) {
            if (sym[i] == 0.0) continue;
            for (int t = 0; t < (int)h.size(); t++) {
                const long p = (long)(i * sps) + t - half;
                const double level = std::fabs(sym[i]) < 2.0 ? sym[i] * innerScale : sym[i];     // a transmitter whose inner deviations are not a third of the outer ones
                if (p >= 0 && p < (long)ns) f[(size_t)p] += level * h[(size_t)t] * sps * 648.0;
            }
        }
        for (size_t i = 0; i < sym.size(); i++)           // carrier gate with a 1.5 ms ramp (72 samples at 48 kHz)
            for (int t = 0; t < sps; t++) g[i * sps + (size_t)t] = on[i] ? 1.0 : 0.0;
        std::vector<double> gs(ns, 0.0);
        const int ramp = 72;
        double acc = 0;
        std::vector<double> csum(ns + 1, 0.0);
        for (size_t i = 0; i < ns; i++) csum[i + 1] = csum[i] + g[i];
        for (size_t i = 0; i < ns; i++) {
            const long lo = (long)i - ramp / 2, hi = (long)i + ramp / 2;
            const long a = std::max(0L, lo), b = std::min((long)ns, hi);
            gs[i] = b > a ? (csum[(size_t)b] - csum[(size_t)a]) / (double)ramp : 0.0;
        }
        (void)acc;
        const size_t n = (size_t)((double)ns / 48000.0 * rate);
        std::vector<cf32> out(n);
        std::mt19937 rng(seed);
        std::normal_distribution<double> nd(0.0, 1.0);
        const double amp = 0.25;
        const double sigma = amp * std::sqrt(rate / 12500.0 / std::pow(10.0, snrDb / 10.0) / 2.0);
        double phase = 0;
        for (size_t i = 0; i < n; i++) {
            const double pos = (double)i / rate * 48000.0;
            const size_t k = std::min((size_t)pos, ns - 2);
            const double fr = pos - (double)k;
            const double fi = f[k] * (1 - fr) + f[k + 1] * fr, gi = gs[k] * (1 - fr) + gs[k + 1] * fr;
            phase += 2 * M_PI * (fi + cfoHz + driftHzPerSec * ((double)i / rate)) / rate;
            out[i] = cf32((float)(amp * gi * std::cos(phase) + sigma * nd(rng) + dc), (float)(amp * gi * std::sin(phase) + sigma * nd(rng) + dc * 0.5));
        }
        return out;
    }
};

struct VoiceBurst { int slot, pos; std::vector<uint8_t> bits; };

static DmrTelemetry receive(const std::vector<cf32>& x, double rate, size_t chunk, std::vector<std::string>* log = nullptr, std::vector<VoiceBurst>* voice = nullptr) {
    DmrReceiver rx;
    rx.setSilent(true);
    if (log) rx.setLogCallback([log](const std::string& s) { log->push_back(s); });
    if (voice) rx.setVoiceCallback([voice](int slot, int pos, const uint8_t* b) { voice->push_back({slot, pos, std::vector<uint8_t>(b, b + 27)}); });
    rx.configure(rate);
    std::vector<cf32> q(chunk);
    for (size_t i = 0; i < x.size(); i += chunk) {
        const size_t n = std::min(chunk, x.size() - i);
        for (size_t k = 0; k < n; k++) {
            auto r8 = [](float v) { return std::round(std::min(127.f, std::max(-128.f, v * 128.f))) / 128.f; };
            q[k] = cf32(r8(x[i + k].real()), r8(x[i + k].imag()));
        }
        rx.feed(q.data(), n);
    }
    DmrTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

int main() {
    const Tx tx;
    // ---- base station: two real CSBK preamble bursts, one per time slot, 60 times each, with a CACH in front of every burst
    {
        const char* preA = "53df0a83b7a8282c2509625014fdff57d75df5dcadde429028c87ae3341e24191c";   // 2308155 -> 2308195, colour code 5
        const char* preB = "51cf0ded894c0dec1ff8fcf294fdff57d75df5dcae7a16d064197982bf5824914c";   // 2308094 -> 2301
        std::vector<double> sym;
        std::vector<char> on;
        for (int rep = 0; rep < 40; rep++)
            for (int slot = 0; slot < 2; slot++) {
                const std::vector<int> c = cachBits(0, slot, 0);
                const std::vector<double> cs = symbolsOf(c), bs = symbolsOf(bitsOf(slot == 0 ? preA : preB));
                sym.insert(sym.end(), cs.begin(), cs.end());
                sym.insert(sym.end(), bs.begin(), bs.end());
                on.insert(on.end(), cs.size() + bs.size(), 1);
            }
        for (double cfo : {0.0, -2500.0, 3300.0}) {
            const std::vector<cf32> x = tx.modulate(sym, on, 2.4e6, cfo, 28, 0.0, 5);
            std::vector<std::string> log;
            const DmrTelemetry t = receive(x, 2.4e6, 65536, &log);
            int a = 0, b = 0, slotWrong = 0;
            for (const DmrCall& c : t.callLog) {
                if (c.kind != 4) continue;
                if (c.src == 2308155 && c.dst == 2308195) { a++; if (c.slot != 1) slotWrong++; }
                else if (c.src == 2308094 && c.dst == 2301) { b++; if (c.slot != 2) slotWrong++; }
                CHECK(c.cc == 5, "control entry with colour code %d", c.cc);
            }
            printf("real CSBK bursts, base station, CFO %+5.0f Hz: state %d link '%s' CC %d, preambles %d + %d logged, slot labels wrong %d, BPTC ok %llu failed %llu, CRC bad %llu, CFO %+.0f\n", cfo, t.state, t.link.c_str(), t.cc,
                   a, b, slotWrong, (unsigned long long)t.bptcOk, (unsigned long long)t.bptcFail, (unsigned long long)t.crcBad, t.cfoHz);
            CHECK(t.state == 2 && t.link == "base station" && t.cc == 5, "base station not recognised: state %d link '%s' cc %d", t.state, t.link.c_str(), t.cc);
            CHECK(t.slotsLocked == 2, "%d slots locked", t.slotsLocked);
            CHECK(a == 1 && b == 1, "preambles logged %d and %d (the same control message repeated within 3 s is logged once)", a, b);
            CHECK(t.bptcOk >= 70, "only %llu of 80 bursts decoded", (unsigned long long)t.bptcOk);
            CHECK(slotWrong == 0, "%d control entries on the wrong slot (the TDMA channel bit of the CACH)", slotWrong);
            CHECK(t.bptcFail == 0 && t.crcBad == 0 && t.golayFail == 0, "damaged blocks: BPTC %llu, CRC %llu, slot type %llu", (unsigned long long)t.bptcFail, (unsigned long long)t.crcBad, (unsigned long long)t.golayFail);
            CHECK(std::fabs(t.cfoHz - cfo) < 120, "carrier offset %.0f Hz, sent %.0f", t.cfoHz, cfo);
            CHECK(t.syncCount[1] >= 60 && t.syncCount[0] == 0, "BS data syncs %llu, voice %llu", (unsigned long long)t.syncCount[1], (unsigned long long)t.syncCount[0]);
        }
    }

    // ---- transmitters whose pulse shape is not the one of the standard: the whole raised cosine in the transmitter, and a Gaussian filter
    {
        const char* preA = "53df0a83b7a8282c2509625014fdff57d75df5dcadde429028c87ae3341e24191c";
        const char* preB = "51cf0ded894c0dec1ff8fcf294fdff57d75df5dcae7a16d064197982bf5824914c";
        std::vector<double> sym;
        std::vector<char> on;
        for (int rep = 0; rep < 60; rep++)
            for (int slot = 0; slot < 2; slot++) {
                const std::vector<double> cs = symbolsOf(cachBits(0, slot, 0)), bs = symbolsOf(bitsOf(slot == 0 ? preA : preB));
                sym.insert(sym.end(), cs.begin(), cs.end());
                sym.insert(sym.end(), bs.begin(), bs.end());
                on.insert(on.end(), cs.size() + bs.size(), 1);
            }
        for (int shape = 1; shape <= 2; shape++) {
            const Tx other(shape);
            const std::vector<cf32> x = other.modulate(sym, on, 2.4e6, 300.0, 30, 0.0, 13);
            const DmrTelemetry t = receive(x, 2.4e6, 65536);
            printf("real CSBK bursts, %-24s: state %d, BPTC ok %llu failed %llu fixed %llu, slot type lost %llu, deviation %.0f\n", shape == 1 ? "raised cosine transmitter" : "Gaussian transmitter", t.state,
                   (unsigned long long)t.bptcOk, (unsigned long long)t.bptcFail, (unsigned long long)t.bptcFixed, (unsigned long long)t.golayFail, t.devHz);
            CHECK(t.state == 2 && t.bptcOk + t.bptcFixed >= 105 && t.bptcFail == 0 && t.golayFail == 0, "pulse shape %d: %llu of 120 bursts decoded, %llu lost", shape, (unsigned long long)(t.bptcOk + t.bptcFixed), (unsigned long long)t.bptcFail);
        }
    }

    // ---- a transmitter that is not ideal: inner deviations off by a quarter, and a carrier that drifts 1.5 kHz per second (far more than any oscillator does: it checks the tracking)
    {
        const char* preA = "53df0a83b7a8282c2509625014fdff57d75df5dcadde429028c87ae3341e24191c";
        const char* preB = "51cf0ded894c0dec1ff8fcf294fdff57d75df5dcae7a16d064197982bf5824914c";
        std::vector<double> sym;
        std::vector<char> on;
        for (int rep = 0; rep < 60; rep++)
            for (int slot = 0; slot < 2; slot++) {
                const std::vector<double> cs = symbolsOf(cachBits(0, slot, 0)), bs = symbolsOf(bitsOf(slot == 0 ? preA : preB));
                sym.insert(sym.end(), cs.begin(), cs.end());
                sym.insert(sym.end(), bs.begin(), bs.end());
                on.insert(on.end(), cs.size() + bs.size(), 1);
            }
        struct V { const char* what; double inner, drift; };
        const V vs[] = {{"inner levels x0.75", 0.75, 0.0}, {"inner levels x1.25", 1.25, 0.0}, {"carrier drift +1.5 kHz/s", 1.0, 1500.0}, {"both", 0.85, -1500.0}};
        for (const V& v : vs) {
            const std::vector<cf32> x = tx.modulate(sym, on, 2.4e6, 500.0, 30, 0.0, 11, v.inner, v.drift);
            const DmrTelemetry t = receive(x, 2.4e6, 65536);
            printf("real CSBK bursts, %-24s: state %d, BPTC ok %llu failed %llu fixed %llu, slot type lost %llu, CFO %+.0f\n", v.what, t.state, (unsigned long long)t.bptcOk, (unsigned long long)t.bptcFail, (unsigned long long)t.bptcFixed,
                   (unsigned long long)t.golayFail, t.cfoHz);
            CHECK(t.state == 2 && t.bptcOk + t.bptcFixed >= 110 && t.bptcFail == 0 && t.golayFail == 0, "%s: %llu of 120 bursts decoded, %llu lost", v.what, (unsigned long long)(t.bptcOk + t.bptcFixed), (unsigned long long)t.bptcFail);
        }
    }

    // ---- mobile station: a real voice superframe (burst A with the voice sync, B to E with embedded link control, F), repeated 8 times in one time slot
    {
        const char* A = "aded847205ae0062959308849047f7d5dd57dfd9537a101efe3ed4206e153827e7";
        const char* emb[5] = {"78f8e0361b6519cdd55ad9c3301130a00030a91b7529dee349fbe3147e040bc9d1", "c762a2114c736c7a45f562c133617170a06057439c9df11e936ec26335ecf569bf",
                              "f30c872376d6102d4791df85442170c112200747b289e11dd5c2877046b1e36bcf", "e1e48370246e951422bda7c73511505223f3a07309cda701bdb6e4733318ef9122",
                              "d5098044132a3761cbc708807701100000000e211a1324cbacb5c675371ddee013"};
        std::vector<double> sym;
        std::vector<char> on;
        auto burst = [&](const char* hex) {
            const std::vector<double> s = symbolsOf(bitsOf(hex));
            sym.insert(sym.end(), 12, 0.0); on.insert(on.end(), 12, 0);          // the half of the slot where a handset does not transmit
            sym.insert(sym.end(), s.begin(), s.end()); on.insert(on.end(), s.size(), 1);
            sym.insert(sym.end(), 144, 0.0); on.insert(on.end(), 144, 0);        // the other slot
        };
        for (int rep = 0; rep < 8; rep++) { burst(A); for (const char* e : emb) burst(e); }
        for (double cfo : {0.0, 4000.0}) {
            const std::vector<cf32> x = tx.modulate(sym, on, 2.4e6, cfo, 25, 0.02, 7);
            std::vector<std::string> log;
            std::vector<VoiceBurst> voice;
            const DmrTelemetry t = receive(x, 2.4e6, 4096, &log, &voice);
            // the vocoder bits handed on are the 216 payload bits of the captured burst (108 before and 108 after the centre field), bit for bit
            {
                const char* all[6] = {A, emb[0], emb[1], emb[2], emb[3], emb[4]};
                for (int pos = 0; pos < 6; pos++) {
                    const std::vector<int> b = bitsOf(all[pos]);
                    std::vector<uint8_t> want(27, 0);
                    for (int i = 0; i < 216; i++) {
                        const int bit = i < 108 ? b[(size_t)i] : b[(size_t)(156 + i - 108)];
                        want[(size_t)(i / 8)] |= (uint8_t)(bit << (7 - i % 8));
                    }
                    int same = 0, other = 0;
                    for (const VoiceBurst& v : voice) if (v.pos == pos) { if (v.bits == want) same++; else other++; }
                    CHECK(same >= 6 && other == 0, "voice burst %c: %d callbacks with the captured bits, %d with other bits", 'A' + pos, same, other);
                }
                for (const VoiceBurst& v : voice) CHECK(v.slot == 1 || v.slot == 2, "voice callback slot %d", v.slot);
            }
            const DmrCall* call = nullptr;
            for (const DmrCall& c : t.callLog) if (c.kind == 0) call = &c;
            printf("real voice superframes, mobile station, CFO %+5.0f Hz: state %d link '%s' CC %d, call %u -> %u, voice frames %d, embedded LC ok %llu failed %llu, EMB ok %llu failed %llu\n", cfo, t.state, t.link.c_str(),
                   t.cc, call ? call->src : 0, call ? call->dst : 0, call ? call->voiceFrames : 0, (unsigned long long)t.embLcOk, (unsigned long long)t.embLcFail, (unsigned long long)t.embOk, (unsigned long long)t.embFail);
            CHECK(t.link == "mobile" && t.cc == 1, "link '%s' cc %d", t.link.c_str(), t.cc);
            CHECK(call != nullptr, "no voice call logged");
            if (call) {
                CHECK(call->src == 2145016 && call->dst == 2149 && call->idsKnown, "call IDs %u -> %u (captured: 2145016 -> 2149)", call->src, call->dst);
                CHECK(call->lateEntry && !call->terminated, "no voice header or terminator was sent: late entry %d, terminated %d", (int)call->lateEntry, (int)call->terminated);
                CHECK(call->voiceFrames >= 3 * 6 * 6, "voice frames %d of %d sent", call->voiceFrames, 3 * 6 * 8);
            }
            CHECK(t.embLcOk >= 6 && t.embLcFail == 0, "embedded LC ok %llu failed %llu", (unsigned long long)t.embLcOk, (unsigned long long)t.embLcFail);
            CHECK(t.syncCount[2] >= 6, "mobile voice syncs %llu", (unsigned long long)t.syncCount[2]);
        }
    }

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
