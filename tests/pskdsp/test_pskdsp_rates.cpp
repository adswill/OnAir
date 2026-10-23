// PSK demodulator analysis: the symbol rate, modulation and carrier of BPSK / QPSK / OQPSK / 8PSK found blindly, from 75 baud to 100 kilobaud
// and in captures from 4 kHz to 1 MHz wide, with the carrier a good way from the centre.
#include "pskdsp_testutil.h"

int main() {
    const double rates[][2] = {{75, 4000}, {150, 8000}, {300, 8000}, {600, 16000}, {1200, 48000}, {2400, 48000}, {4800, 48000},
                               {9600, 96000}, {19200, 96000}, {38400, 192000}, {100000, 1000000}};
    int n = 0;
    for (int oi = 0; oi < 4; oi++)
        for (const auto& r : rates) {
            Case c; c.order = allOrders()[oi]; c.fs = r[1]; c.baud = r[0]; c.off = 0.13 * r[1]; c.tag = "rate";
            CHECK(runCase(c), "rate %.0f %s", r[0], orderName(c.order));
            n++;
        }
    // standard rates are snapped, others are measured
    {
        Analysis a;
        Case c; c.baud = 9600; c.fs = 96000;
        CHECK(runCase(c, &a) && a.baudSnapped && a.baud == 9600.0, "9600 baud snapped to the standard rate (got %.3f)", a.baud);
        Case d; d.baud = 5555; d.fs = 48000; d.tag = "odd";
        CHECK(runCase(d, &a) && !a.baudSnapped, "5555 baud is not a standard rate (snapped %d)", (int)a.baudSnapped);
    }
    printf("test_pskdsp_rates: %d signals\n", n);
    return testSummary("test_pskdsp_rates");
}
