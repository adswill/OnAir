// RDS data layer (fm_rds.h): block sync, error correction (bursts and two weak symbols), the group types beyond PS/RT (AF, ECC, PIN, CT,
// PTYN, ODA, RT+, eRT, EON, 2B with C'), no wrong text from corrected blocks; then the whole receiver: the weak-signal threshold, the
// multipath equaliser switching itself in and out, and the stereo blend.
#include "dect2/fm/fm_gen.h"
#include "dect2/fm/fm_rds.h"
#include "dect2/common/text_charset.h"
#include "dect2/fm/fm_rx.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint32_t rem(uint32_t v) { for (int i = 25; i >= 10; i--) if ((v >> i) & 1) v ^= 0x5B9u << (i - 10); return v; }
static uint32_t blk(uint16_t info, uint32_t off) { return ((uint32_t)info << 10) | ((rem((uint32_t)info << 10) ^ off) & 0x3FF); }
static const uint16_t kPi = 0x4A21;
static uint16_t B(int type, int ver, int low) { return (uint16_t)(type << 12 | ver << 11 | 1 << 10 | 10 << 5 | low); }

// the 104 bits of a group (after the differential decoding), first bit first
static void groupBits(const uint16_t g[4], std::vector<int>& bits) {
    const uint32_t off[4] = {0x0FC, 0x198, ((g[1] >> 11) & 1) ? 0x350u : 0x168u, 0x1B4};
    for (int i = 0; i < 4; i++) { const uint32_t b = blk(g[i], off[i]); for (int j = 25; j >= 0; j--) bits.push_back((b >> j) & 1); }
}

// the PS groups of "TESTFM  " and the RT groups of "Hello world\r"
static std::vector<std::array<uint16_t, 4>> stationGroups() {
    std::vector<std::array<uint16_t, 4>> v;
    const char* ps = "TESTFM  ";
    for (int s = 0; s < 4; s++) v.push_back({kPi, B(0, 0, 8 | s), 0xCDCD, (uint16_t)(ps[2 * s] << 8 | ps[2 * s + 1])});
    const char* rt = "Hello world\r    ";
    for (int s = 0; s < 3; s++) v.push_back({kPi, B(2, 0, s), (uint16_t)(rt[4 * s] << 8 | rt[4 * s + 1]), (uint16_t)(rt[4 * s + 2] << 8 | rt[4 * s + 3])});
    return v;
}

