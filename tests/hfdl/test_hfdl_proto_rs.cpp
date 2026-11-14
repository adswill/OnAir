// Golden-vector test of the HFDL layers (rust/onair_hfdl behind the D::hfdl* functions): rates and burst sizes, the A / M sequences and the preamble, the
// scrambler, the interleavers, the convolutional encoder, the Viterbi decoder (random soft values and encoded data with noise, every rate), the bit chain of every
// rate, bursts, the FCS, the station table and masks, and the PDU parser on built (SPDU, MPDU up and down, logon, unnumbered data with performance / frequency
// data, ACARS, system table), mutated and random PDUs. ONAIR_DIFF_ITERS (default 200000) inputs per family. Every result goes into a digest
// (tests/common/golden.h; doubles rounded to 1e-6, floats widened) compared with the one recorded from the C++ layers the Rust code replaced.
#include "dect2/hfdl/hfdl_proto.h"
#include "../common/golden.h"
#include "../common/ordered.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
namespace D = dect2;
static golden::Digest dig;
static std::mt19937 g(2718);
static uint8_t rb() { return (uint8_t)g(); }

static void addMsg(const dect2::HfdlMessage& a) {
    dig.add(a.squitter, a.uplink, a.gsId, a.acId, a.icao, a.kind, a.hfnpdu, a.summary, a.detail, a.frameIndex, a.utcSync, a.activeKhz, a.acars);
    dig.add(a.reg, a.flight, a.label, a.labelText, a.msgNum, a.acarsText, a.blockId, a.acarsCrcOk, a.hasPos, a.lat, a.lon, a.hasAlt);
    dig.add(a.altFt, a.posKind, a.utcSec);
}

static std::vector<uint8_t> mutate(std::vector<uint8_t> v) {
    for (int k = (int)(g() % 4); k > 0; k--) {
        if (v.empty()) { v.push_back(rb()); continue; }
        switch (g() % 5) {
        case 0: v[g() % v.size()] = rb(); break;
        case 1: v[g() % v.size()] ^= (uint8_t)(1u << (g() % 8)); break;
        case 2: v.erase(v.begin() + (long)(g() % v.size())); break;
        case 3: ORD_CALL(v.insert, v.begin() + (long)(g() % (v.size() + 1)), rb()); break;   // ORD_*: draws in a fixed order (arguments and operands are not evaluated in one)
        default: v.resize(g() % (v.size() + 1)); break;
        }
    }
    return v;
}
static std::vector<uint8_t> rndBytes(size_t n) { std::vector<uint8_t> v(n); for (auto& b : v) b = rb(); return v; }
static std::string rndStr(size_t n, const char* a, size_t al) { std::string s; for (size_t i = 0; i < n; i++) s += a[g() % al]; return s; }
#define RS(n, lit) rndStr((n), lit, sizeof(lit) - 1)
static double rndCoord(double r) { return ((double)(g() % 2000001) / 1000000.0 - 1.0) * r; }

