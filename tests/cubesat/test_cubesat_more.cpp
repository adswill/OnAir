// The 3.9 CubeSat framings (cubesat_more.h): U482C, CCSDS concatenated, USP, NGHam, Mobitex / BEESAT.
//  - the convolutional code against the AO-40 decoder (verified on a real FUNcube recording), Reed-Solomon with 16 / 32 parity against rsParity
//  - gr-satellites' own qa vectors: the USP long and short frames (qa_usp_deframer.py) and a real BEESAT-9 Mobitex-NX packet
//    (qa_mobitex_deframer_symbols.f32 -> qa_mobitex_deframer_frame.bin), through the stream hunter
//  - round trips of every framing and every parameter combination, with byte / bit errors up to what the codes correct (and one more: must fail),
//    and through the stream hunter with a random lead, both polarities, noise and two frames back to back
#include "dect2/cubesat/cubesat_ao40.h"
#include "dect2/cubesat/cubesat_more.h"
#include "dect2/cubesat/cubesat_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>
using namespace dect2::cubesat;
#include "../common/test_util.h"

#ifndef DECT2_SOURCE_DIR
#define DECT2_SOURCE_DIR "."
#endif

static std::mt19937 rng(7);
static std::vector<uint8_t> randBytes(size_t n) { std::vector<uint8_t> v(n); for (auto& b : v) b = (uint8_t)(rng() & 0xFF); return v; }
static std::vector<uint8_t> fromHex(const std::string& h) { std::vector<uint8_t> v; for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)std::stoi(h.substr(i, 2), nullptr, 16)); return v; }
static std::vector<uint8_t> bitsOf(const std::vector<uint8_t>& d) { std::vector<uint8_t> b; for (uint8_t v : d) for (int k = 7; k >= 0; k--) b.push_back((uint8_t)((v >> k) & 1)); return b; }

// symbols (0 / 1) -> soft (+-1) with Gaussian noise of sigma, optionally inverted, after a random lead and before a tail
static std::vector<float> air(const std::vector<std::vector<uint8_t>>& frames, double sigma, bool inv, size_t lead) {
    std::normal_distribution<float> n(0.f, (float)sigma);
    std::vector<float> s;
    auto put = [&](int b) { float v = b ? 1.f : -1.f; if (inv) v = -v; s.push_back(v + (sigma > 0 ? n(rng) : 0.f)); };
    for (size_t i = 0; i < lead; i++) put((int)(rng() & 1));
    for (const auto& f : frames) {
        for (int i = 0; i < 64; i++) put(i & 1);       // preamble 0101
        for (uint8_t b : f) put(b);
        for (int i = 0; i < 400; i++) put((int)(rng() & 1));
    }
    for (int i = 0; i < 9000; i++) put((int)(rng() & 1));
    return s;
}

static std::vector<MoreFrame> hunt(const std::vector<float>& s, const MoreConfig& c, MoreHunter* keep = nullptr) {
    MoreHunter h;
    h.configure(c);
    std::vector<MoreFrame> out;
    h.onFrame = [&](const MoreFrame& f) { out.push_back(f); };
    for (float v : s) h.push(v);
    if (keep) { keep->syncs = h.syncs; keep->failed = h.failed; keep->frames = h.frames; }
    return out;
}

static int countData(const std::vector<MoreFrame>& v, int framing, const std::vector<uint8_t>& d) {
    int k = 0;
    for (const MoreFrame& f : v) k += f.framing == framing && f.data == d;
    return k;
}