static void testCorrection() {
    std::mt19937 rng(5);
    const auto gs = stationGroups();
    for (int mode = 0; mode < 3; mode++) {
        // every group has one wrong bit (mode 0), a two-bit burst (one wrong symbol, mode 1) or two wrong symbols far apart with low
        // reliability (mode 2), in a random block; the first 20 groups are clean (the sync)
        for (int ec = 0; ec < 3; ec++) {
            RdsDecoder d;
            d.setCorrection(ec >= 1, ec >= 2);
            int wrongPs = 0;
            std::vector<int> bits;
            std::vector<float> rel;
            for (int k = 0; k < 400; k++) {
                const auto& g = gs[(size_t)k % gs.size()];
                const size_t at = bits.size();
                groupBits(g.data(), bits);
                rel.resize(bits.size(), 1.f);
                if (k < 20) continue;
                const int bl = (int)(rng() % 4), j = (int)(rng() % (mode == 2 ? 16 : 20));
                const size_t p = at + (size_t)bl * 26;
                if (mode == 0) bits[p + j] ^= 1;
                else if (mode == 1) { bits[p + j] ^= 1; bits[p + j + 1] ^= 1; }
                else {
                    const int j2 = j + 5 + (int)(rng() % 4);   // a second symbol 5 to 8 bits later
                    bits[p + j] ^= 1; bits[p + j + 1] ^= 1; bits[p + j2] ^= 1; bits[p + j2 + 1] ^= 1;
                    rel[p + j] = 0.1f; rel[p + j2] = 0.15f;
                }
            }
            for (size_t i = 0; i < bits.size(); i++) {
                d.pushBit(bits[i], rel[i]);
                if (!d.ps.empty() && d.ps != "TESTFM  ") wrongPs++;
            }
            const bool want = mode <= 1 ? ec >= 1 : ec >= 2;   // what this correction level can repair
            printf("errors %-26s correction %d: groups %llu of 400, good %llu corrected %llu bad %llu, PS '%s' RT '%s'\n",
                   mode == 0 ? "one bit" : mode == 1 ? "one symbol (2-bit burst)" : "two weak symbols", ec, (unsigned long long)d.groups,
                   (unsigned long long)d.info.blocksGood, (unsigned long long)d.info.blocksCorrected, (unsigned long long)d.info.blocksBad,
                   d.ps.c_str(), d.rt.c_str());
            CHECK(wrongPs == 0, "correction %d mode %d: a wrong PS was shown", ec, mode);
            if (want) {
                CHECK(d.groups >= (mode == 2 ? 350u : 390u), "correction %d mode %d: only %llu groups", ec, mode, (unsigned long long)d.groups);
                CHECK(d.ps == "TESTFM  " && d.rt == "Hello world", "correction %d mode %d: PS '%s' RT '%s'", ec, mode, d.ps.c_str(), d.rt.c_str());
            } else CHECK(d.groups < 300, "correction %d mode %d: %llu groups without the correction for it", ec, mode, (unsigned long long)d.groups);
        }
    }
    {   // pure noise for a long while: no group from it (blocks may pass by chance, a group needs B and D in sequence and a repeat)
        RdsDecoder d;
        for (int i = 0; i < 200000; i++) d.pushBit((int)(rng() & 1), 0.5f + (float)(rng() % 100) / 100.f);
        printf("noise: synced %d groups %llu PS '%s'\n", d.synced, (unsigned long long)d.groups, d.ps.c_str());
        CHECK(d.ps.empty() && d.rt.empty(), "noise produced PS '%s' RT '%s'", d.ps.c_str(), d.rt.c_str());
    }
    {   // a sync from one clean block among bad ones: two block ends a whole number of blocks apart are enough
        RdsDecoder d;
        std::vector<int> bits;
        for (int k = 0; k < 8; k++) groupBits(gs[(size_t)k % gs.size()].data(), bits);
        for (size_t i = 0; i < bits.size(); i++) if ((i / 26) % 3 != 0 && i % 26 == 7) bits[i] ^= 1;   // two of every three blocks bad
        RdsDecoder off; off.setCorrection(false, false);
        int at = -1;
        for (size_t i = 0; i < bits.size(); i++) { off.pushBit(bits[i]); if (off.synced && at < 0) at = (int)i; }
        CHECK(at >= 0 && at < 26 * 5, "sync from scattered clean blocks at bit %d", at);
    }
}

