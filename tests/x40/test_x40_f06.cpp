// FSK200/1000 (F06 and F06a: 200 Bd, 1000 Hz shift, 288-bit blocks with a CRC-16). What is checked:
//  1. the block layout and CRC: blocks built from the layout as Rivet unpacks it (an independent CRC-16/X-25 here) decode with "CRC OK",
//     the header block (block count, messages and where they start), F06 figure blocks, ASCII blocks, an F06a file (name, size, file CRC,
//     contents) and the switch between F06 and F06a;
//  2. impairments: dial error, off-centre tuning (tones centred at 1100 or 2700 Hz), sender clock error, drift, reversed shift (the inverted
//     sync word), 48 and 250 ksps; one wrong bit in a block is corrected by the CRC;
//  3. a noise sweep (dial error and clock error): share of the blocks with CRC OK and the right figures, the 50 % and 90 % points, and the
//     blocks shown with wrong figures and "CRC OK" (must be none);
//  4. noise alone, a carrier and an FSK200/500 signal give no blocks (false decodes counted).
// Simulated signals only: no recording of FSK200/1000 was available.
#include "x40_gen_rivet.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static Path path(double off, double drift = 0, double ppm = 0) { Path p; p.offsetHz = off; p.driftHzPerSec = drift; p.ppm = ppm; return p; }

// an F06 message: header block 0, block 1 (0x1b: figure traffic), then n figure blocks; the figure text of each block is returned
struct Msg { std::vector<std::vector<int>> blocks; std::vector<std::string> text; };
static Msg f06Message(unsigned seed, int n) {
    Msg m; std::mt19937 r(seed);
    uint8_t h[16] = {}; const int count = n + 3; h[0] = (uint8_t)(count >> 3); h[1] = (uint8_t)(((count & 7) << 5) | 1); h[2] = 0; h[3] = (uint8_t)(2 << 5);   // one message, at block #2
    m.blocks.push_back(f06Block(0, h, seed)); m.text.push_back("");
    uint8_t b1[16] = {0x1b}; m.blocks.push_back(f06Block(1, b1, seed + 1)); m.text.push_back("");
    for (int i = 0; i < n; i++) {
        if ((2 + i) % 16 == 0) { m.blocks.push_back(f06Block(2 + i, h, seed + 99)); m.text.push_back(""); n++; continue; }   // a header block every 16 blocks
        int g[14]; std::string t;
        for (int k = 0; k < 12; k++) { g[k] = (int)(r() % 1000); t += std::to_string(1000 + g[k]).substr(1); }
        g[12] = (int)(r() % 10); g[13] = (int)(r() % 10); t += std::to_string(g[12]) + std::to_string(g[13]);
        uint8_t d[16]; f06Digits(g, d);
        m.blocks.push_back(f06Block(2 + i, d, seed + 10 + (unsigned)i)); m.text.push_back(t);
    }
    return m;
}
// blocks decoded right: a Block row of that number whose figures match and whose CRC holds; wrongOk counts figures that do not match with CRC OK
static int blocksRight(const Msg& m, const X40Telemetry& t, int* wrongOk = nullptr) {
    int ok = 0;
    for (const auto& r : t.rows) {
        if (r.kind != "Block" || r.id.size() < 2) continue;
        const int idx = std::atoi(r.id.c_str() + 1);
        if (idx < 2 || idx >= (int)m.text.size()) continue;
        const bool crc = r.text.find("CRC OK") != std::string::npos, same = r.text.rfind(m.text[(size_t)idx], 0) == 0;
        if (crc && same) ok++;
        else if (crc && !same && wrongOk) (*wrongOk)++;
    }
    return ok;
}