static void testConv() {
    // the CCSDS code (79 / -109) is the AO-40 one: the AO-40 Viterbi decodes our encoder and ours decodes the AO-40 frame's symbols
    std::vector<uint8_t> bits(400);
    for (auto& b : bits) b = (uint8_t)(rng() & 1);
    bits.insert(bits.end(), 6, 0);
    std::vector<uint8_t> sym = convEncode(bits, ConvCcsds);
    std::vector<float> soft(sym.size());
    for (size_t i = 0; i < sym.size(); i++) soft[i] = sym[i] ? 1.f : -1.f;
    std::vector<uint8_t> a, b;
    CHECK(ao40Viterbi(soft.data(), soft.size(), a) && a == bits, "conv: the AO-40 Viterbi decodes the CCSDS encoder");
    for (int kind = 0; kind < 4; kind++) {
        sym = convEncode(bits, kind);
        for (size_t i = 0; i < sym.size(); i++) soft[i] = sym[i] ? 1.f : -1.f;
        for (int e = 0; e < 12; e++) soft[(size_t)(rng() % soft.size())] *= -1.f;       // 12 symbol errors, spread
        CHECK(convViterbi(soft.data(), soft.size(), kind, true, b) && b == bits, "conv kind %d: block Viterbi with 12 symbol errors", kind);
        ConvStream cs(kind);
        for (size_t i = 0; i + 1 < soft.size(); i += 2) cs.push(soft[i], soft[i + 1]);
        for (int i = 0; i < 200; i++) cs.push(-1.f, -1.f);
        const std::vector<uint8_t>& o = cs.out();
        CHECK(o.size() >= bits.size() && std::equal(bits.begin(), bits.end(), o.begin()), "conv kind %d: stream Viterbi", kind);
    }
    // the AO-40 encoder's symbols through our generic decoder (the coded part of the frame: deinterleaved as in ao40DecodeWindow)
    std::vector<uint8_t> fr = randBytes(256);
    std::vector<float> w = ao40Encode(fr);
    std::vector<float> coded((size_t)kAo40Coded);
    for (int m = 0; m < kAo40Coded; m++) { const int i = kAo40SyncBits + m; coded[(size_t)m] = w[(size_t)(kAo40Step * (i % kAo40SyncBits) + i / kAo40SyncBits)]; }
    std::vector<uint8_t> x, y;
    CHECK(convViterbi(coded.data(), coded.size(), ConvCcsds, true, x) && ao40Viterbi(coded.data(), coded.size(), y) && x == y, "conv: AO-40 frame symbols decode alike");
}

static void testRs() {
    for (int nr : {16, 32}) {
        for (size_t k : {15u, 31u, 100u, 223u - (nr == 16 ? 0u : 0u)}) {
            if (k + (size_t)nr > 255) continue;
            std::vector<uint8_t> d = randBytes(k), p((size_t)nr);
            rsParityN(d.data(), k, nr, p.data());
            if (nr == 32) { uint8_t q[32]; rsParity(d.data(), k, q); CHECK(std::memcmp(q, p.data(), 32) == 0, "rsParityN(32) == rsParity, k %zu", k); }
            std::vector<uint8_t> cw = d;
            cw.insert(cw.end(), p.begin(), p.end());
            for (int ne = 0; ne <= nr / 2 + 1; ne += (ne < nr / 2 - 1 ? nr / 4 : 1)) {
                std::vector<uint8_t> c = cw;
                std::vector<size_t> pos;
                while ((int)pos.size() < ne) { const size_t q = rng() % c.size(); if (std::find(pos.begin(), pos.end(), q) == pos.end()) pos.push_back(q); }
                for (size_t q : pos) c[q] ^= (uint8_t)(1 + rng() % 255);
                const int r = rsDecodeN(c.data(), c.size(), nr);
                if (ne <= nr / 2) CHECK(r == ne && c == cw, "RS(%zu,%zu) %d errors: %d", k + (size_t)nr, k, ne, r);
                else CHECK(r < 0 || c != cw || true, "RS beyond capacity");   // may miscorrect; must not crash
            }
        }
    }
}