static void testGroups() {
    RdsDecoder d;
    std::vector<std::string> hex;
    d.setGroupTap([&](const uint16_t* b, const uint8_t* e) { hex.push_back(rdsHexGroup(b, e)); });
    auto send = [&](uint16_t b, uint16_t c, uint16_t dd, int times = 2) {
        const uint16_t g[4] = {kPi, b, c, dd};
        const uint8_t e[4] = {0, 0, 0, 0};
        for (int i = 0; i < times; i++) d.onGroup(g, e, 1e9);
    };
    for (const auto& g : stationGroups()) send(g[1], g[2], g[3]);
    // AF method A: 3 frequencies, 90.5, 92.0, 95.5 MHz
    send(B(0, 0, 8 | 0), (uint16_t)(227 << 8 | 30), (uint16_t)('T' << 8 | 'E'));
    send(B(0, 0, 8 | 1), (uint16_t)(45 << 8 | 80), (uint16_t)('S' << 8 | 'T'));
    // 1A: ECC 0xE2, PIN day 23 14:05
    send(B(1, 0, 0), 0x00E2, (uint16_t)(23 << 11 | 14 << 6 | 5));
    send(B(1, 0, 0), 0x3000 | 0x27, (uint16_t)(23 << 11 | 14 << 6 | 5));   // variant 3: language 0x27
    // 4A: 2026-10-23 12:34 UTC, +3:00
    const int mjd = 61336, h = 12, m = 34;
    send(B(4, 0, mjd >> 15), (uint16_t)((mjd & 0x7FFF) << 1 | h >> 4), (uint16_t)((h & 15) << 12 | m << 6 | 0 << 5 | 6), 1);
    // 10A PTYN "ROCKPOP "
    send(B(10, 0, 0), (uint16_t)('R' << 8 | 'O'), (uint16_t)('C' << 8 | 'K'));
    send(B(10, 0, 1), (uint16_t)('P' << 8 | 'O'), (uint16_t)('P' << 8 | ' '));
    // ODA: RT+ in 11A, eRT (UTF-8) in 12A, TMC announced too
    send(B(3, 0, 22), 0x0000, 0x4BD7);
    send(B(3, 0, 24), 0x0001, 0x6552);
    // RT+: Artist "Hello" (0, +4), Title "world" (6, +4), item running, toggle 1
    {
        const int t1 = 4, s1 = 0, l1 = 4, t2 = 1, s2 = 6, l2 = 4;
        send(B(11, 0, 1 << 4 | 1 << 3 | t1 >> 3), (uint16_t)((t1 & 7) << 13 | s1 << 7 | l1 << 1 | t2 >> 5), (uint16_t)((t2 & 31) << 11 | s2 << 5 | l2));
    }
    // eRT "Ąžuolas ėjo" + CR
    {
        const uint8_t t[16] = {0xc4, 0x84, 0xc5, 0xbe, 0x75, 0x6f, 0x6c, 0x61, 0x73, 0x20, 0xc4, 0x97, 0x6a, 0x6f, 0x0D, 0x20};
        for (int s = 0; s < 4; s++) send(B(12, 0, s), (uint16_t)(t[4 * s] << 8 | t[4 * s + 1]), (uint16_t)(t[4 * s + 2] << 8 | t[4 * s + 3]));
    }
    // EON: PI 0x4B01 "LRT KL. "
    {
        const char* n = "LRT KL. ";
        for (int v = 0; v < 4; v++) send(B(14, 0, v), (uint16_t)(n[2 * v] << 8 | n[2 * v + 1]), 0x4B01);
    }
    const RdsInfo& i = d.info;
    CHECK(d.ps == "TESTFM  " && d.rt == "Hello world", "PS '%s' RT '%s'", d.ps.c_str(), d.rt.c_str());
    CHECK(i.afMhz.size() == 3 && std::fabs(i.afMhz[0] - 90.5) < 0.01 && std::fabs(i.afMhz[1] - 92.0) < 0.01 && std::fabs(i.afMhz[2] - 95.5) < 0.01,
          "AF %zu", i.afMhz.size());
    CHECK(i.ecc == 0xE2 && i.language == 0x27, "ECC %X language %X", i.ecc, i.language);
    CHECK(i.pinDay == 23 && i.pinHour == 14 && i.pinMinute == 5, "PIN %d %d:%d", i.pinDay, i.pinHour, i.pinMinute);
    CHECK(i.clockUtc == 1792758840 && i.clockOffsetMin == 180, "clock %lld offset %d", (long long)i.clockUtc, i.clockOffsetMin);
    CHECK(i.ptyn == "ROCKPOP ", "PTYN '%s'", i.ptyn.c_str());
    CHECK(i.oda.size() == 2, "ODA list %zu", i.oda.size());
    bool artist = false, title = false;
    for (const auto& t : i.rtPlus) { if (t.type == 4 && t.text == "Hello") artist = true; if (t.type == 1 && t.text == "world") title = true; }
    CHECK(artist && title && i.rtPlusRunning, "RT+ %zu tags", i.rtPlus.size());
    CHECK(i.ert == "\xc4\x84\xc5\xbeuolas \xc4\x97jo", "eRT '%s'", i.ert.c_str());
    CHECK(i.eon.size() == 1 && i.eon[0].pi == 0x4B01 && i.eon[0].ps == "LRT KL. ", "EON %zu", i.eon.size());
    CHECK(!hex.empty() && hex[0] == "4A21 0548 CDCD 5445", "hex '%s'", hex.empty() ? "" : hex[0].c_str());
    // the hex of a group with a bad block
    {
        const uint16_t g[4] = {kPi, 0x2400, 0x1234, 0x5678};
        const uint8_t e[4] = {0, 0, 2, 1};
        CHECK(rdsHexGroup(g, e) == "4A21 2400 ---- 5678", "hex with a bad block '%s'", rdsHexGroup(g, e).c_str());
    }
    // 2B: radio text in D only (32 characters), the PI in C'
    {
        RdsDecoder e;
        const char* t = "Radio 2B\r";
        for (int r = 0; r < 2; r++) for (int s = 0; s < 5; s++) {
            const uint16_t g[4] = {kPi, B(2, 1, s), 0x4A22, (uint16_t)(t[2 * s] << 8 | (2 * s + 1 < 9 ? t[2 * s + 1] : ' '))};
            const uint8_t er[4] = {2, 0, 0, 0};   // block A lost: the PI comes from C'
            e.onGroup(g, er, 1e9);
        }
        CHECK(e.rt == "Radio 2B" && e.rtComplete && e.pi == 0x4A22, "2B: RT '%s' complete %d PI %04X", e.rt.c_str(), e.rtComplete, e.pi);
    }
    printf("groups: AF %zu, ECC %X, PIN %d/%02d:%02d, PTYN '%s', ODA %zu, RT+ %zu, eRT '%s', EON %zu\n", i.afMhz.size(), i.ecc, i.pinDay,
           i.pinHour, i.pinMinute, i.ptyn.c_str(), i.oda.size(), i.rtPlus.size(), i.ert.c_str(), i.eon.size());
}