static void testBitLayer(long iters) {
    for (int m = -3; m < 12; m++) {
        const auto &a = D::hfdlRate(m);
        dig.add(a.m1, a.bps, a.bitsPerSym, a.quarter, a.twoSlots, a.blocks, a.colShift);
        dig.add(D::hfdlBurstSyms(m), D::hfdlCodedBits(m), D::hfdlPayloadOctets(m));
    }
    for (int k = 0; k < 127; k++) {
        dig.add(D::hfdlABit(k));
        for (int s = 0; s < 8; s++) dig.add(D::hfdlMBit(s, k));
    }
    for (int m = 0; m < 8; m++) {
        dig.add(D::hfdlPreambleRef(m));
        dig.add(D::hfdlInterleave(m));
    }
    for (int n = 0; n < 1200; n++) {
        std::vector<int8_t> a;
        D::hfdlScramblerSigns(n, a);
        dig.add(a);
    }
    for (long i = 0; i < iters / 20; i++) {
        std::vector<uint8_t> bits(g() % 400);
        for (auto& x : bits) x = g() & 1;
        if (g() % 4 == 0) for (auto& x : bits) x |= (uint8_t)(g() % 4 ? 0 : 0xFE);   // only the low bit counts
        dig.add(D::hfdlConvEncode(bits));
    }
    // Viterbi: random soft values, and encoded data with noise
    for (long i = 0; i < iters / 20; i++) {
        const int n = (int)(g() % 300);
        std::vector<float> soft((size_t)n * 2);
        const int kind = (int)(g() % 3);
        if (kind == 0) { for (auto& s : soft) s = (float)((int)(g() % 2001) - 1000) / 100.f; }
        else {
            std::vector<uint8_t> bits((size_t)n);
            for (auto& x : bits) x = g() & 1;
            const auto c = D::hfdlConvEncode(bits);
            const float sigma = kind == 1 ? 0.5f : 1.5f;
            for (size_t j = 0; j < soft.size(); j++) soft[j] = (c[j] ? -1.f : 1.f) + sigma * (float)((int)(g() % 2001) - 1000) / 500.f;
        }
        dig.add(D::hfdlViterbi(soft, n));
    }
    // the bit chain of every rate: encode -> channel soft values (with noise, some erased) -> decode
    for (int m = 0; m < 8; m++) {
        for (long i = 0; i < iters / 400 + 2; i++) {
            std::vector<uint8_t> pdu(g() % (size_t)(D::hfdlPayloadOctets(m) + 1));
            for (auto& x : pdu) x = rb();
            const auto sa = D::hfdlBurst(m, pdu);
            dig.add((uint64_t)sa.size());
            for (const auto& z : sa) dig.add(z.real(), z.imag());
            const int N = D::hfdlCodedBits(m);
            std::vector<float> chan((size_t)N);
            const int mode = (int)(g() % 3);
            for (auto& c : chan) {
                if (mode == 0) { c = (float)((int)(g() % 2001) - 1000) / 250.f; continue; }
                const float sign = g() % 2 ? 1.f : -1.f;
                const float mag = 1.f + (float)(g() % 100) / 50.f;
                const float gate = mode == 2 && g() % 20 == 0 ? 0.f : 1.f;
                c = sign * mag * gate;
            }
            dig.add(D::hfdlDecodeSoft(m, chan));
        }
    }
}

static void testFcsStations(long iters) {
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> v = rndBytes(g() % 120 + 2);
        const size_t n = v.size() - 2;
        dig.add(D::hfdlFcs(v.data(), n));
        if (g() % 2) { const uint16_t f = D::hfdlFcs(v.data(), n); v[n] = (uint8_t)f; v[n + 1] = (uint8_t)(f >> 8); if (g() % 3 == 0) v[g() % v.size()] ^= 1; }
        dig.add(D::hfdlFcsOk(v.data(), n));
    }
    {
        const auto& a = D::hfdlStations();
        dig.add((uint64_t)a.size());
        for (const auto& st : a) dig.add(st.id, std::string(st.name), st.lat, st.lon, st.khz);
    }
    for (int gs = -2; gs < 135; gs++) {
        const auto *a = D::hfdlStation(gs);
        dig.add(a != nullptr); if (a) dig.add(a->id);
        for (int k = 0; k < 200; k++) {
            const uint32_t mask = k < 20 ? 1u << k : (uint32_t)g();
            dig.add(D::hfdlMaskKhz(gs, mask));
        }
    }
    for (int k = -5; k < 25000; k++) dig.add(D::hfdlStationOnKhz(k));
}

