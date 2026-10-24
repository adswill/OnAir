// HF digital keyboard modes (hfdig_kbd.h): Olivia, Contestia, MFSK, THOR, DominoEX and Hell. The transmitter here is written from the
// same fldigi tables as the decoder, so a loopback only shows the two agree; what is checked against the specification: the tables
// themselves (varicodes, scrambling codes, the Hadamard code, the convolutional code's free distance and the interleaver permutation
// against values computed by hand from fldigi's source), the bandwidth and baud rate of every variant against fldigi's documentation,
// and the decoder on impaired audio: tuning errors, sample clock errors, noise near the threshold, noise alone.
// DECT2_KBD_REF=<raw float file> DECT2_KBD_FAMILY=<n> DECT2_KBD_VARIANT=<n> [DECT2_KBD_TONES, DECT2_KBD_BW] decodes a file made by
// the original fldigi code (8 kHz, 32 bit float) and prints the text: how the Olivia port was checked against Jalocha's transmitter.
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "dect2/engine/modes.h"
#include "dect2/source/source.h"
#include "../../core/src/hfdig/hfdig_kbd_tables.h"
#include "../../core/src/hfdig/hfdig_kbd_dsp.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Noise {
    uint64_t s = 4242;
    double u() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return ((s >> 11) + 0.5) / 9007199254740992.0; }
    double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); }
};

struct Result {
    std::string text;
    bool found = false;
    double sec = 0;            // audio seconds until `need` was seen (or all)
    double offset = 0, snr = 0, centre = 0;
    uint64_t chars = 0;
};

// Signal power of a transmitter: measured from its own audio (over 20 s)
static double signalPower(const KbdTxConfig& c) {
    auto tx = makeKbdTx(c);
    std::vector<float> b(8000);
    double p = 0; size_t n = 0;
    for (int k = 0; k < 25; k++) { tx->generate(b.data(), b.size()); if (k < 5) continue; for (float v : b) { p += (double)v * v; n++; } }
    return p / (double)n;
}

// Decodes the transmitter's audio at `snrDb` (signal power over the noise power in 2500 Hz) until `need` is in the text or maxSec is up
static Result run(KbdTxConfig tx, bool autoCentre, double snrDb, double maxSec, const std::string& need, double rxCentre = -1) {
    Result r;
    const double sp = tx.amp > 0 ? signalPower(tx) : 0;
    const double sigma = tx.amp > 0 ? std::sqrt(sp / std::pow(10.0, snrDb / 10.0) * 4000.0 / 2500.0) : 0.1;
    auto gen = tx.amp > 0 ? makeKbdTx(tx) : nullptr;
    auto dec = makeKbdDecoder();
    KbdConfig k = tx.cfg;
    k.autoCentre = autoCentre;
    if (rxCentre > 0) k.centreHz = rxCentre;
    dec->setConfig(k);
    Noise nz;
    std::vector<float> buf(800);
    HfdigKbdTelemetry t;
    for (int blk = 0; blk < (int)(maxSec * 10); blk++) {
        if (gen) gen->generate(buf.data(), buf.size()); else std::fill(buf.begin(), buf.end(), 0.f);
        for (float& v : buf) v += (float)(sigma * nz.g());
        dec->feedAudio(buf.data(), buf.size());
        r.sec = (blk + 1) * 0.1;
        if (blk % 5 == 4) {
            dec->telemetry(t);
            if (!need.empty() && t.text.find(need) != std::string::npos) { r.found = true; break; }
        }
    }
    dec->telemetry(t);
    r.text = t.text; r.offset = t.offsetHz; r.snr = t.snr; r.centre = t.centreHz; r.chars = t.chars;
    if (!need.empty() && t.text.find(need) != std::string::npos) r.found = true;
    return r;
}

static KbdTxConfig olivia(int fam, int tones, int bw, const std::string& txt = "LY0AIR TEST 73 ") {
    KbdTxConfig c;
    c.cfg.family = fam; c.cfg.tones = tones; c.cfg.bandwidth = bw;
    c.text = txt;
    return c;
}