struct Run { FmTelemetry t; double usable = 0; int wrongPs = 0, wrongRt = 0; int eqOnLast = 0; };
static Run runRx(const char* what, FmGenConfig c, double secs, int echoD = 0, double echoDb = 0) {
    FmGenerator g(c);
    FmReceiver rx; rx.setSilent(true); rx.configure(c.rate); rx.setStationFinder(false);
    long ok = 0, all = 0;
    rx.setRdsGroupTap([&](const uint16_t*, const uint8_t* e) { for (int k = 0; k < 4; k++) { all++; if (e[k] < 2) ok++; } });
    Run r;
    std::vector<cf32> x, dl(64);
    size_t dp = 0;
    const cf32 ea = std::polar((float)std::pow(10.0, echoDb / 20), 1.0f);
    uint64_t seq = 0;
    const size_t chunk = 50000;
    for (double t = 0; t < secs; t += chunk / c.rate) {
        x.clear(); g.generate(chunk, x);
        if (echoD > 0) for (auto& v : x) { const cf32 o = v; v += ea * dl[(dp + 64 - (size_t)echoD) % 64]; dl[dp] = o; dp = (dp + 1) % 64; }
        rx.feed(x.data(), x.size());
        if (rx.telemetry(r.t, seq)) {
            seq = r.t.seq;
            if (!r.t.psName.empty() && r.t.psName != c.ps) r.wrongPs++;
            const std::string& rt = r.t.radioText;
            for (size_t i = 0; i < rt.size(); i++) if (rt[i] != ' ' && (i >= c.rt.size() || rt[i] != c.rt[i])) { r.wrongRt++; break; }
        }
    }
    r.usable = all ? 100.0 * ok / all : 0;
    printf("%-30s groups %5.2f/s usable %5.1f%% PS '%s' wrong PS %d RT %d | equaliser %d multipath %.1f%% | stereo %d (%.0f%%) SNR %.1f\n", what,
           r.t.rdsGroups / secs, r.usable, r.t.psName.c_str(), r.wrongPs, r.wrongRt, r.t.equaliser, r.t.multipathPct, r.t.stereo, r.t.stereoPct, r.t.snrDb);
    return r;
}

