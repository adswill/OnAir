// DMR through an echo (simulcast / urban repeater): voice bursts found with the symbol equaliser off and on, over echo delays of 1-3 symbols and
// strengths from -12 to -3 dB, base station and direct mode, through the real receiver. The equaliser is off by default; this records what it does through the real receiver and fails only on a clear loss.
#include "dect2/dmr/dmr_eval.h"
#include "dect2/voice/sym_eq.h"
#include "../common/impair.h"
#include "../common/test_long.h"
#include <cstdio>
using namespace dect2;

int main() {
    int fails = 0, offSum = 0, onSum = 0, sent = 0;
    printf("direct delay(half-sym) echo(dB)   voice bursts off/on   embedded ok off/on   BER off/on\n");
    // the quick run: the shortest and longest echo at the weakest and strongest level, both modes (8 scenarios); ONAIR_TEST_LONG=1: the whole 32
    for (int direct = 0; direct < 2; direct++)
        for (int dly : {250, 500, 1000, 1500})   // samples at 2.4 Msps: 0.5, 1, 2, 3 symbols
            for (double db : {-6.0, -3.0, 0.0, 3.0}) {
                if (!testLong() && ((dly != 250 && dly != 1500) || (db != -6.0 && db != 3.0))) continue;
                DmrScenario s;
                s.cfg.snrDb = 25; s.cfg.direct = direct; s.secs = 12;
                s.mod = [dly, db](cf32* x, size_t n, size_t, double) {
                    std::vector<cf32> v(x, x + n);
                    impair::echo(v, dly, db, 2.0);
                    std::copy(v.begin(), v.end(), x);
                };
                symeq::setEnabled(false);
                const DmrRunResult a = dmrRun(s);
                symeq::setEnabled(true);
                const DmrRunResult b = dmrRun(s);
                printf("%d      %d          %5.0f        %4d/%4d            %4d/%4d        %.4f/%.4f\n", direct, dly * 2 / 1000, db, (int)a.tel.voiceBursts, (int)b.tel.voiceBursts,
                       (int)a.tel.embOk, (int)b.tel.embOk, a.tel.ber, b.tel.ber);
                offSum += (int)a.tel.voiceBursts + (int)a.tel.embOk; onSum += (int)b.tel.voiceBursts + (int)b.tel.embOk; sent += 1;
                if ((b.tel.voiceBursts + b.tel.embOk) * 100 < (a.tel.voiceBursts + a.tel.embOk) * 85) { printf("FAIL: the equaliser lost bursts\n"); fails++; }
                if (b.sc.idsWrong > a.sc.idsWrong || b.sc.falseCalls > a.sc.falseCalls) { printf("FAIL: wrong IDs or false calls\n"); fails++; }
            }
    printf("voice + embedded bursts found over %d scenarios: off %d, on %d\n", sent, offSum, onSum);
    if (onSum * 100 < offSum * 98) { printf("FAIL: net loss over the scenarios\n"); fails++; }
    printf("%s\n", fails ? "FAILED" : "dmr_equalizer OK");
    symeq::setEnabled(false);
    return fails ? 1 : 0;
}