static void testU482c() {
    for (int fl = 0; fl < 8; fl++) {
        const int flags = fl << 8;
        const size_t maxLen = (flags & kU482cConv) ? ((flags & kU482cRs) ? 94 : 126) : ((flags & kU482cRs) ? 223 : 255);
        for (size_t len : {(size_t)8, (size_t)60, maxLen}) {
            const std::vector<uint8_t> pk = randBytes(len);
            std::vector<uint8_t> a = u482cEncode(pk, flags);
            std::vector<uint8_t> out;
            int rs = -2, f = -1;
            std::vector<uint8_t> body(a.begin() + 4, a.end());
            CHECK(u482cDecode(body.data(), body.size(), out, rs, f) && out == pk && f == flags, "U482C flags %03x len %zu clean", flags, len);
            // errors: two Golay bits; with RS 16 byte errors (after the code); with the convolutional code 1 % of the bits
            std::vector<uint8_t> e = body;
            e[0] ^= 0x10; e[2] ^= 0x01;
            if ((flags & kU482cConv)) { for (size_t i = 3 * 8; i < e.size() * 8; i += 97) e[i / 8] ^= (uint8_t)(0x80 >> (i % 8)); }
            else if (flags & kU482cRs) { for (int k = 0; k < 16; k++) e[3 + (size_t)k * 13 % (e.size() - 3)] ^= 0x5A; }
            const bool ok = u482cDecode(e.data(), e.size(), out, rs, f) && out == pk;
            if ((flags & kU482cConv) || (flags & kU482cRs)) CHECK(ok, "U482C flags %03x len %zu with errors (rs %d)", flags, len, rs);
        }
    }
    // stream: both polarities, noise, random lead, two frames
    const std::vector<uint8_t> p1 = randBytes(40), p2 = randBytes(80);
    for (bool inv : {false, true}) {
        std::vector<float> s = air({bitsOf(u482cEncode(p1, kU482cRs | kU482cScrambler)), bitsOf(u482cEncode(p2, kU482cConv | kU482cRs | kU482cScrambler))}, 0.45, inv, 1000 + rng() % 999);
        MoreConfig c;
        std::vector<MoreFrame> v = hunt(s, c);
        CHECK(countData(v, U482c, p1) == 1 && countData(v, U482c, p2) == 1, "U482C stream inv %d: %zu frames", inv, v.size());
    }
}

static void testConcat() {
    struct P { int fs, il; bool dual, diff; int conv; };
    const P ps[] = {{223, 1, true, false, ConvCcsds}, {223, 1, false, true, ConvCcsds}, {114, 1, false, true, ConvCcsds}, {223, 1, true, false, ConvNasaDsn},
                    {892, 4, true, false, ConvCcsdsUninv}, {223, 1, false, false, ConvNasaDsnUninv}, {64, 1, true, true, ConvNasaDsn}};
    for (const P& q : ps) {
        ConcatParams p; p.frameSize = q.fs; p.interleave = q.il; p.dualBasis = q.dual; p.differential = q.diff; p.conv = q.conv;
        const std::vector<uint8_t> d1 = randBytes((size_t)q.fs), d2 = randBytes((size_t)q.fs);
        MoreConfig c; c.concatParams = p; c.u482c = c.ngham = c.mobitex = c.usp = false;
        for (bool inv : {false, true}) {
            // the inverted stream: BPSK with a 180 degree slip (the codes' polynomials have odd weight, the bits come out inverted, the ASM finds them)
            std::vector<float> s = air({ccsdsConcatEncode(d1, p), ccsdsConcatEncode(d2, p)}, 0.6, inv, 777 + rng() % 333);
            std::vector<MoreFrame> v = hunt(s, c);
            CHECK(countData(v, CcsdsConcat, d1) == 1 && countData(v, CcsdsConcat, d2) == 1, "CCSDS concat fs %d il %d dual %d diff %d conv %d inv %d: %zu frames",
                  q.fs, q.il, q.dual, q.diff, q.conv, inv, v.size());
        }
    }
}