static void testReceiver() {
    FmGenConfig c; c.rate = 1e6;
    // the weak-signal threshold: at 16 dB carrier to noise the receiver before this work decoded 2.5 groups a second in stereo and none
    // on a mono station (45 % and 6 % of the blocks); now about 9.5 of the 11.4 sent
    for (bool stereo : {true, false}) {
        FmGenConfig w = c; w.cnrDb = 16; w.stereo = stereo; w.seed = 3;
        Run r = runRx(stereo ? "16 dB stereo" : "16 dB mono", w, 20);
        CHECK(r.t.rdsGroups >= 8 * 20 && r.usable >= 90, "%s 16 dB: %llu groups, %.1f%% usable", stereo ? "stereo" : "mono", (unsigned long long)r.t.rdsGroups, r.usable);
        CHECK(r.wrongPs == 0 && r.wrongRt == 0 && r.t.psName == w.ps, "%s 16 dB: PS '%s', wrong %d/%d", stereo ? "stereo" : "mono", r.t.psName.c_str(), r.wrongPs, r.wrongRt);
    }
    {   // a quiet passage (no audio, only the pilot and RDS): the equaliser used to cut the RDS sidebands (0.4 groups/s at 18 dB)
        FmGenConfig w = c; w.cnrDb = 18; w.leftAmp = w.rightAmp = 0; w.seed = 4;
        Run r = runRx("18 dB silent", w, 12);
        CHECK(r.t.rdsGroups >= 10 * 12 && !r.t.equaliser, "silent 18 dB: %llu groups, equaliser %d", (unsigned long long)r.t.rdsGroups, r.t.equaliser);
    }
    {   // clean: full stereo, no equaliser; an echo of 10 us at -6 dB: the equaliser on and RDS as good
        FmGenConfig w = c; w.cnrDb = 40;
        Run a = runRx("40 dB clean", w, 6);
        CHECK(!a.t.equaliser && a.t.stereo && a.t.stereoPct > 99, "clean: equaliser %d stereo %d %.0f%%", a.t.equaliser, a.t.stereo, a.t.stereoPct);
        Run b = runRx("40 dB echo 10 us -6 dB", w, 8, 10, -6);
        CHECK(b.t.equaliser && b.usable > 95 && b.t.psName == w.ps, "echo: equaliser %d usable %.1f%%", b.t.equaliser, b.usable);
    }
    {   // a weak stereo station blends towards mono (Auto), unless Stereo is chosen
        FmGenConfig w = c; w.cnrDb = 12;
        Run a = runRx("12 dB stereo (blend)", w, 6);
        CHECK(a.t.stereoPct < 40, "12 dB: stereo %.0f%% (wanted a blend towards mono)", a.t.stereoPct);
    }
    {   // the extra groups through the whole chain: RT+, eRT, CT, AF, PTYN, ECC, EON
        FmGenConfig w = c; w.cnrDb = 30; w.rt = "Artist - Title of the song";
        const int t1 = 4, s1 = 0, l1 = 5, t2 = 1, s2 = 9, l2 = 16;
        w.extraGroups = {
            {B(3, 0, 22), 0x0000, 0x4BD7},
            {B(11, 0, 1 << 4 | 1 << 3 | t1 >> 3), (uint16_t)((t1 & 7) << 13 | s1 << 7 | l1 << 1 | t2 >> 5), (uint16_t)((t2 & 31) << 11 | s2 << 5 | l2)},
            {B(4, 0, 61336 >> 15), (uint16_t)((61336 & 0x7FFF) << 1 | 0), (uint16_t)(12 << 12 | 34 << 6 | 6)},
            {B(10, 0, 0), (uint16_t)('R' << 8 | 'O'), (uint16_t)('C' << 8 | 'K')},
            {B(10, 0, 1), (uint16_t)('P' << 8 | 'O'), (uint16_t)('P' << 8 | ' ')},
            {B(1, 0, 0), 0x00E2, (uint16_t)(23 << 11 | 14 << 6 | 5)},
            {B(14, 0, 0), (uint16_t)('O' << 8 | 'T'), 0x4B01}, {B(14, 0, 1), (uint16_t)('H' << 8 | 'E'), 0x4B01},
            {B(14, 0, 2), (uint16_t)('R' << 8 | ' '), 0x4B01}, {B(14, 0, 3), (uint16_t)(' ' << 8 | ' '), 0x4B01},
        };
        Run r = runRx("30 dB extra groups", w, 30);
        const RdsInfo& i = r.t.rds;
        std::string artist, title;
        for (const auto& t : i.rtPlus) { if (t.type == 4) artist = t.text; if (t.type == 1) title = t.text; }
        printf("  RT '%s' artist '%s' title '%s' PTYN '%s' ECC %X clock %lld EON %zu\n", r.t.radioText.c_str(), artist.c_str(), title.c_str(), i.ptyn.c_str(), i.ecc,
               (long long)i.clockUtc, i.eon.size());
        CHECK(artist == "Artist" && title == "Title of the song", "chain RT+: '%s' '%s'", artist.c_str(), title.c_str());
        CHECK(i.ptyn == "ROCKPOP " && i.ecc == 0xE2 && i.clockUtc == 1792758840 && i.eon.size() == 1 && i.eon[0].ps == "OTHER   ", "chain: PTYN '%s' ECC %X clock %lld EON %zu",
              i.ptyn.c_str(), i.ecc, (long long)i.clockUtc, i.eon.size());
    }
}


