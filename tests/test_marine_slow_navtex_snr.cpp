// NAVTEX through the radio layer: character error rate against signal to noise ratio (noise in 3 kHz), without and with a two-path
// fading channel (1 ms, 0.5 Hz Doppler spread).
#include "data/marine/rf_util.h"
using namespace mt;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const double snrNo[] = {6, 0, -2, -4, -5, -6, -8};
    const double snrFade[] = {12, 6, 3, 0, -2, -4};
    double firstFail[2] = {99, 99};
    for (int fade = 0; fade < 2; fade++) {
        printf("%s\n  SNR(3 kHz)  messages  exact  CER (edit distance / characters)\n", fade ? "two-path fading" : "no fading");
        const double* list = fade ? snrFade : snrNo;
        const int count = fade ? 6 : 7;
        for (int k = 0; k < count; k++) {
            const double snr = list[k];
            Scenario s; s.secs = 100; s.snr = snr; s.fade = fade; s.idle = 2; s.msg = 4;
            Outcome o = run(s);
            const NavScore sc = scoreNavtex(o.tel);
            const double cer = sc.chars ? (double)sc.dist / sc.chars : 1.0;
            printf("  %6.1f      %4d     %4d   %.4f\n", snr, sc.msgs, sc.exact, cer);
            if (sc.exact < 5 && firstFail[fade] == 99) firstFail[fade] = snr;       // about 5.5 messages are sent in 100 s
            if (!fade && snr >= -4) { CHECK(sc.exact >= 5 && cer < 0.005, "no fading, %.1f dB: %d exact, CER %.4f", snr, sc.exact, cer); }
            if (fade && snr >= 6) { CHECK(sc.exact >= 5, "fading, %.1f dB: %d exact", snr, sc.exact); }
            if (fade && snr >= 0) { CHECK(cer < 0.05, "fading, %.1f dB: CER %.4f", snr, cer); }
        }
    }
    printf("fewer than 5 exact messages of about 5.5 sent: first at %.1f dB without fading, %.1f dB with fading\n", firstFail[0], firstFail[1]);
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