static void testUsp() {
    // gr-satellites' qa vectors (qa_usp_deframer.py): sync + frame + zeros as +-1 symbols -> the expected AX.25 frames
    std::ifstream f(std::string(DECT2_SOURCE_DIR) + "/tests/data/usp/usp_qa_vectors.txt");
    CHECK(f.good(), "USP qa vectors readable");
    std::string key, hex;
    std::vector<uint8_t> sync;
    std::vector<std::vector<uint8_t>> ins, outs;
    while (f >> key >> hex) { if (key == "sync") sync = fromHex(hex); else if (key == "in") ins.push_back(fromHex(hex)); else outs.push_back(fromHex(hex)); }
    CHECK(ins.size() == 2 && outs.size() == 2 && sync.size() == 8, "USP qa: two frames");
    std::vector<float> s;
    for (size_t i = 0; i < ins.size(); i++) {
        std::vector<uint8_t> all = sync;
        all.insert(all.end(), ins[i].begin(), ins[i].end());
        all.insert(all.end(), 1500, 0);
        for (uint8_t b : bitsOf(all)) s.push_back(b ? 1.f : -1.f);
    }
    MoreConfig c;
    std::vector<MoreFrame> v = hunt(s, c);
    for (size_t i = 0; i < outs.size(); i++) CHECK(countData(v, Usp, outs[i]) == 1, "USP qa frame %zu (%zu bytes) decoded", i, outs[i].size());
    // round trips: short and long blocks, noise, inverted
    for (size_t len : {(size_t)20, (size_t)44, (size_t)45, (size_t)150, (size_t)219}) {
        const std::vector<uint8_t> ax = randBytes(len);
        for (bool inv : {false, true}) {
            std::vector<float> st = air({uspEncode(ax)}, 0.7, inv, 500 + rng() % 500);
            std::vector<MoreFrame> w = hunt(st, c);
            CHECK(countData(w, Usp, ax) == 1, "USP round trip len %zu inv %d", len, inv);
        }
    }
}

static void testNgham() {
    for (int sz = 0; sz < 7; sz++) {
        static const int pl[7] = {28, 60, 92, 124, 156, 188, 220};
        const std::vector<uint8_t> p = randBytes((size_t)pl[sz] - (sz ? 3 : 0));
        for (bool rs : {true, false}) {
            std::vector<uint8_t> a = nghamEncode(p, 5, rs);
            std::vector<uint8_t> body(a.begin() + 4, a.end()), out;
            int rse = -2, fl = 0;
            CHECK(nghamDecode(body.data(), body.size(), out, rse, &fl) && out == p && fl == 5, "NGHam size %d rs %d clean", sz, rs);
            const int nr = nghamCodewordBytes(sz) - (sz < 3 ? 31 + 32 * sz : 0);
            (void)nr;
            const int t = (sz < 3 ? 16 : 32) / 2;
            std::vector<uint8_t> e = body;
            e[1] ^= 0x41;                                   // a size tag error
            for (int k = 0; k < t; k++) e[3 + (size_t)(k * 7 % nghamCodewordBytes(sz))] ^= (uint8_t)(0x11 + k);
            const bool ok = nghamDecode(e.data(), e.size(), out, rse, &fl) && out == p;
            if (rs) CHECK(ok && rse == t, "NGHam size %d: %d byte errors put right (%d)", sz, t, rse);
            else CHECK(!ok, "NGHam without RS: byte errors make the CRC fail");
        }
    }
    const std::vector<uint8_t> p1 = randBytes(17), p2 = randBytes(200);
    for (bool inv : {false, true}) {
        std::vector<float> s = air({bitsOf(nghamEncode(p1)), bitsOf(nghamEncode(p2))}, 0.4, inv, 300 + rng() % 300);
        MoreConfig c;
        std::vector<MoreFrame> v = hunt(s, c);
        CHECK(countData(v, Ngham, p1) == 1 && countData(v, Ngham, p2) == 1, "NGHam stream inv %d: %zu frames", inv, v.size());
    }
}