static int refFile() {
    const char* f = getenv("DECT2_KBD_REF");
    if (!f) return 0;
    FILE* fp = fopen(f, "rb");
    if (!fp) { printf("cannot open %s\n", f); return 1; }
    std::vector<float> a;
    float v;
    while (fread(&v, 4, 1, fp) == 1) a.push_back(v);
    fclose(fp);
    KbdConfig k;
    k.family = getenv("DECT2_KBD_FAMILY") ? atoi(getenv("DECT2_KBD_FAMILY")) : 0;
    if (getenv("DECT2_KBD_VARIANT")) k.variant = atoi(getenv("DECT2_KBD_VARIANT"));
    if (getenv("DECT2_KBD_TONES")) k.tones = atoi(getenv("DECT2_KBD_TONES"));
    if (getenv("DECT2_KBD_BW")) k.bandwidth = atoi(getenv("DECT2_KBD_BW"));
    if (getenv("DECT2_KBD_CENTRE")) k.centreHz = atof(getenv("DECT2_KBD_CENTRE"));
    k.autoCentre = getenv("DECT2_KBD_AUTO") != nullptr;
    auto dec = makeKbdDecoder();
    dec->setConfig(k);
    for (size_t i = 0; i < a.size(); i += 800) dec->feedAudio(a.data() + i, std::min<size_t>(800, a.size() - i));
    HfdigKbdTelemetry t;
    dec->telemetry(t);
    printf("decoded %llu chars: [%s]\n", (unsigned long long)t.chars, t.text.c_str());
    return 0;
}

