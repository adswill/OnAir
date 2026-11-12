// GW FSK (Globe Wireless, 100 Bd, 200 Hz shift, bursts). What is checked:
//  1. bursts built from the format as Rivet reads it: free-channel markers (station code and name), 2/106 text packets, 2/101 ship MMSI
//     packets (the digit coding of Rivet's displayGW_ShipMMSI), 5/41 traffic packets, a position report from 5/102, ten 5/86 fragments and
//     5/118, ACKs; through the receiver with impairments: dial error, off-centre tuning (tones at 600 or 2900 Hz), sender clock error,
//     reversed shift, 48 and 250 ksps;
//  2. a noise sweep (dial error and clock error): share of the packets read right, the 50 % and 90 % points;
//  3. noise alone, a carrier and FSK200/500 text give no packets (false decodes counted).
// Simulated signals only: the format is reverse-engineered by Rivet's authors and no recording was available here.
#include "x40_gen_rivet.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static Path path(double off, double drift = 0, double ppm = 0) { Path p; p.offsetHz = off; p.driftHzPerSec = drift; p.ppm = ppm; return p; }
static std::vector<int> text6(const char* s) { std::vector<int> v; for (int i = 0; i < 6; i++) v.push_back(gwCode(s[i])); return v; }
static std::string inverted(std::string s) { for (char& c : s) c = c == '1' ? '0' : '1'; return s; }
static int packets(const X40Telemetry& t) { return countKind(t, "Packet") + countKind(t, "Channel marker") + countKind(t, "ACK") + countKind(t, "Position"); }
static bool has(const X40Telemetry& t, const char* kind, const std::string& what) { for (auto& r : t.rows) if (r.kind == kind && r.text.find(what) != std::string::npos) return true; return false; }