static std::vector<uint8_t> rndHfnpdu() {
    std::vector<uint8_t> h;
    const int kind = (int)(g() % 8);
    const std::string fl = RS(g() % 8, "ABCDEFGHJKLMNPRSTUVWXYZ0123456789 ");
    const std::vector<D::HfdlSquitterGs> heard = {{(int)(g() % 20), true, (uint32_t)(g() & 0xFFFFF)}, {(int)(g() % 20), true, (uint32_t)(g() & 0xFFFFF)}, {(int)(g() % 130), false, (uint32_t)(g() & 0xFFFFF)}};
    switch (kind) {
    case 0: h = ORD_CALL(D::hfdlPerfData, fl, rndCoord(90), rndCoord(180), (int)(g() % 90000), (int)(g() % 130), (int)(g() % 9)); break;
    case 1: h = ORD_CALL(D::hfdlFreqData, fl, rndCoord(90), rndCoord(180), (int)(g() % 90000), heard); break;
    case 2: h = ORD_CALL(D::hfdlAcarsHfnpdu, RS(7, "-.ABCDEFGHJ0123456789"), RS(2, "ABCH1789_d"), (char)('0' + g() % 10),
                                         g() % 2 ? RS(g() % 60, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 /.\r\n-") : std::string("POS N52123E004567,1234,FL350"), false); break;
    case 3: h = {0xFF, 0xD0, (uint8_t)g(), rb(), rb(), rb(), rb()}; break;
    case 4: h = {0xFF, (uint8_t)(g() % 2 ? 0xD2 : 0xDE)}; break;
    case 5: h = {0xFF, rb()}; break;
    case 6: h = rndBytes(g() % 8); break;
    default: h = rndBytes(g() % 60); h.insert(h.begin(), 0xFF); break;
    }
    return h;
}
static std::vector<uint8_t> rndLpdu() {
    switch (g() % 9) {
    case 0: return D::hfdlLogonRequest(g() & 0xFFFFFF);
    case 1: return ORD_CALL(D::hfdlLogonConfirm, g() & 0xFFFFFF, (int)(g() % 256));
    case 2: case 3: case 4: return D::hfdlUnnumbered(rndHfnpdu());
    case 5: { auto b = rndBytes(g() % 8); const uint8_t t[] = {0x1D, 0x4F, 0x5F, 0x2F, 0x3F, 0xBF, 0x8F, 0x9F}; return D::hfdlLpdu(t[g() % 8], b); }
    case 6: { auto h = rndHfnpdu(); return D::hfdlLpdu(0x1D, h); }
    case 7: return ORD_CALL(D::hfdlLpdu, rb(), rndBytes(g() % 12));
    default: return mutate(D::hfdlUnnumbered(rndHfnpdu()));
    }
}
static std::vector<uint8_t> rndPdu() {
    switch (g() % 6) {
    case 0: {
        D::HfdlSquitterSpec s;
        s.gsId = (int)(g() % 130); s.utc = g() % 2; s.frameIndex = (int)(g() % 5000); s.changeNote = (int)(g() % 4); s.version = (int)(g() % 4); s.mask = g() & 0xFFFFF;
        s.systable = (int)(g() % 4096); s.other[0] = {(int)(g() % 130), g() % 2 == 0, (uint32_t)(g() & 0xFFFFF)}; s.other[1] = {(int)(g() % 130), g() % 2 == 0, (uint32_t)(g() & 0xFFFFF)};
        auto b = D::hfdlSpdu(s);
        if (g() % 4 == 0) { b[0] = (uint8_t)(b[0] | (g() % 2 ? 0 : 0x20)); const uint16_t f = D::hfdlFcs(b.data(), 64); b[64] = (uint8_t)f; b[65] = (uint8_t)(f >> 8); }
        if (g() % 3 == 0) b.resize(66 + g() % 10, 0);
        return b;
    }
    case 1: case 2: { std::vector<std::vector<uint8_t>> l; for (size_t i = g() % 5; i > 0; i--) l.push_back(rndLpdu()); return ORD_CALL(D::hfdlMpduDown, (int)(g() % 130), (int)(g() % 256), l); }
    case 3: case 4: { std::vector<std::vector<uint8_t>> l; for (size_t i = g() % 5; i > 0; i--) l.push_back(rndLpdu()); return ORD_CALL(D::hfdlMpduUp, (int)(g() % 130), (int)(g() % 256), l); }
    default: return rndBytes(g() % 90);
    }
}

static bool testParse(long iters) {
    long parsed = 0, msgs = 0;
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> pdu = rndPdu();
        if (g() % 2) pdu = mutate(pdu);
        if (g() % 5 == 0) for (size_t k = g() % 40; k > 0; k--) pdu.push_back(0);   // padding after the PDU
        std::vector<dect2::HfdlMessage> a;
        const bool oa = D::hfdlParsePdu(pdu.data(), pdu.size(), a);
        dig.add(oa, (uint64_t)a.size());
        for (const auto& m : a) addMsg(m);
        parsed += oa; msgs += (long)a.size();
    }
    printf("  parse: %ld of %ld PDUs accepted, %ld messages\n", parsed, iters, msgs);
    if (parsed <= iters / 4) { printf("too few PDUs accepted (%ld): the generator is off\n", parsed); return false; }
    return true;
}

int main() {
    const char* it = getenv("ONAIR_DIFF_ITERS");
    const long iters = it ? atol(it) : 200000;
    testBitLayer(iters);
    testFcsStations(iters);
    if (!testParse(iters)) return 1;
    return golden::check("hfdl_proto_rs", dig, 0x42e935f56ae8bfd9ull, !it);
}