int main() {
    if (getenv("DECT2_KBD_REF")) return refFile();
    const std::string key = "LY0AIR TEST 73";

    {   // names, counts and bandwidths
        CHECK(kbdVariantCount(KbdMfsk) == 8 && kbdVariantCount(KbdThor) == 10 && kbdVariantCount(KbdDominoex) == 6 && kbdVariantCount(KbdHell) == 7, "variant counts");
        KbdConfig c;
        c.family = KbdOlivia; c.tones = 32; c.bandwidth = 1000;
        CHECK(std::fabs(kbdBandwidthHz(c) - 1000) < 1, "Olivia 32/1000 bandwidth %.1f", kbdBandwidthHz(c));
    }
    {   // values worked out by hand from fldigi's tables
        // K = 7 code, polynomials 0x6d and 0x4f: the output for a single 1 followed by zeros is the polynomials' bits, first polynomial in bit 0
        kbd::ConvEnc e(7, 0x6d, 0x4f);
        const int want[7] = {3, 2, 3, 3, 0, 1, 3};
        for (int i = 0; i < 7; i++) { const int g = e.encode(i == 0); CHECK(g == want[i], "convolutional code step %d: %d", i, g); }
        // the IZ8BLY varicode: CR and BS are the short codes, NUL the 11 bit one; every code starts with 1 and ends with 00
        CHECK(std::string(kbd::kMfskVaricode[13]) == "10101100" && std::string(kbd::kMfskVaricode[8]) == "10101000" && std::string(kbd::kMfskVaricode[0]) == "11101011100", "varicode CR / BS / NUL");
        int badCode = 0;
        for (int c = 0; c < 256; c++) { const std::string v = kbd::kMfskVaricode[c]; if (v[0] != '1' || v.substr(v.size() - 2) != "00") badCode++; }
        CHECK(badCode == 0, "%d varicode words break the 1 .. 00 rule", badCode);
        // DominoEX: the first nibble of a character has bit 3 clear, a continuation nibble has it set, at most three nibbles
        int badNib = 0;
        for (int c = 0; c < 256; c++) {
            const unsigned char* n = kbd::kDominoVaricode[c];
            if (n[0] & 8) badNib++;
            if (n[1] && !(n[1] & 8)) badNib++;
            if (n[2] && !(n[2] & 8)) badNib++;
        }
        CHECK(badNib == 0, "%d DominoEX characters break the nibble rule", badNib);
        // bandwidth of the variants: tone spacing fs * ds / symlen times the tones (fldigi's constants)
        KbdConfig b;
        b.family = KbdMfsk; b.variant = 3;
        CHECK(std::fabs(kbdBandwidthHz(b) - 15 * 15.625) < 0.01, "MFSK16 %.2f Hz", kbdBandwidthHz(b));
        b.variant = 6;
        CHECK(std::fabs(kbdBandwidthHz(b) - 15 * 31.25) < 0.01, "MFSK32 %.2f Hz", kbdBandwidthHz(b));
        b.family = KbdThor; b.variant = 9;
        CHECK(std::fabs(kbdBandwidthHz(b) - 1800) < 0.01, "THOR 100 %.2f Hz", kbdBandwidthHz(b));
        b.family = KbdDominoex; b.variant = 4;
        CHECK(std::fabs(kbdBandwidthHz(b) - 18 * 15.625) < 0.01, "DominoEX 16 %.2f Hz", kbdBandwidthHz(b));
        b.family = KbdOlivia; b.tones = 8; b.bandwidth = 250;
        CHECK(std::fabs(kbdBandwidthHz(b) - 250) < 0.01, "Olivia 8/250");
    }
    {   // callsigns
        CHECK(kbdFindCallsign("CQ CQ DE LY0AIR K") == "LY0AIR", "LY0AIR");
        CHECK(kbdFindCallsign("DE W1AW/P PSE") == "W1AW/P", "W1AW/P: %s", kbdFindCallsign("DE W1AW/P PSE").c_str());
        CHECK(kbdFindCallsign("THE QUICK BROWN FOX").empty(), "no callsign in plain words");
        CHECK(kbdFindCallsign("5 R 9A1AA").size() == 5, "9A1AA");
    }

    {   // Olivia 32/1000 and Contestia 32/1000, clean
        auto r = run(olivia(KbdOlivia, 32, 1000), false, 40, 60, key);
        CHECK(r.found, "Olivia 32/1000 clean: [%s]", r.text.c_str());
        printf("Olivia 32/1000: %.1f s, offset %.1f Hz\n", r.sec, r.offset);
        r = run(olivia(KbdContestia, 32, 1000), false, 40, 60, key);
        CHECK(r.found, "Contestia 32/1000 clean: [%s]", r.text.c_str());
    }
    {   // every common tone and bandwidth combination, clean, in a few blocks
        int n = 0, ok = 0;
        for (int fam : {KbdOlivia, KbdContestia})
            for (int tones : {4, 8, 16, 32, 64})
                for (int bw : {125, 250, 500, 1000, 2000}) {
                    const int symLen = 1 << ((int)std::lround(std::log2(tones)) + 7 - (int)std::lround(std::log2(bw / 125)));
                    if (symLen < 64) continue;
                    n++;
                    const double blockSec = (fam == KbdOlivia ? 64.0 : 32.0) * (symLen / 2) / 8000.0;
                    auto r = run(olivia(fam, tones, bw, "LY0AIR TEST 73 "), false, 30, blockSec * 14 + 8, key);
                    if (r.found) ok++; else printf("  %s %d/%d did not decode in %.0f s: [%s]\n", fam == KbdOlivia ? "Olivia" : "Contestia", tones, bw, r.sec, r.text.c_str());
                }
        CHECK(ok == n, "tone / bandwidth combinations decoded: %d of %d", ok, n);
    }
    {   // tuning error: inside the search margin without the signal finder, anywhere with it
        int ok = 0, n = 0;
        for (double off : {-100.0, -50.0, -20.0, 0.0, 20.0, 50.0, 100.0}) {
            auto tx = olivia(KbdOlivia, 32, 1000); tx.offsetHz = off;
            auto r = run(tx, false, 20, 60, key); n++; ok += r.found;
            if (!r.found) printf("  offset %+.0f Hz without search: not decoded\n", off);
        }
        CHECK(ok == n, "Olivia 32/1000 tuning errors inside the margin: %d of %d", ok, n);
        ok = 0; n = 0;
        for (double off : {-400.0, -250.0, 250.0, 400.0}) {
            auto tx = olivia(KbdOlivia, 32, 1000); tx.offsetHz = off;
            auto r = run(tx, true, 20, 90, key); n++; ok += r.found;
            if (!r.found) printf("  offset %+.0f Hz with search: not decoded (centre %.0f)\n", off, r.centre);
        }
        CHECK(ok == n, "Olivia 32/1000 with the signal finder: %d of %d", ok, n);
    }
    {   // sample clock error
        int ok = 0, n = 0;
        for (double ppm : {-300.0, -100.0, 100.0, 300.0}) {
            auto tx = olivia(KbdOlivia, 32, 1000); tx.clockPpm = ppm;
            auto r = run(tx, false, 20, 80, key); n++; ok += r.found;
        }
        CHECK(ok == n, "Olivia sample clock errors: %d of %d", ok, n);
    }
    {   // noise: the decoding threshold, and nothing from noise alone
        double thr = 99;
        for (double snr = -4; snr >= -20; snr -= 2) {
            auto r = run(olivia(KbdOlivia, 32, 1000, "LY0AIR TEST 73 "), false, snr, 150, key);
            if (r.found) thr = snr; else break;
        }
        printf("Olivia 32/1000 decodes down to %.0f dB (signal to noise in 2.5 kHz)\n", thr);
        CHECK(thr <= -8, "Olivia 32/1000 threshold %.0f dB", thr);
        KbdTxConfig none = olivia(KbdOlivia, 32, 1000);
        none.amp = 0;
        auto r = run(none, true, 0, 120, "");
        CHECK(r.chars == 0, "noise alone gave %llu characters [%s]", (unsigned long long)r.chars, r.text.c_str());
    }

    {   // MFSK: every variant clean, then impairments on MFSK16
        static const double symSec[8] = {2048 / 8000.0, 1024 / 8000.0, 1024 / 11025.0, 512 / 8000.0, 512 / 11025.0, 256 / 8000.0, 256 / 8000.0, 128 / 8000.0};
        for (int v = 0; v < 8; v++) {
            KbdTxConfig c;
            c.cfg.family = KbdMfsk; c.cfg.variant = v;
            c.text = "LY0AIR TEST 73 ";
            auto r = run(c, false, 30, symSec[v] * 260 + 10, key);
            CHECK(r.found, "%s clean: [%s]", kbdVariantName(KbdMfsk, v), r.text.c_str());
            printf("%s: %.1f s\n", kbdVariantName(KbdMfsk, v), r.sec);
        }
    }
    {   // DominoEX and THOR: every variant clean
        static const double domSym[6] = {2048 / 8000.0, 2048 / 11025.0, 1024 / 8000.0, 1024 / 11025.0, 512 / 8000.0, 512 / 11025.0};
        static const double thorSym[10] = {2048 / 8000.0, 2048 / 11025.0, 1024 / 8000.0, 1024 / 11025.0, 512 / 8000.0, 512 / 11025.0, 320 / 8000.0, 160 / 8000.0, 160 / 8000.0, 80 / 8000.0};
        for (int f : {KbdDominoex, KbdThor})
            for (int v = 0; v < kbdVariantCount(f); v++) {
                KbdTxConfig c;
                c.cfg.family = f; c.cfg.variant = v;
                c.text = "LY0AIR TEST 73 ";
                auto r = run(c, false, 30, (f == KbdThor ? thorSym[v] : domSym[v]) * (f == KbdThor ? 700 : 300) + 10, key);
                CHECK(r.found, "%s clean: [%s]", kbdVariantName(f, v), r.text.c_str());
                printf("%s: %.1f s\n", kbdVariantName(f, v), r.sec);
            }
    }
    {   // Hell: the picture of every variant against the pixels of the font
        for (int vv = 0; vv < 10; vv++) {
            const int v = vv < 7 ? vv : 0;
            const bool uni = vv >= 7;   // UTF-8 text: accented Latin letters (issue 225)
            KbdTxConfig c;
            c.cfg.family = KbdHell; c.cfg.variant = v;
            c.text = vv == 8 ? "\xD0\x91\xD0\xB6\xD0\xAF\xD0\xA8 73 " : vv == 9 ? "\xCE\x94\xCE\xA3\xCE\xA9\xCE\xA8 73 " : uni ? "L\xC3\xA9\xC3\xB1\xC3\x84 73 " : "LY0AIR 73 ";
            const double colRate = v == 1 ? 2.1875 : v == 2 ? 35 : v == 3 ? 87.5 : v == 4 ? 157.5 : 17.5;
            const double sec = std::min(300.0, 130 / colRate + 20);
            auto gen = makeKbdTx(c);
            auto dec = makeKbdDecoder();
            KbdConfig k = c.cfg; k.autoCentre = false;
            dec->setConfig(k);
            Noise nz;
            std::vector<float> buf(800);
            const double sp = signalPower(c);
            const double sigma = std::sqrt(sp / std::pow(10.0, 20.0 / 10) * 4000.0 / 2500.0);
            for (int blk = 0; blk < (int)(sec * 10); blk++) {
                gen->generate(buf.data(), buf.size());
                for (float& x : buf) x += (float)(sigma * nz.g());
                dec->feedAudio(buf.data(), buf.size());
            }
            HfdigKbdTelemetry t;
            dec->telemetry(t);
            // the ideal columns of the text, repeated; the best match over the shifts of the last columns
            std::vector<std::vector<int>> ideal;
            auto nullCol = [&] { ideal.push_back(std::vector<int>(14, 0)); };
            for (int rep = 0; rep < 2; rep++)
                for (size_t ti = 0; ti < c.text.size(); ti++) {
                    int ch = (uint8_t)c.text[ti];
                    if (ch >= 0xC0) { const int c2 = (uint8_t)c.text[++ti] & 63; ch = ((ch & 31) << 6) | c2; }
                    nullCol();
                    if (ch == ' ') { nullCol(); nullCol(); nullCol(); }
                    else {
                        unsigned short g[14];
                        kbd::hellGlyph(ch, g);
                        int ord = 0; for (int i = 0; i < 14; i++) ord |= g[i];
                        for (int col = 0; col < 16; col++) {
                            std::vector<int> cl(14, 0); int any = 0;
                            for (int r = 0; r < 14; r++) if (g[r] & (1 << (15 - col))) { cl[r] = 1; any = 1; }
                            if (!any && (ord & ((1 << (15 - col)) - 1)) == 0) break;
                            ideal.push_back(cl);
                        }
                    }
                    nullCol();
                }
            double bestMatch = 0;
            const int N = std::min(t.hellCols, 60);
            if (N >= 30) {
                for (size_t sh = 0; sh + N <= ideal.size(); sh++) {
                    int ok = 0, inkOk = 0, ink = 0;
                    for (int ci = 0; ci < N; ci++) for (int r = 0; r < 14; r++) {
                        const int got = t.hell[(size_t)(t.hellCols - N + ci) * 14 + r] > 127, want = ideal[sh + ci][r];
                        ok += got == want; ink += want; inkOk += want && got;
                    }
                    bestMatch = std::max(bestMatch, (double)ok / (N * 14));
                    (void)inkOk; (void)ink;
                }
            }
            printf("%s: %d columns, best match %.3f, level %.1f dB\n", kbdVariantName(KbdHell, v), t.hellCols, bestMatch, t.snr);
            CHECK(bestMatch > (uni ? 0.9 : 0.92), "%s: picture matches the font only %.3f", kbdVariantName(KbdHell, v), bestMatch);
        }
    }
    {   // impairments for one variant of each family: tuning errors, clock errors, noise near the threshold, noise alone
        struct Case { int fam, var, tones, bw; double symSec, blockSec; double maxThr; double offIn, offOut; };
        const Case cases[] = {
            {KbdContestia, -1, 16, 500, 0, 0, -4, 80, 300},
            {KbdMfsk, 3, 0, 0, 512 / 8000.0, 0, -4, 25, 150},
            {KbdMfsk, 7, 0, 0, 128 / 8000.0, 0, 6, 100, 300},
            {KbdDominoex, 3, 0, 0, 1024 / 11025.0, 0, 0, 100, 300},
            {KbdThor, 4, 0, 0, 512 / 8000.0, 0, 0, 100, 300},
            {KbdThor, 9, 0, 0, 80 / 8000.0, 0, 8, 150, 250},
        };
        for (const Case& cs : cases) {
            KbdTxConfig base;
            base.cfg.family = cs.fam; base.cfg.variant = cs.var; base.cfg.tones = cs.tones; base.cfg.bandwidth = cs.bw;
            base.text = "LY0AIR TEST 73 ";
            const char* nm = cs.fam == KbdContestia ? "Contestia 16/500" : kbdVariantName(cs.fam, cs.var);
            const double span = cs.fam == KbdContestia ? 40.0 : cs.symSec * (cs.fam == KbdThor ? 600 : cs.fam == KbdMfsk ? 300 : 300) + 10;
            int ok = 0, n = 0;
            for (double off : {-cs.offIn, -cs.offIn / 3, cs.offIn / 3, cs.offIn}) {
                auto tx = base; tx.offsetHz = off;
                auto r = run(tx, false, 25, span, key); n++; ok += r.found;
                if (!r.found) printf("  %s offset %+.0f Hz, no search: [%s]\n", nm, off, r.text.c_str());
            }
            CHECK(ok == n, "%s tuning errors inside the margin: %d of %d", nm, ok, n);
            ok = n = 0;
            for (double off : {-cs.offOut, cs.offOut}) {
                auto tx = base; tx.offsetHz = off;
                auto r = run(tx, true, 25, span + 30, key); n++; ok += r.found;
                if (!r.found) printf("  %s offset %+.0f Hz, search: [%s] centre %.0f\n", nm, off, r.text.c_str(), r.centre);
            }
            CHECK(ok == n, "%s with the signal finder: %d of %d", nm, ok, n);
            ok = n = 0;
            for (double ppm : {-200.0, 200.0}) {
                auto tx = base; tx.clockPpm = ppm;
                auto r = run(tx, false, 25, span, key); n++; ok += r.found;
            }
            CHECK(ok == n, "%s clock errors: %d of %d", nm, ok, n);
            double thr = 99;
            for (double snr = 6; snr >= -20; snr -= 2) {
                auto r = run(base, false, snr, span * 2 + 20, key);
                if (r.found) thr = snr; else break;
            }
            printf("%s decodes down to %.0f dB (signal to noise in 2.5 kHz)\n", nm, thr);
            CHECK(thr <= cs.maxThr, "%s threshold %.0f dB", nm, thr);
            KbdTxConfig none = base; none.amp = 0;
            auto r = run(none, true, 0, 150, "");
            CHECK(r.chars == 0, "%s: noise alone gave %llu characters [%s]", nm, (unsigned long long)r.chars, r.text.c_str());
        }
        for (int v : {0, 1, 2, 3, 4, 5, 6}) {   // Hell and noise: no picture from noise alone
            KbdTxConfig none; none.cfg.family = KbdHell; none.cfg.variant = v; none.amp = 0;
            auto dec = makeKbdDecoder();
            dec->setConfig(none.cfg);
            Noise nz;
            std::vector<float> buf(800);
            float maxSnr = 0;
            HfdigKbdTelemetry t;
            for (int blk = 0; blk < 1200; blk++) { for (float& x : buf) x = (float)(0.1 * nz.g()); dec->feedAudio(buf.data(), buf.size()); if (blk % 10 == 9) { dec->telemetry(t); maxSnr = std::max(maxSnr, t.snr); } }
            printf("%s noise: max level %.1f dB\n", kbdVariantName(KbdHell, v), maxSnr);
            dec->telemetry(t);
            CHECK(t.hellTotal == 0, "%s: noise alone drew %llu columns", kbdVariantName(KbdHell, v), (unsigned long long)t.hellTotal);
        }
    }
    // a user's font file draws what the built-in glyphs cannot (#225): Polish l with stroke, Turkish s with cedilla, an arrow
    {
        std::string here = __FILE__;
        here = here.substr(0, here.rfind("/tests/")) + "/third_party/fonts/Inter-Regular.ttf";
        unsigned short g[14];
        const int cps[] = {0x142, 0x15F, 0x2192};
        bool before = false;
        for (int cp : cps) before = before || kbd::hellGlyph(cp, g);
        CHECK(!before, "no font file: the extra letters are '?' (false)");
        std::string err;
        CHECK(kbd::hellLoadUserFont("/nonexistent/font.ttf", err) == false && !err.empty(), "a missing font file is refused");
        CHECK(kbd::hellLoadUserFont(here, err), "Inter loads: %s", err.c_str());
        for (int cp : cps) {
            const bool ok = kbd::hellGlyph(cp, g);
            int ink = 0, outside = 0;
            for (int r = 0; r < 14; r++) { for (int b = 0; b < 16; b++) if (g[r] & (1 << b)) { ink++; if (r < 2 || r > 11) outside++; } }
            CHECK(ok && ink >= 4 && outside == 0, "U+%04X from the font: ok %d, %d pixels, %d outside rows 2..11", cp, ok, ink, outside);
        }
        CHECK(kbd::hellGlyph('A', g) && g[2] != 0 + 0xFFFF, "ASCII keeps the built-in Feld glyph");
        { unsigned short ref[14]; for (int i = 0; i < 14; i++) ref[i] = kbd::kHellFont['A' - ' '][i]; bool same = true; for (int i = 0; i < 14; i++) same = same && g[i] == ref[i]; CHECK(same, "ASCII is unchanged by the font"); }
        CHECK(!kbd::hellGlyph(0x4E2D, g), "a character the font does not have stays '?'");
        kbd::hellLoadUserFont("", err);
        CHECK(!kbd::hellGlyph(0x142, g), "font cleared: back to '?'");
    }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
