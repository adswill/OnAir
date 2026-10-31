// T2-MI / raw BB frame export of the DVB-T2 PLP (core/include/dect2/t2/t2mi_out.h): packet layout, CRC, TS framing at awkward frame sizes
// (not a multiple of 184 bytes), lost blocks, other PIDs in the stream, bad CRCs, and end to end through the generator and the receiver
// (noise, a carrier offset, T2-Base and T2-Lite sizes) with the exported frames compared with what the generator sent.
#include "dect2/t2/t2mi_out.h"
#include "dect2/t2/t2gen.h"
#include "dect2/t2/t2rx.h"
#include "dect2/common/stable_random.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> randBits(std::mt19937& g, size_t n) { std::vector<uint8_t> b(n); for (auto& v : b) v = g() & 1; return b; }

static void unitTests() {
    // CRC-32/MPEG-2 check value
    const uint8_t chk[] = {'1','2','3','4','5','6','7','8','9'};
    CHECK(t2mi::crc32(chk, 9) == 0x0376E6E7u, "crc check value %08x", t2mi::crc32(chk, 9));
    std::mt19937 g(5);
    // every kBch of the T2 FEC tables is a multiple of 8; also try sizes that make 1, 2 and 3 TS packets plus odd remainders
    const int sizes[] = {3072, 5232, 7032, 7200, 16008, 38688, 43200, 58192, 80, 96};
    for (int sz : sizes) {
        t2mi::Extract ex(0x1000);
        uint8_t cc = 0;
        std::vector<std::vector<uint8_t>> sent;
        std::vector<uint8_t> ts;
        for (int i = 0; i < 40; i++) {
            auto bb = t2mi::bbBytes(randBits(g, sz));
            sent.push_back(bb);
            if (i % 5 == 4) continue;   // a lost block: the counter still advances
            // other PIDs between the packets, and the counter of the T2-MI packet wraps over 255
            t2mi::toTs(t2mi::bbPacket((uint8_t)(i * 7), 3, (uint8_t)i, 2, i == 0, bb), 0x1000, cc, ts);
            if (i % 3 == 0) { uint8_t o[188] = {0x47, 0x1F, 0xFF, 0x10}; ts.insert(ts.end(), o, o + 188); }
            if (i % 7 == 0) { uint8_t o[188] = {0x47, 0x10, 0x01, 0x10}; ts.insert(ts.end(), o, o + 188); }   // PID 0x1001
        }
        CHECK(ts.size() % 188 == 0, "ts size");
        for (size_t o = 0; o < ts.size(); o += 188) ex.feed(&ts[o]);
        size_t k = 0;
        bool same = true;
        for (int i = 0; i < 40; i++) {
            if (i % 5 == 4) continue;
            if (k >= ex.frames.size() || ex.frames[k] != sent[i] || ex.frameIdx[k] != (uint8_t)i || ex.plpIds[k] != 2 || ex.counts[k] != (uint8_t)(i * 7)) same = false;
            k++;
        }
        CHECK(same && k == ex.frames.size() && ex.crcBad == 0, "size %d: %zu frames out of %zu, crcBad %llu", sz, ex.frames.size(), k, (unsigned long long)ex.crcBad);
    }
    // a stream that begins in the middle of a packet, with a flipped bit in one packet: that packet is dropped (crcBad), the rest is intact
    {
        std::vector<uint8_t> ts;
        uint8_t cc = 0;
        std::vector<std::vector<uint8_t>> sent;
        for (int i = 0; i < 10; i++) { auto bb = t2mi::bbBytes(randBits(g, 7032)); sent.push_back(bb); t2mi::toTs(t2mi::bbPacket((uint8_t)i, 0, (uint8_t)i, 0, false, bb), 0x30, cc, ts); }
        ts[188 * 20 + 50] ^= 0x10;   // inside packet 2 or 3
        t2mi::Extract ex(0x30);
        for (size_t o = 188 * 2; o < ts.size(); o += 188) ex.feed(&ts[o]);   // the start of the first packet is missing
        CHECK(ex.crcBad == 1, "bit flip: crcBad %llu", (unsigned long long)ex.crcBad);
        for (size_t k = 0; k < ex.frames.size(); k++) CHECK(ex.frames[k] == sent[ex.counts[k]], "frame %zu differs", k);
        CHECK(ex.frames.size() >= 6, "only %zu frames survive", ex.frames.size());
    }
}

struct Case { const char* name; int s1; bool shortFrame; int mod, cod; double snr; double cfoHz; double bw; };