int main() {
    // the CRC of the generator against the check value of CRC-16/X-25 ("123456789" -> 0x906E)
    { const uint8_t s[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'}; CHECK(crcX25(s, 9) == 0x906E, "CRC-16/X-25 check value %04x", crcX25(s, 9)); }
    const Msg m = f06Message(5, 20);
    struct Case { const char* what; double centre, fs, rxOff; Path p; bool rev; };
    const Case cases[] = {
        {"8 ksps, tones at 1000 / 2000 Hz", 1500, 8000, 0, path(0), false},
        {"dial -44 Hz", 1500, 8000, 0, path(-44), false},
        {"off-centre: tones at 600 / 1600 Hz, 48 ksps", 1100, 48000, 0, path(0), false},
        {"off-centre: tones at 2200 / 3200 Hz", 2700, 8000, 0, path(0), false},
        {"clock +500 ppm, drift -0.5 Hz/s", 1500, 8000, 0, path(10, -0.5, 500), false},
        {"shift reversed (inverted sync)", 1500, 8000, 0, path(0), true},
        {"250 ksps, receiver offset +30 kHz", 1500, 250000, 30000, path(30000 + 5), false},
    };
    for (const Case& c : cases) {
        auto x = f06Signal(m.blocks, c.centre, c.fs, c.p, c.rev);
        addSnr(x, 15, c.fs, 3);
        const auto t = run(x40::F06, x, c.fs, c.rxOff);
        const int ok = blocksRight(m, t);
        printf("  %-46s %2d / 20 blocks, header %d | %s\n", c.what, ok, countKind(t, "Header"), t.headline.c_str());
        CHECK(ok >= 19, "%s: %d of 20 blocks", c.what, ok);
        CHECK(countKind(t, "Header") == 2, "%s: %d header rows", c.what, countKind(t, "Header"));
    }
    // the header row: block count and where the message starts
    { auto x = f06Signal(m.blocks, 1500, 8000, path(0)); const auto t = run(x40::F06, x, 8000);
      std::string h; for (auto& r : t.rows) if (r.kind == "Header") h = r.text;
      CHECK(h.find("23 blocks, 1 message(s)") != std::string::npos && h.find("Message 1 at block #2") != std::string::npos, "header row '%s'", h.c_str()); }
    // one wrong bit in each of three blocks: corrected by the CRC
    { auto blocks = m.blocks; for (int b : {3, 8, 15}) blocks[(size_t)b][(size_t)(60 + 7 * b)] ^= 1;
      auto x = f06Signal(blocks, 1500, 8000, path(0)); addSnr(x, 25, 8000, 4); const auto t = run(x40::F06, x, 8000);
      int fixedRows = 0; for (auto& r : t.rows) if (r.text.find("one bit corrected") != std::string::npos) fixedRows++;
      const int ok = blocksRight(m, t);
      printf("  one wrong bit in 3 blocks: %d / 20 right, %d corrected\n", ok, fixedRows);
      CHECK(ok == 20 && fixedRows == 3, "bit errors: %d right %d corrected", ok, fixedRows); }
    // F06a: block 1 = 0x00 0x00, header with a message at block 2: block 2 the file header (size, name), block 3 the file CRC and text
    { std::vector<std::vector<int>> b;
      uint8_t h[16] = {}; h[0] = 0; h[1] = (uint8_t)((5 << 5) | 1); h[3] = (uint8_t)(2 << 5); b.push_back(f06Block(0, h, 1));
      uint8_t z[16] = {}; b.push_back(f06Block(1, z, 2));
      uint8_t fh[16] = {0, 0, 0, 40, 'N', 'O', 'T', 'E', '.', 'T', 'X', 'T', 0, 0, 0, 0}; b.push_back(f06Block(2, fh, 3));
      uint8_t c1[16] = {0xde, 0xad, 0xbe, 0xef, 'H', 'E', 'L', 'L', 'O', ' ', 'R', 'I', 'V', 'E', 'T', '!'}; b.push_back(f06Block(3, c1, 4));
      uint8_t c2[16] = {'L', 'I', 'N', 'E', ' ', 'T', 'W', 'O', 13, 10, 'E', 'N', 'D', 0, 0, 0}; b.push_back(f06Block(4, c2, 5));
      auto x = f06Signal(b, 1500, 8000, path(9)); addSnr(x, 15, 8000, 6); const auto t = run(x40::F06, x, 8000);
      const std::string files = rowText(t, "File"), blocks = rowText(t, "Block");
      printf("  F06a: %s  %s", files.c_str(), blocks.c_str());
      CHECK(files.find("file named NOTE.TXT, size 40 bytes | CRC OK") != std::string::npos, "F06a file header: '%s'", files.c_str());
      CHECK(files.find("0xdeadbeef") != std::string::npos, "F06a file CRC: '%s'", files.c_str());
      CHECK(blocks.find("HELLO RIVET!") != std::string::npos && blocks.find("LINE TWO<CR><LF>END<EOF>") != std::string::npos, "F06a text: '%s'", blocks.c_str());
      CHECK(t.headline.find("F06a") != std::string::npos, "F06a headline '%s'", t.headline.c_str()); }
    // ASCII blocks (block 1 starts 0x34 0x36)
    { std::vector<std::vector<int>> b; uint8_t h[16] = {}; h[1] = 1; b.push_back(f06Block(0, h, 1));
      uint8_t a1[16] = {0x34, 0x36, 'A', 'B', 'C', ' ', '1', '2', '3', ' ', 'X', 'Y', 'Z', '.', '.', '.'}; b.push_back(f06Block(1, a1, 2));
      uint8_t a2[16] = {'P', 'L', 'A', 'I', 'N', ' ', 'T', 'E', 'X', 'T', ' ', 'B', 'L', 'O', 'C', 'K'}; b.push_back(f06Block(2, a2, 3));
      auto x = f06Signal(b, 1500, 8000, path(0)); addSnr(x, 15, 8000, 7); const auto t = run(x40::F06, x, 8000);
      const std::string s = rowText(t, "Block");
      CHECK(s.find("PLAIN TEXT BLOCK | CRC OK") != std::string::npos, "ASCII blocks: '%s'", s.c_str()); }
    // noise sweep
    printf("noise sweep (dial +17 Hz, clock +200 ppm): blocks right\n");
    std::vector<std::pair<double, double>> pts; int wrongOk = 0;
    for (double snr = -6; snr <= 8.1; snr += 2) {
        int ok = 0, n = 0;
        for (unsigned seed : {1u, 2u}) {
            const Msg mm = f06Message(500 + seed, 20);
            auto x = f06Signal(mm.blocks, 1500, 8000, path(17, 0, 200));
            addSnr(x, snr, 8000, 90 + seed);
            const auto t = run(x40::F06, x, 8000);
            ok += blocksRight(mm, t, &wrongOk); n += 20;
        }
        pts.push_back({snr, (double)ok / n});
        printf("  %6.1f dB: %3d / %d\n", snr, ok, n);
    }
    printf("  FSK200/1000: 50 %% of the blocks at %.1f dB, 90 %% at %.1f dB (signal to noise in 3 kHz); %d wrong blocks with CRC OK\n", snrAt(pts, 0.5), snrAt(pts, 0.9), wrongOk);
    CHECK(pts.back().second >= 0.9, "%.2f of the blocks at %.0f dB", pts.back().second, pts.back().first);
    CHECK(wrongOk == 0, "%d wrong blocks shown with CRC OK", wrongOk);
    int falseRows = 0;
    auto falseKinds = [](const X40Telemetry& t) { return countKind(t, "Block") + countKind(t, "Header") + countKind(t, "File") + countKind(t, "Divider"); };
    for (unsigned seed : {7u, 8u}) { std::vector<cf32> n((size_t)(8000 * 60), cf32(0, 0)); addNoise(n, 1.0, seed); falseRows += falseKinds(run(x40::F06, n, 8000)); }
    { std::vector<std::pair<double, double>> seg = {{1000, 30.0}}; auto x = toneSeq(seg, 8000, path(0)); addSnr(x, 0, 8000, 9); falseRows += falseKinds(run(x40::F06, x, 8000)); }
    { auto x = f01Signal(randomGroups(3, 60), 1500, 8000, path(0)); addSnr(x, 15, 8000, 10); falseRows += falseKinds(run(x40::F06, x, 8000)); }
    printf("false decodes (2 x 60 s noise, a carrier, an FSK200/500 signal): %d rows\n", falseRows);
    CHECK(falseRows == 0, "%d rows from noise, a carrier or FSK200/500", falseRows);
    printf(fails ? "x40 f06: %d FAILED\n" : "x40 f06: ok\n", fails);
    return fails ? 1 : 0;
}