int main() {
    // the MMSI coding of the generator read back by hand: 235012389 (nibbles per byte: low digit, then high digit)
    const auto mm = gwShipMmsi("235012389");
    const std::vector<std::string> b = {gwMarker(0xfa), gwPacket(2, 0, 106, text6("AB12E3")), gwPacket(2, 1, 101, mm), gwMarker(0xcc), gwPacket(2, 0, 106, text6("WN,7.<"))};
    struct Case { const char* what; double centre, fs, rxOff; Path p; bool rev; };
    const Case cases[] = {
        {"8 ksps, tones at 1600 / 1800 Hz", 1700, 8000, 0, path(0), false},
        {"dial +27 Hz", 1700, 8000, 0, path(27), false},
        {"off-centre: tones at 500 / 700 Hz, 48 ksps", 600, 48000, 0, path(0), false},
        {"off-centre: tones at 2800 / 3000 Hz", 2900, 8000, 0, path(0), false},
        {"clock +1 %", 1700, 8000, 0, path(-8, 0, 10000), false},
        {"shift reversed", 1700, 8000, 0, path(0), true},
        {"250 ksps, receiver offset -10 kHz", 1700, 250000, -10000, path(-10000 + 3), false},
    };
    for (const Case& c : cases) {
        std::vector<std::string> bb = b; if (c.rev) for (auto& s : bb) s = inverted(s);
        auto x = gwSignal(bb, c.centre, c.fs, c.p);
        addSnr(x, 15, c.fs, 3);
        const auto t = run(x40::Gw, x, c.fs, c.rxOff);
        const bool m1 = has(t, "Channel marker", "0xfa (KPH, San Francisco, USA)"), m2 = has(t, "Channel marker", "0xcc (LFI"), p1 = has(t, "Packet", "| AB12E3"), p2 = has(t, "Packet", "MMSI : 235012389"), p3 = has(t, "Packet", "| WN,7.<");
        printf("  %-44s markers %d %d, text %d %d, MMSI %d | %s\n", c.what, m1, m2, p1, p3, p2, t.headline.c_str());
        CHECK(m1 && m2 && p1 && p2 && p3, "%s: markers %d %d text %d %d MMSI %d", c.what, m1, m2, p1, p3, p2);
        CHECK(packets(t) == 5, "%s: %d rows for 5 bursts", c.what, packets(t));
    }
    // a position report: start (5/102), ten fragments (5/86), end (5/118); a 5/41 packet after a shore marker; an ACK
    { std::vector<std::string> q = {gwPacket(5, 0, 102, text6("$GLL,1"))};
      const char* frag[10] = {"234.56", ",N,012", "34.56,", "E,1234", "56,A*4", "B<<<<<", "123456", "7890,.", "**<<..", "AABBEE"};
      for (int i = 0; i < 10; i++) q.push_back(gwPacket(5, i & 1, 86, text6(frag[i])));
      q.push_back(gwPacket(5, 0, 118, text6("<<<<<<")));
      q.push_back(gwMarker(0x9c)); q.push_back(gwPacket(5, 1, 41, gwShipMmsi("538001234567"))); q.push_back("1010101010");
      auto x = gwSignal(q, 1700, 8000, path(5)); addSnr(x, 15, 8000, 4); const auto t = run(x40::Gw, x, 8000);
      const std::string pos = rowText(t, "Position");
      printf("  position report: '%s'\n", pos.c_str());
      CHECK(pos.find("234.56,N,01234.56,E,123456,A*4B<<<<<1234567890,.**<<..AABBEE<<<<<<") != std::string::npos, "position '%s'", pos.c_str());
      CHECK(countKind(t, "ACK") == 1, "%d ACK rows", countKind(t, "ACK"));
      CHECK(has(t, "Packet", "Traffic for : MMSI"), "5/41 after a shore marker"); }
    printf("noise sweep (dial -15 Hz, clock +300 ppm): packets right of 2 x 10\n");
    std::vector<std::pair<double, double>> pts;
    for (double snr = -6; snr <= 10.1; snr += 2) {
        int ok = 0, n = 0;
        for (unsigned seed : {1u, 2u}) {
            std::vector<std::string> q; std::vector<std::string> want;
            std::mt19937 r(seed);
            for (int i = 0; i < 10; i++) { char s[7]; for (int k = 0; k < 6; k++) s[k] = "0123456789AB"[r() % 12]; s[6] = 0; q.push_back(gwPacket(2, 0, 106, text6(s))); want.push_back(std::string("| ") + s); }
            auto x = gwSignal(q, 1700, 8000, path(-15, 0, 300)); addSnr(x, snr, 8000, 100 + seed);
            const auto t = run(x40::Gw, x, 8000);
            for (auto& w : want) ok += has(t, "Packet", w); n += 10;
        }
        pts.push_back({snr, (double)ok / n});
        printf("  %6.1f dB: %3d / %d\n", snr, ok, n);
    }
    printf("  GW FSK: 50 %% of the packets at %.1f dB, 90 %% at %.1f dB (signal to noise in 3 kHz)\n", snrAt(pts, 0.5), snrAt(pts, 0.9));
    CHECK(pts.back().second >= 0.9, "%.2f of the packets at %.0f dB", pts.back().second, pts.back().first);
    int falseRows = 0;
    for (unsigned seed : {7u, 8u}) { std::vector<cf32> n((size_t)(8000 * 60), cf32(0, 0)); addNoise(n, 1.0, seed); falseRows += packets(run(x40::Gw, n, 8000)); }
    { std::vector<std::pair<double, double>> seg = {{1600, 20.0}}; auto x = toneSeq(seg, 8000, path(0)); addSnr(x, 0, 8000, 9); falseRows += packets(run(x40::Gw, x, 8000)); }
    { auto x = f01Signal(randomGroups(3, 60), 1500, 8000, path(0)); addSnr(x, 15, 8000, 10); falseRows += packets(run(x40::Gw, x, 8000)); }
    printf("false decodes (2 x 60 s noise, a carrier, FSK200/500 text): %d rows\n", falseRows);
    CHECK(falseRows == 0, "%d rows from noise, a carrier or FSK200/500", falseRows);
    printf(fails ? "x40 gwfsk: %d FAILED\n" : "x40 gwfsk: ok\n", fails);
    return fails ? 1 : 0;
}
