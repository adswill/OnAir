// ATSC PSIP: MGT, VCT source ids, EIT titles, ETT descriptions and STT through TsDemux, sections built by hand (simulated data).
#include "dect2/tv/ts.h"
#include "dect2/tv/ts_proto.h"
#include "dect2/tv/psip.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static std::vector<uint8_t> finish(std::vector<uint8_t> s) { int n = (int)s.size() + 4 - 3; s[1] = (uint8_t)(0xB0 | (n >> 8)); s[2] = (uint8_t)n; s.resize(s.size() + 4); uint32_t c = tsp::crc32(s.data(), (int)s.size() - 4); size_t e = s.size(); s[e - 4] = (uint8_t)(c >> 24); s[e - 3] = (uint8_t)(c >> 16); s[e - 2] = (uint8_t)(c >> 8); s[e - 1] = (uint8_t)c; return s; }
static std::vector<uint8_t> head(int tid, int ext) { std::vector<uint8_t> s = {(uint8_t)tid, 0, 0, (uint8_t)(ext >> 8), (uint8_t)ext, 0xC1, 0, 0, 0}; return s; }
static void pkts(TsDemux& d, int pid, const std::vector<uint8_t>& sec, int& cc) { size_t p = 0; bool first = true; while (p < sec.size()) { uint8_t t[188]; memset(t, 0xFF, 188); t[0] = 0x47; t[1] = (uint8_t)((first ? 0x40 : 0) | (pid >> 8)); t[2] = (uint8_t)pid; t[3] = 0x10 | (cc++ & 15); int off = 4; if (first) t[off++] = 0; size_t k = std::min<size_t>(188 - off, sec.size() - p); memcpy(t + off, sec.data() + p, k); p += k; first = false; d.feed(t); } }
static std::vector<uint8_t> mssText(const char* t) { std::vector<uint8_t> o = {1, 'e', 'n', 'g', 1, 0, 0, (uint8_t)strlen(t)}; o.insert(o.end(), t, t + strlen(t)); return o; }
int main() {
    TsDemux d; int cc = 0, ccE = 0, ccT = 0;
    auto mgt = head(0xC7, 0); mgt.insert(mgt.end(), {0, 2, 0x01, 0x00, 0xE0 | 0x1D, 0x00, 0xC1, 0, 0, 1, 0, 0xF0, 0, 0x02, 0x00, 0xE0 | 0x1E, 0x00, 0xC1, 0, 0, 1, 0, 0xF0, 0}); pkts(d, 0x1FFB, finish(mgt), cc);
    {   // TVCT: one channel, program 3, source 77
        auto v = head(0xC8, 100); v.push_back(1); std::vector<uint8_t> e(32, 0); e[24] = 0; e[25] = 3; e[28] = 0; e[29] = 77; e[30] = 0xFC; e[31] = 0; v.insert(v.end(), e.begin(), e.end()); pkts(d, 0x1FFB, finish(v), cc); }
    {   // STT: 18 leap seconds
        auto v = head(0xCD, 0); const uint32_t gps = 1400000000; v.insert(v.end(), {(uint8_t)(gps >> 24), (uint8_t)(gps >> 16), (uint8_t)(gps >> 8), (uint8_t)gps, 18, 0, 0}); pkts(d, 0x1FFB, finish(v), cc); }
    {   // EIT: source 77, one event
        auto v = head(0xCB, 77); v.push_back(1);
        const uint32_t gps = 1400001000; auto tt = mssText("Evening News");
        v.insert(v.end(), {0xC0, 42, (uint8_t)(gps >> 24), (uint8_t)(gps >> 16), (uint8_t)(gps >> 8), (uint8_t)gps, 0xE0, 0x0E, 0x10, (uint8_t)tt.size()});
        v.insert(v.end(), tt.begin(), tt.end()); v.insert(v.end(), {0xF0, 0}); pkts(d, 0x1D00, finish(v), ccE); }
    {   // ETT for that event, with an ETM id (source 77 << 16 | event 42 << 2 | 2)
        auto v = head(0xCC, 0); const uint32_t id = 77u << 16 | 42u << 2 | 2; v.insert(v.end(), {(uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id}); auto tt = mssText("Local headlines and weather."); v.insert(v.end(), tt.begin(), tt.end()); pkts(d, 0x1E00, finish(v), ccT); }
    auto ep = d.epg();
    CHECK(ep.count(3) && ep[3].size() == 1, "event stored against program 3: %zu programs", ep.size());
    if (ep.count(3) && !ep[3].empty()) {
        const auto& e = ep[3][0];
        CHECK(e.title == "Evening News", "title '%s'", e.title.c_str());
        CHECK(e.extended == "Local headlines and weather.", "ETT '%s'", e.extended.c_str());
        CHECK(e.duration == 3600, "duration %d", e.duration);
        CHECK(e.start == 1400001000 + 315964800 - 18, "start %lld", (long long)e.start);
        CHECK(e.eventId == 42, "event id %d", e.eventId);
    }
    {   // compressed text: left blank and flagged; damaged section refused
        const uint8_t comp[] = {1, 'e', 'n', 'g', 1, 1, 0, 2, 0x12, 0x34};
        bool c = false; std::string t = psip::mss(comp, sizeof comp, &c);
        CHECK(c && t.empty(), "compressed");
        const uint8_t utf[] = {1, 'e', 'n', 'g', 1, 0, 0x3F, 4, 0x00, 'A', 0x20, 0xAC};
        t = psip::mss(utf, sizeof utf, &c); CHECK(t == "A\xE2\x82\xAC", "UTF-16 '%s'", t.c_str());
        auto bad = finish(head(0xC7, 0)); bad[bad.size() - 2] ^= 1; psip::State s; CHECK(!psip::parseMgt(bad.data(), (int)bad.size(), s), "bad CRC accepted");
    }
    printf(fails ? "psip FAILED\n" : "psip passed\n");
    return fails ? 1 : 0;
}
