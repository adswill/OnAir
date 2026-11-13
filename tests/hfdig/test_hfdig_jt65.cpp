// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF digital, JT65: the 72-bit packing, the Reed-Solomon (63,12) code with errors and erasures, then the whole receiver on synthetic audio
// (clean, near the threshold, A / B / C, frequency and time offsets, clock error, two stations, noise alone) and on the real recordings of the
// WSJT-X samples when HFDIG_WSJT_SAMPLES is set.
#include "hfdig_wsjt_testutil.h"
#include "../../core/src/hfdig/hfdig_wsjt_int.h"
#include <chrono>
using namespace wsjttest;
#include "../common/test_util.h"
using namespace dect2::wsjt;

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    const int M = ftx::kJt65;
    {   // packing: every message comes back as sent; every locator square survives
        const char* msgs[] = {"CQ K1ABC FN42", "K1ABC W9XYZ EN37", "W9XYZ K1ABC -15", "K1ABC W9XYZ R-03", "K1ABC W9XYZ RRR", "K1ABC W9XYZ 73",
                              "K1ABC W9XYZ RO", "CQ DX K1ABC FN42", "CQ EU DL1XYZ JO62", "QRZ K1ABC FN42", "G4ABC DL1XYZ +05", "G4ABC DL1XYZ R+12",
                              "CQ 123 K1ABC FN42", "CQ 5 K1ABC FN42", "K1ABC W9XYZ", "3DA0XYZ K1ABC FN42", "TNX BOB 73 GL", "HELLO WORLD", "CQ PJ4/K1ABC", "DE K1ABC FN42"};
        for (const char* m : msgs) {
            uint8_t s[12];
            const bool ok = packJt72(m, s);
            const std::string back = unpackJt72(s);
            CHECK(ok && back == m, "pack \"%s\" -> \"%s\"", m, back.c_str());
        }
        int bad = 0;
        for (int a = 0; a < 18; a++) for (int b = 0; b < 18; b++) for (int c = 0; c < 10; c++) for (int d = 0; d < 10; d++) {
            if (a == 10 && b == 0) continue;   // KA and LA squares stand for signal reports
            if (a == 11 && b == 0) continue;
            const std::string g = std::string(1, (char)('A' + a)) + (char)('A' + b) + (char)('0' + c) + (char)('0' + d);
            int ng = -1;
            if (!packGrid15(g, ng) || ng < 0 || ng >= 32400 || unpackGrid15(ng) != g) bad++;
        }
        CHECK(!bad, "%d locator squares do not survive packing", bad);
        // a known answer from the standard: the 28-bit number of "K1ABC" is 37 * 36 * 10 * 27 * 27 * 27 ... computed by hand: (((((n(' ')=36) * 36 + 'K'=20) * 10 + 1) * 27 + 'A'-10) ...
        uint8_t s[12];
        packJt72("CQ K1ABC FN42", s);
        const long nc1 = ((long)s[0] << 22) + ((long)s[1] << 16) + ((long)s[2] << 10) + ((long)s[3] << 4) + ((s[4] >> 2) & 15);
        CHECK(nc1 == 262177560 + 1, "CQ is NBASE + 1 (%ld)", nc1);
        const long nc2 = ((long)(s[4] & 3) << 26) + ((long)s[5] << 20) + ((long)s[6] << 14) + ((long)s[7] << 8) + ((long)s[8] << 2) + ((s[9] >> 4) & 3);
        const long k1abc = ((((36L * 36 + 20) * 10 + 1) * 27 + 0) * 27 + 1) * 27 + 2;   // ' ' K 1 A B C -> 36, 20, 1, 0, 1, 2
        CHECK(nc2 == k1abc, "K1ABC is %ld (expected %ld)", nc2, k1abc);
    }
    {   // Reed-Solomon (63,12): a codeword has no remainder; errors and erasures up to e + 2 t = 51 are corrected, nothing beyond is claimed for random words
        Gauss g(7);
        int okAll = 0, tot = 0, falseOk = 0;
        for (int trial = 0; trial < 200; trial++) {
            uint8_t d[12], sent[63], cw[63];
            for (auto& v : d) v = (uint8_t)(g.u() * 64);
            rsEncode63(d, sent);
            for (int i = 0; i < 63; i++) cw[62 - i] = sent[i];
            uint8_t chk[63];
            memcpy(chk, cw, 63);
            CHECK(rsDecode63(chk, nullptr, 0) == 0, "a codeword has zero syndromes");
            const int ne = (int)(g.u() * 40), nt = (int)(g.u() * ((51 - ne) / 2 + 1));
            int pos[63];
            for (int i = 0; i < 63; i++) pos[i] = i;
            for (int i = 62; i > 0; i--) std::swap(pos[i], pos[(int)(g.u() * (i + 1))]);
            memcpy(chk, cw, 63);
            for (int i = 0; i < ne; i++) chk[pos[i]] = (uint8_t)(g.u() * 64);
            for (int i = 0; i < nt; i++) chk[pos[ne + i]] ^= (uint8_t)(1 + (int)(g.u() * 63));
            const int r = rsDecode63(chk, pos, ne);
            tot++;
            if (r >= 0 && !memcmp(chk, cw, 63)) okAll++;
            // beyond the limit
            memcpy(chk, cw, 63);
            for (int i = 0; i < 40; i++) chk[pos[i]] ^= (uint8_t)(1 + (int)(g.u() * 63));
            if (rsDecode63(chk, nullptr, 0) >= 0 && memcmp(chk, cw, 63)) falseOk++;
        }
        CHECK(okAll == tot, "RS: %d of %d correctable words corrected", okAll, tot);
        CHECK(falseOk == 0, "RS: %d false decodes with 40 errors", falseOk);
    }
    const std::vector<St> one = {{"CQ K1ABC FN42", 1500, -10, 0.0}};
    {   // clean, strong
        Imp f;
        const auto d = decode(audio(M, one, 2, f, 3), M, f);
        print(d);
        const FtxDecode* x = find(d, "CQ K1ABC FN42");
        CHECK(x && std::fabs(x->hz - 1500) < 0.5 && std::fabs(x->dt) < 0.1, "clean JT65A");
        CHECK(d.size() == 2, "clean: %d decodes (2 slots)", (int)d.size());
        if (x) CHECK(std::fabs(x->snrDb + 10) < 2.5, "SNR estimate %.1f dB against -10", x->snrDb);
    }
    {   // the three sub-modes
        for (int sub = 0; sub < 3; sub++) {
            Imp f; f.sub = sub;
            const auto d = decode(audio(M, {{"K1ABC W9XYZ -15", 1200, -16, 0.4}}, 1, f, 5), M, f);
            CHECK(find(d, "K1ABC W9XYZ -15"), "JT65%c not decoded", 'A' + sub);
        }
    }
    {   // strong, medium and weak signals in A, B and C (regression: strong B / C used to be missed, the sync search was fooled by the leakage)
        int bad = 0;
        for (int sub = 0; sub < 3; sub++)
            for (double snr : {25.0, 10.0, 5.0, 0.0, -10.0, -18.0}) {
                Imp f; f.sub = sub;
                const auto d = decode(audio(M, {{"K1ABC W9XYZ -15", 1200, snr, 0.3}}, 1, f, 40 + sub), M, f);
                const bool ok = find(d, "K1ABC W9XYZ -15") != nullptr;
                if (!ok) bad++;
                CHECK(ok, "JT65%c at %+.0f dB not decoded", 'A' + sub, snr);
            }
        printf("strong/medium/weak: %d missed of 18\n", bad);
    }
    {   // threshold sweep: how many of 6 slots decode at each level (SNR in 2500 Hz, noise only changes with the seed)
        for (double snr : {-18.0, -21.0, -23.0, -24.0, -25.0}) {
            Imp f;
            int ok = 0, n = 6;
            for (int k = 0; k < n; k++) {
                const auto d = decode(audio(M, {{"CQ K1ABC FN42", 1000 + 37.3 * k, snr, 0.1 * k}}, 1, f, 100 + k), M, f);
                if (find(d, "CQ K1ABC FN42")) ok++;
                for (const auto& x : d) CHECK(x.msg == "CQ K1ABC FN42", "false decode \"%s\" at %.0f dB", x.msg.c_str(), snr);
            }
            printf("JT65A %5.1f dB: %d of %d\n", snr, ok, n);
            if (snr >= -21) CHECK(ok == n, "%.0f dB: %d of %d", snr, ok, n);
            if (snr == -23) CHECK(ok >= 4, "%.0f dB: %d of %d", snr, ok, n);
        }
    }
    {   // frequency offsets (fractions of a tone spacing and whole tones), time offsets, clock error, dial error
        struct C { double hz, dt, ppm, dial; };
        const C cs[] = {{700.0, -2.4, 0, 0}, {701.3, 2.4, 0, 0}, {1802.7, 0.5, 0, 0}, {2700.4, 0.0, 0, 0}, {1500, 0.3, 100, 0}, {1500, 0.3, -100, 0},
                        {1500, -1.1, 0, 100}, {1500, 1.7, 0, -100}, {250.0, 0.2, 0, 0}};
        for (const C& c : cs) {
            Imp f; f.ppm = c.ppm; f.dialHz = c.dial;
            const auto d = decode(audio(M, {{"W9XYZ K1ABC EN37", c.hz, -19, c.dt}}, 1, f, 9), M, f);
            const FtxDecode* x = find(d, "W9XYZ K1ABC EN37");
            CHECK(x && std::fabs(x->dt - c.dt) < 0.15, "f %.1f dt %.1f ppm %.0f dial %.0f: %s", c.hz, c.dt, c.ppm, c.dial, x ? "wrong DT" : "not decoded");
        }
    }
    {   // Doppler drift: all frequencies change in proportion (0.15 Hz/s at 1500 Hz, 7 Hz over the transmission), both ways, and a stronger one
        for (double dr : {0.15, -0.15, 0.3}) {
            Imp f; f.drift = dr;
            const auto d = decode(audio(M, {{"W9XYZ K1ABC EN37", 1500, -17, 0.2}}, 1, f, 21), M, f);
            CHECK(find(d, "W9XYZ K1ABC EN37"), "drift %.2f Hz/s: not decoded", dr);
        }
    }
    {   // two stations, 40 Hz apart, and a third at the far end
        Imp f;
        const std::vector<St> st = {{"CQ K1ABC FN42", 1000, -16, 0.0}, {"K1ABC W9XYZ -12", 1090, -14, 0.5}, {"CQ DX G4ABC IO91", 2300, -18, -0.7}};
        const auto d = decode(audio(M, st, 1, f, 12), M, f);
        print(d);
        for (const auto& s : st) CHECK(find(d, s.msg), "\"%s\" not decoded", s.msg.c_str());
    }
    {   // noise alone: no decodes (5 slots)
        Imp f;
        const auto d = decode(audio(M, {}, 5, f, 77), M, f);
        CHECK(d.empty(), "noise alone: %d decodes", (int)d.size());
    }
    if (const char* dir = std::getenv("HFDIG_WSJT_SAMPLES")) {   // real recordings of the WSJT-X samples (JT65B, 12 kHz)
        (void)dir;
        int total = 0;
        for (const char* n : {"JT65__JT65B__000000_0001", "JT65__JT65B__000000_0002", "JT65__JT65B__000000_0003", "JT65__JT65B__000000_0004",
                              "JT65__JT65B__000000_0005", "JT65__JT65B__000000_0006", "JT65__JT65B__000000_0007", "JT65__JT65B__DL7UAE_040308_002400"}) {
            auto a = sample(n);
            if (a.empty()) continue;
            a.resize(a.size() + 8000 * 8, 0.f);   // the recording is shorter than the slot window (a start up to 8 s late)
            Imp f; f.sub = 1;
            HfdigFtxTelemetry tel;
            const auto d = decode(a, M, f, kT0, &tel);
            printf("%s: %d decodes\n", n, (int)d.size());
            print(d);
            total += (int)d.size();
            if (std::string(n).find("DL7UAE") != std::string::npos) CHECK(find(d, "K1JT DL7UAE JO62"), "the real JT65B recording of K1JT and DL7UAE");
        }
        printf("real JT65B recordings: %d decodes\n", total);
    }
    printf("%.1f s\n%s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