static void runCase(const Case& c) {
    const double fn = nativeRateHz(c.bw);
    TxParams tp;
    tp.s1 = c.s1; tp.s2field1 = 1; tp.giIdx = 2; tp.l1Scrambled = true; tp.l1Mod = 1; tp.payload = true; tp.plpShort = c.shortFrame; tp.plpMod = c.mod; tp.plpCod = c.cod; tp.plpTi = 0;
    T2Generator gen(tp);
    T2Receiver rx;
    rx.configure(fn, c.bw);
    std::mutex mu;
    std::vector<PlpResult> results;
    rx.setPlpCallback([&](const PlpResult& r) { std::lock_guard<std::mutex> lk(mu); results.push_back(r); });
    std::mt19937 rng(11);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    std::map<int, std::vector<std::vector<uint8_t>>> sent;
    std::vector<cf32> frame;
    double sigma = 0, ph = 0;
    const int nFrames = 10;
    for (int i = 0; i < nFrames; i++) {
        gen.nextFrame(frame);
        sent[i & 0xff] = gen.lastBbFrames();
        if (i == 0) { double pw = 0; for (auto& v : frame) pw += std::norm(v); pw /= frame.size(); sigma = std::sqrt(pw / std::pow(10.0, c.snr / 10.0) / 2.0); }
        for (auto& v : frame) {
            v *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph = std::fmod(ph + 2 * M_PI * c.cfoHz / fn, 2 * M_PI);
            v += cf32{nd(rng), nd(rng)} * (float)sigma;
        }
        for (size_t o = 0; o < frame.size(); o += 1 << 16) rx.feed(frame.data() + o, std::min<size_t>(1 << 16, frame.size() - o));
        RxTelemetry tt; uint64_t sq = 0;
        for (int w = 0; w < 200; w++) {
            rx.telemetry(tt, sq);
            size_t n; { std::lock_guard<std::mutex> lk(mu); n = results.size(); }
            if ((int)n + 3 >= i) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    RxTelemetry t; uint64_t seq = 0; size_t seen = 0;
    for (int i = 0, still = 0; i < 1500 && still < 300; i++) {
        rx.telemetry(t, seq);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        size_t n; { std::lock_guard<std::mutex> lk(mu); n = results.size(); }
        still = n == seen ? still + 1 : 0; seen = n;
    }
    const std::string fb = std::string("t2mi_test_") + c.name + ".bbf", fm = std::string("t2mi_test_") + c.name + ".ts";
    T2Export eb, em;
    T2ExportConfig cb; cb.format = T2ExportConfig::Bbf; cb.file = fb;
    T2ExportConfig cm; cm.format = T2ExportConfig::T2mi; cm.file = fm; cm.pid = 0x123;
    CHECK(eb.configure(cb) && em.configure(cm), "%s: configure: %s %s", c.name, eb.error().c_str(), em.error().c_str());
    std::vector<std::vector<uint8_t>> expect;   // the BB frames that were decoded, as the generator sent them
    int wrong = 0;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& r : results) {
            if (r.blocks == 0) continue;
            eb.onPlp(r); em.onPlp(r);
            auto it = sent.find(r.t2Frame);
            for (size_t b = 0; b < r.frames.size(); b++) {
                if (r.frames[b].bits.empty()) continue;
                if (it == sent.end() || b >= it->second.size() || r.frames[b].bits != it->second[b]) { wrong++; continue; }
                expect.push_back(t2mi::bbBytes(r.frames[b].bits));
            }
        }
    }
    eb.close(); em.close();
    CHECK(wrong == 0, "%s: %d decoded blocks differ from what was sent", c.name, wrong);
    CHECK(expect.size() >= 6, "%s: only %zu blocks decoded", c.name, expect.size());
    // read both files back
    std::vector<uint8_t> raw, tsb;
    for (int k = 0; k < 2; k++) {
        FILE* f = fopen(k ? fm.c_str() : fb.c_str(), "rb");
        std::vector<uint8_t>& d = k ? tsb : raw;
        if (f) { uint8_t b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + n); fclose(f); }
        remove(k ? fm.c_str() : fb.c_str());
    }
    size_t want = 0; for (auto& e : expect) want += e.size();
    CHECK(raw.size() == want, "%s: raw file %zu bytes, expected %zu", c.name, raw.size(), want);
    size_t off = 0; bool ok = true;
    for (auto& e : expect) { if (off + e.size() > raw.size() || memcmp(&raw[off], e.data(), e.size())) ok = false; off += e.size(); }
    CHECK(ok, "%s: raw frames differ", c.name);
    t2mi::Extract ex(0x123);
    CHECK(tsb.size() % 188 == 0, "%s: ts size", c.name);
    for (size_t o = 0; o + 188 <= tsb.size(); o += 188) ex.feed(&tsb[o]);
    CHECK(ex.frames == expect && ex.crcBad == 0, "%s: T2-MI gave %zu frames, expected %zu (crcBad %llu)", c.name, ex.frames.size(), expect.size(), (unsigned long long)ex.crcBad);
    printf("%-12s %s %.0f dB, CFO %+.0f Hz: %zu frames exported, raw %zu bytes, T2-MI %zu TS packets\n", c.name, c.shortFrame ? "short " : "normal", c.snr, c.cfoHz, expect.size(), raw.size(), tsb.size() / 188);
}

int main() {
    unitTests();
    const Case cases[] = {
        {"normal_qpsk", 0, false, 0, 2, 20, 1500, 8},
        {"short_16qam", 0, true, 1, 1, 24, -2200, 8},
        {"normal_64q",  0, false, 2, 1, 28, 900, 7},
    };
    for (auto& c : cases) runCase(c);
    printf(fails ? "FAILED\n" : "all T2-MI export tests passed\n");
    return fails ? 1 : 0;
}
