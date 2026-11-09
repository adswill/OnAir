// Sub-audible tone reader: every CTCSS tone and a set of DCS codes (both polarities) read back from synthetic audio at 48 kHz under speech-like
// interference and noise, with a frequency error, and no false tone from speech or noise alone. Simulated signals only.
#include "dect2/chanvoice/chv_subtone.h"
#include "dect2/tones/tones_gen.h"
#include "dect2/tones/tones_proto.h"
#include <cmath>
#include <cstdio>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static chv::SubTone run(const ToneScene& s) {
    chv::SubToneReader r;
    const auto& a = s.audio();
    for (size_t i = 0; i < a.size(); i += 480) r.feed48k(a.data() + i, std::min<size_t>(480, a.size() - i));
    return r.tone();
}

int main() {
    // every CTCSS tone, speech at 0.35 over it, noise 0.01 rms
    int ok = 0;
    for (int k = 0; k < tones::kCtcssN; k++) {
        ToneScene s(48000, 10 + k);
        s.noise(0, 5, 0.01); s.speech(0, 5, 0.35); s.ctcss(tones::ctcssHz(k) * 1.002, 0, 5, 0.08);   // +0.2 % tone error
        const chv::SubTone t = run(s);
        if (t.ctcssIdx == k) ok++; else printf("CTCSS %.1f read as idx %d dcs %d\n", tones::ctcssHz(k), t.ctcssIdx, t.dcsCode);
    }
    CHECK(ok == tones::kCtcssN, "CTCSS tones read: %d of 50", ok);
    // amplitude sweep: 150 Hz to 700 Hz deviation
    for (double dev : {150.0, 300.0, 500.0, 700.0}) {
        ToneScene s(48000, 77); s.noise(0, 5, 0.01); s.speech(0, 5, 0.3); s.ctcss(103.5, 0, 5, dev / 5000.0);
        const chv::SubTone t = run(s);
        CHECK(t.ctcssIdx >= 0 && std::fabs(t.ctcssHz - 103.5) < 0.1, "CTCSS 103.5 at %.0f Hz deviation: idx %d", dev, t.ctcssIdx);
    }
    // DCS: a spread of standard codes, both polarities
    const int codes[] = {23, 25, 26, 31, 43, 47, 114, 155, 205, 244, 315, 365, 411, 464, 503, 631, 703, 743};
    int dok = 0, dn = 0;
    for (int c : codes) for (int inv = 0; inv < 2; inv++) {
        ToneScene s(48000, 200 + c); s.noise(0, 6, 0.01); s.speech(0, 6, 0.3); s.dcs(c, inv != 0, 0, 6, 0.1, 0.37 * c / 100.0);
        const chv::SubTone t = run(s);
        dn++;
        // an inverted code is the same waveform as a normal one: either reading is right when it names the sent code or its alias
        if ((t.dcsCode == c && t.dcsInverted == (inv != 0)) || (t.dcsAlias == c && t.dcsAliasInverted == (inv != 0))) dok++;
        else printf("DCS %03d%c read as code %d inv %d ctcss %d\n", c, inv ? 'I' : 'N', t.dcsCode, (int)t.dcsInverted, t.ctcssIdx);
    }
    CHECK(dok == dn, "DCS codes read: %d of %d", dok, dn);
    // no tone: speech and noise only (several seeds), must stay empty
    int fp = 0;
    for (int seed = 1; seed <= 20; seed++) {
        ToneScene s(48000, 900 + seed); s.noise(0, 8, 0.02); s.speech(0, 8, 0.4);
        if (run(s).any()) fp++;
    }
    CHECK(fp == 0, "false tones on speech: %d of 20", fp);
    printf(fails ? "chv_subtone: %d FAILED\n" : "chv_subtone: all passed\n", fails);
    return fails ? 1 : 0;
}