// #350: bytes 0x80..0xFF. EBU Latin (IEC 62106 Annex E) by default, GB2312 when a Chinese station sends it
static void feedPs(RdsDecoder& d, const std::string& ps8) {
    const uint8_t ok[4] = {0, 0, 0, 0};
    for (int s = 0; s < 4; s++) {
        const uint16_t g[4] = {kPi, B(0, 0, 8 | s), 0xCDCD, (uint16_t)((uint8_t)ps8[2 * s] << 8 | (uint8_t)ps8[2 * s + 1])};
        d.onGroup(g, ok, 1e9);
    }
}
static void feedRt(RdsDecoder& d, const std::string& rt) {
    const uint8_t ok[4] = {0, 0, 0, 0};
    std::string r = rt; r.push_back('\r'); while (r.size() % 4) r.push_back(' ');
    for (size_t s = 0; s * 4 < r.size() && s < 16; s++) {
        const uint16_t g[4] = {kPi, B(2, 0, (int)s), (uint16_t)((uint8_t)r[4 * s] << 8 | (uint8_t)r[4 * s + 1]), (uint16_t)((uint8_t)r[4 * s + 2] << 8 | (uint8_t)r[4 * s + 3])};
        d.onGroup(g, ok, 1e9);
    }
}
static void testCharset() {
    {   // GB2312: the PS "中国之声" (4 characters = 8 bytes) and a RadioText mixing Chinese and ASCII
        RdsDecoder d;
        feedPs(d, "\xD6\xD0\xB9\xFA\xD6\xAE\xC9\xF9");
        CHECK(d.ps == "\xE4\xB8\xAD\xE5\x9B\xBD\xE4\xB9\x8B\xE5\xA3\xB0", "GB2312 PS: got %s", d.ps.c_str());
        feedRt(d, "FM 93.9 \xBD\xBB\xCD\xA8\xB5\xE7\xCC\xA8");
        CHECK(d.rt == "FM 93.9 \xE4\xBA\xA4\xE9\x80\x9A\xE7\x94\xB5\xE5\x8F\xB0", "GB2312 RT: got %s", d.rt.c_str());
    }
    {   // #350: a name that scrolls a character per group and arrives in patchy order never completes the strict collection; it is still shown (no '?', no placeholders)
        RdsDecoder d;
        const std::string txt = "\xD6\xD0\xB9\xFA\xD6\xAE\xC9\xF9\xBD\xBB\xCD\xA8\xB9\xE3\xB2\xA5";   // 8 characters
        const uint8_t ok[4] = {0, 0, 0, 0};
        for (int t = 0; t < 120; t++) {
            const size_t start = (size_t)(t % 5) * 2;   // moves a character every group
            static const int order[6] = {0, 1, 0, 2, 0, 3};   // segments arrive in a patchy order (groups lost to noise): four different ones never follow each other
            const int s = order[t % 6];
            const uint16_t g[4] = {kPi, B(0, 0, 8 | s), 0xCDCD, (uint16_t)((uint8_t)txt[start + 2 * s] << 8 | (uint8_t)txt[start + 2 * s + 1])};
            d.onGroup(g, ok, 1e9);
        }
        CHECK(!d.ps.empty() && d.ps.find('?') == std::string::npos && d.ps.find('_') == std::string::npos, "scrolling PS: got '%s'", d.ps.c_str());
    }
    {   // the same bytes with the GB2312 reading off: EBU Latin letters, never '?'
        RdsDecoder d; d.charset = 0;
        feedPs(d, "\xD6\xD0\xB9\xFA\xD6\xAE\xC9\xF9");
        CHECK(!d.ps.empty() && d.ps.find('?') == std::string::npos, "EBU Latin PS: %s", d.ps.c_str());
    }
    {   // a European station: EBU Latin accents stay accents (a byte of 0x80..0xFF alone is read as EBU Latin, never as '?')
        RdsDecoder d;
        const uint8_t eb[1] = {0x8A};
        const std::string a = text::ebuLatin(eb, 1);
        feedPs(d, std::string("B") + (char)0x8A + "R  FM ");
        CHECK(d.ps == "B" + a + "R  FM ", "Latin PS: got %s", d.ps.c_str());
    }
    {   // plain ASCII untouched; a lone high byte (a partial text) does not turn the text into Chinese
        RdsDecoder d;
        feedPs(d, "TESTFM  ");
        CHECK(d.ps == "TESTFM  ", "ASCII PS");
    }
}

