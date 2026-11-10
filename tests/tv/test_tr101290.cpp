// TR 101 290 on simulated transport streams: a clean stream is clean, and each fault fires its check.
#include "dect2/tv/tr101290.h"
#include "dect2/tv/ts_proto.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
struct Str { int cc[8192] = {}; double t = 0; std::vector<uint8_t> out; std::vector<double> ts; double dt = 188.0 * 8 / 20e6;
    void pkt(int pid, const uint8_t* payload, int n, bool pusi, bool pcr = false, double pcrSec = 0) { uint8_t p[188]; memset(p, 0xFF, 188); p[0] = 0x47; p[1] = (uint8_t)((pusi ? 0x40 : 0) | (pid >> 8)); p[2] = (uint8_t)pid; int off = 4; int afc = 1; if (pcr) { afc = 3; p[4] = 7; p[5] = 0x10; const int64_t b = (int64_t)(pcrSec * 90000); p[6] = (uint8_t)(b >> 25); p[7] = (uint8_t)(b >> 17); p[8] = (uint8_t)(b >> 9); p[9] = (uint8_t)(b >> 1); p[10] = (uint8_t)((b & 1) << 7 | 0x7E); p[11] = 0; off = 12; } p[3] = (uint8_t)(afc << 4 | (cc[pid]++ & 15)); if (n > 0) memcpy(p + off, payload, std::min(n, 188 - off)); out.insert(out.end(), p, p + 188); ts.push_back(t); t += dt; } };
static std::vector<uint8_t> finish(std::vector<uint8_t> s) { int n = (int)s.size() + 4 - 3; s[1] = (uint8_t)(0xB0 | (n >> 8)); s[2] = (uint8_t)n; s.resize(s.size() + 4); uint32_t c = tsp::crc32(s.data(), (int)s.size() - 4); size_t e = s.size(); s[e - 4] = (uint8_t)(c >> 24); s[e - 3] = (uint8_t)(c >> 16); s[e - 2] = (uint8_t)(c >> 8); s[e - 1] = (uint8_t)c; return s; }
static void section(Str& s, int pid, const std::vector<uint8_t>& sec) { std::vector<uint8_t> v = {0}; v.insert(v.end(), sec.begin(), sec.end()); s.pkt(pid, v.data(), (int)v.size(), true); }
static std::vector<uint8_t> pat() { return finish({0, 0, 0, 0, 1, 0xC1, 0, 0, 0, 1, 0xE1, 0x00}); }
static std::vector<uint8_t> pmt() { return finish({2, 0, 0, 0, 1, 0xC1, 0, 0, 0xE1, 0x10, 0xF0, 0, 0x1B, 0xE1, 0x11, 0xF0, 0}); }
struct R { Tr290Monitor m; void run(const Str& s) { for (size_t i = 0; i < s.ts.size(); i++) m.feed(s.out.data() + i * 188, s.ts[i]); } uint64_t n(const char* name) const { for (auto& c : m.checks()) if (strstr(c.name, name)) return c.count; return ~0ull; } };
static Str good(double secs, double patEvery = 0.1) {
    Str s; double nextPat = 0, nextPcr = 0, nextPts = 0; uint8_t pes[20] = {0, 0, 1, 0xE0, 0, 0, 0x80, 0x80, 5, 0x21, 0, 1, 0, 1};
    while (s.t < secs) { if (s.t >= nextPat) { section(s, 0, pat()); section(s, 0x100, pmt()); section(s, 1, finish({1, 0, 0, 0, 0, 0xC1, 0, 0})); section(s, 0x10, finish({0x40, 0, 0, 0, 1, 0xC1, 0, 0, 0xF0, 0, 0xF0, 0})); section(s, 0x11, finish({0x42, 0, 0, 0, 1, 0xC1, 0, 0, 0, 1, 0xFF})); section(s, 0x12, finish({0x4E, 0, 0, 0, 1, 0xC1, 0, 0, 0, 1, 0, 1, 1, 0x4E})); uint8_t tdt[] = {0, 0x70, 0x70, 5, 0xDA, 0xC8, 0x00, 0x00, 0x00}; s.pkt(0x14, tdt, 9, true); nextPat += patEvery; }
        if (s.t >= nextPcr) { s.pkt(0x110, nullptr, 0, false, true, s.t); nextPcr += 0.02; }
        else if (s.t >= nextPts) { s.pkt(0x111, pes, 14, true); nextPts += 0.2; }
        else s.pkt(0x111, nullptr, 0, false); }
    return s;
}
int main() {
    { Str s = good(3); R r; r.run(s); for (auto& c : r.m.checks()) if (c.count) printf("clean stream: %s = %llu\n", c.name, (unsigned long long)c.count); uint64_t bad = 0; for (auto& c : r.m.checks()) bad += c.count; CHECK(bad == 0, "clean stream fired %llu", (unsigned long long)bad); printf("jitter %.3f ms\n", r.m.pcrJitterMs()); CHECK(r.m.pcrJitterMs() < 0.2, "jitter"); }
    { Str s = good(3); s.out[188 * 500] = 0x48; R r; r.run(s); CHECK(r.n("Sync_byte") >= 1, "sync byte"); }
    { Str s = good(3); s.out[188 * 900 + 3] ^= 0x03; R r; r.run(s); CHECK(r.n("Continuity") >= 1, "cc"); }
    { Str s = good(3); s.out[188 * 800 + 1] |= 0x80; R r; r.run(s); CHECK(r.n("Transport_error") == 1, "tei"); }
    { Str s = good(3, 0.8); R r; r.run(s); CHECK(r.n("PAT_error") >= 1, "PAT late"); CHECK(r.n("PMT_error") >= 1, "PMT late"); }
    { Str s = good(3); for (size_t i = 0; i < s.out.size() / 188; i++) if (((s.out[188 * i + 1] & 0x1F) << 8 | s.out[188 * i + 2]) == 0) { s.out[188 * i + 20] ^= 1; break; } R r; r.run(s); CHECK(r.n("CRC_error") >= 1, "crc"); }
    { Str s; s = good(0.3); for (int i = 0; i < 3000; i++) { s.pkt(0x111, nullptr, 0, false); } double t0 = s.t; while (s.t < t0 + 0.1) s.pkt(0x110, nullptr, 0, false, true, s.t); R r; r.run(s); CHECK(r.n("PCR_repetition") >= 1, "PCR late"); }
    { Str s = good(3); double j = 0; for (size_t i = 0; i < s.ts.size(); i++) if (((s.out[188 * i + 1] & 0x1F) << 8 | s.out[188 * i + 2]) == 0x110) { s.ts[i] += (j += 0.002) > 0.01 ? -0.002 : 0.002; } R r; r.run(s); CHECK(r.n("PCR_accuracy") >= 1, "PCR jitter"); }
    { Str s = good(1); for (int i = 0; i < 200; i++) s.pkt(0x444, nullptr, 0, false); R r; r.run(s); CHECK(r.n("Unreferenced") >= 1, "unreferenced %llu", (unsigned long long)r.n("Unreferenced")); }
    printf(fails ? "tr290 FAILED\n" : "tr290 passed\n");
    return fails ? 1 : 0;
}