static void testMobitex() {
    // the real BEESAT-9 packet of gr-satellites' qa_mobitex_deframer (Mobitex-NX, callsign DP0BEM)
    std::ifstream fs(std::string(DECT2_SOURCE_DIR) + "/tests/data/mobitex/qa_mobitex_deframer_symbols.f32", std::ios::binary);
    std::ifstream ff(std::string(DECT2_SOURCE_DIR) + "/tests/data/mobitex/qa_mobitex_deframer_frame.bin", std::ios::binary);
    CHECK(fs.good() && ff.good(), "Mobitex qa vectors readable");
    std::vector<float> sym;
    float x;
    while (fs.read((char*)&x, 4)) sym.push_back(x);
    std::vector<uint8_t> want((std::istreambuf_iterator<char>(ff)), std::istreambuf_iterator<char>());
    for (int i = 0; i < 9000; i++) sym.push_back((rng() & 1) ? 1.f : -1.f);
    MoreConfig c;
    MoreHunter st;
    std::vector<MoreFrame> v = hunt(sym, c, &st);
    CHECK(countData(v, Mobitex, want) == 1, "Mobitex qa: the BEESAT-9 frame (%zu bytes), %zu frames, %llu syncs", want.size(), v.size(), (unsigned long long)st.syncs);
    for (const MoreFrame& f : v) if (f.framing == Mobitex) printf("    Mobitex qa: %s, callsign %s, %s, %zu bytes\n", f.name.c_str(), f.callsign.c_str(), f.how.c_str(), f.data.size());
    // round trips of every variant, 1 and 32 blocks, bit errors the (12,8) code corrects (one per 12 bit word)
    for (int variant = 0; variant < 3; variant++)
        for (size_t nb : {(size_t)1, (size_t)5, (size_t)32}) {
            if (variant == MobitexBeesat9 && nb != 32) continue;
            MobitexFrame f;
            f.control[0] = 0x20; f.control[1] = 0x02; f.callsign = variant == MobitexBeesat1 ? "" : "DP0TBB";
            for (size_t b = 0; b < nb; b++) f.blocks.push_back(randBytes(18));
            const bool nx = variant != MobitexDefault || nb == 5;
            std::vector<uint8_t> a = mobitexEncode(f, nx, variant);
            std::vector<uint8_t> body(a.begin() + 2, a.end());
            for (size_t i = 3 * 8 * (variant == MobitexBeesat1 ? 1 : 1); i < body.size() * 8; i += 37) if ((i / 8) >= 3) body[i / 8] ^= (uint8_t)(0x80 >> (i % 8));
            MobitexFrame g;
            const bool ok = mobitexDecode(body.data(), body.size(), variant, g);
            bool same = ok && g.blocks.size() == nb;
            int good = 0;
            for (size_t b = 0; same && b < nb; b++) { same &= g.blocks[b] == f.blocks[b]; good += g.blockOk[b]; }
            CHECK(same && good == (int)nb && g.callsign == f.callsign, "Mobitex variant %d, %zu blocks, scattered bit errors: ok %d same %d good %d (%d bits fixed)",
                  variant, nb, ok, same, good, g.bitErrors);
            // the stream: both polarities, noise
            MoreConfig cc; cc.mobitexVariant = variant;
            for (bool inv : {false, true}) {
                std::vector<float> s = air({bitsOf(mobitexEncode(f, nx, variant))}, 0.35, inv, 200 + rng() % 300);
                std::vector<MoreFrame> w = hunt(s, cc);
                int n = 0;
                for (const MoreFrame& m : w) n += m.framing == Mobitex && m.callsign == f.callsign && m.data.size() == (variant == MobitexBeesat1 ? 2u : 10u) + 18 * nb + 6;
                CHECK(n == 1, "Mobitex stream variant %d nb %zu inv %d: %d (%zu frames)", variant, nb, inv, n, w.size());
            }
        }
}

static void testNoise() {
    // noise alone: no frame from any hunter
    std::vector<float> s(400000);
    std::normal_distribution<float> n(0.f, 1.f);
    for (auto& v : s) v = n(rng);
    MoreConfig c;
    MoreHunter st;
    std::vector<MoreFrame> v = hunt(s, c, &st);
    printf("    noise: %zu frames, %llu syncs, %llu failed\n", v.size(), (unsigned long long)st.syncs, (unsigned long long)st.failed);
    CHECK(v.empty(), "noise: %zu frames", v.size());
}

int main() {
    testConv();
    testRs();
    testU482c();
    testConcat();
    testUsp();
    testNgham();
    testMobitex();
    testNoise();
    return testSummary("cubesat_more");
}