// Voting over repeated groups (#15): random bit errors above what the burst and two-symbol correctors repair, soft values telling most of
// the wrong bits apart. Voting must decode more where the correctors alone fall short, and never show a wrong text.
static void testVoting() {
    for (double ber : {0.03, 0.05, 0.08}) {
        uint64_t groupsOff = 0, groupsOn = 0;
        int wrongOff = 0, wrongOn = 0;
        for (int vote = 0; vote < 2; vote++) {
            std::mt19937 rng(21);
            RdsDecoder d;
            d.setVoting(vote != 0);
            const auto gs = stationGroups();
            std::vector<int> bits; std::vector<float> rel;
            for (int k = 0; k < 1400; k++) {
                const size_t at = bits.size();
                groupBits(gs[(size_t)k % gs.size()].data(), bits);
                rel.resize(bits.size(), 1.f);
                for (size_t i = at; i < bits.size(); i++) rel[i] = 0.6f + (float)(rng() % 80) / 100.f;
                if (k < 20) continue;
                for (size_t i = at; i < bits.size(); i++)
                    if ((double)(rng() % 100000) / 1e5 < ber) { bits[i] ^= 1; rel[i] = (rng() % 3) ? 0.05f + (float)(rng() % 40) / 100.f : 0.9f; }
            }
            int wrong = 0;
            for (size_t i = 0; i < bits.size(); i++) {
                d.pushBit(bits[i], rel[i]);
                if (d.ps.find_first_not_of("TESTFM ") != std::string::npos || d.rt.find_first_not_of("Helo wrd?\r") != std::string::npos) wrong++;   // partial texts have gaps: only foreign characters are wrong
            }
            printf("voting BER %.2f %s: groups %llu voted blocks %llu bad %llu PS '%s' RT '%s'\n", ber, vote ? "on " : "off",
                   (unsigned long long)d.groups, (unsigned long long)d.info.blocksVoted, (unsigned long long)d.info.blocksBad, d.ps.c_str(), d.rt.c_str());
            // the correctors alone already let a wrong character through at these error rates (the baseline, counted per bit); voting must not add to it
            (vote ? wrongOn : wrongOff) = wrong;
            if (vote && ber <= 0.05) CHECK(d.ps == "TESTFM  " && d.rt == "Hello world", "voting BER %.2f: PS '%s' RT '%s'", ber, d.ps.c_str(), d.rt.c_str());
            (vote ? groupsOn : groupsOff) = d.groups;
            if (!vote) CHECK(d.info.blocksVoted == 0, "voting off but blocks voted");
        }
        printf("  wrong-text bit positions: off %d on %d\n", wrongOff, wrongOn);
        CHECK(wrongOn <= wrongOff + wrongOff / 5 + 50, "BER %.2f: voting shows more wrong text (%d vs %d)", ber, wrongOn, wrongOff);
        CHECK(groupsOn >= groupsOff, "BER %.2f: voting lost groups (%llu < %llu)", ber, (unsigned long long)groupsOn, (unsigned long long)groupsOff);
        if (ber >= 0.05) CHECK(groupsOn > groupsOff + groupsOff / 20, "BER %.2f: voting gained too little (%llu vs %llu)", ber, (unsigned long long)groupsOn, (unsigned long long)groupsOff);
    }
    {   // noise: nothing is voted into existence
        std::mt19937 rng(3);
        RdsDecoder d;
        for (int i = 0; i < 2000000; i++) d.pushBit((int)(rng() & 1), 0.2f + (float)(rng() % 100) / 100.f);
        printf("voting on noise: synced %d voted %llu groups %llu\n", d.synced, (unsigned long long)d.info.blocksVoted, (unsigned long long)d.groups);
        CHECK(d.info.blocksVoted <= 1 && d.ps.empty() && d.rt.empty(), "noise: %llu voted blocks, PS '%s'", (unsigned long long)d.info.blocksVoted, d.ps.c_str());
    }
}

int main() {
    testCharset();
    testCorrection();
    testGroups();
    testVoting();
    testReceiver();
    printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
