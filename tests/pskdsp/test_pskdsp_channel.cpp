// PSK demodulator carrier finder: the centre, width and SNR of a narrow signal anywhere in the span, a 25 Bd-wide signal in an 8 Msps capture,
// a DC spike left alone, the nearest of two signals, and nothing in pure noise.
#include "pskdsp_testutil.h"

static std::vector<cf> sig(Order o, double fs, double baud, double off, double esn0, int frames, uint32_t seed = 1) {
    GenSpec g; g.order = o; g.fs = fs; g.baud = baud; g.offsetHz = off; g.esn0Db = esn0; g.frames = frames; g.seed = seed;
    return generate(g);
}

int main() {
    // the carrier, wherever it is in the span, and a bandwidth that follows the symbol rate (the -3 dB width of RRC pulses is the rate)
    for (double off : {-20000.0, -7000.0, 0.0, 1500.0, 12345.0, 21000.0}) {
        const auto x = sig(Order::Bpsk, 48000, 2400, off, 25, 60);
        const Carrier c = findCarrier(x.data(), x.size(), 48000);
        CHECK(c.found && std::fabs(c.offsetHz - off) < 60, "carrier at %.0f Hz found at %.1f", off, c.offsetHz);
        CHECK(c.found && c.bw3Hz > 0.8 * 2400 && c.bw3Hz < 1.25 * 2400, "-3 dB width %.0f Hz for 2400 baud (at %.0f)", c.bw3Hz, off);
        CHECK(c.found && c.bw10Hz >= c.bw3Hz && c.occHz >= c.bw10Hz * 0.95 && c.snrDb > 5 && c.peakDb > 10, "widths %.0f / %.0f / %.0f, SNR %.1f, peak %.1f", c.bw3Hz, c.bw10Hz, c.occHz, c.snrDb, c.peakDb);
    }
    // a narrow signal in a wide capture: the span is zoomed
    {
        const auto x = sig(Order::Bpsk, 2000000, 1200, 150000, 25, 4);   // about 1 s at 2 Msps
        const Carrier c = findCarrier(x.data(), x.size(), 2000000);
        CHECK(c.found && std::fabs(c.offsetHz - 150000) < 150, "1200 baud in 2 Msps: found at %.1f Hz", c.offsetHz);
        CHECK(c.found && c.bw3Hz > 800 && c.bw3Hz < 1600, "its -3 dB width %.0f Hz", c.bw3Hz);
    }
    // a DC spike is not a signal; the signal beside it is
    {
        auto x = sig(Order::Qpsk, 48000, 1200, 5000, 25, 30);
        for (auto& v : x) v += cf(0.5f, -0.3f);
        const Carrier c = findCarrier(x.data(), x.size(), 48000);
        CHECK(c.found && std::fabs(c.offsetHz - 5000) < 60, "DC spike ignored: found %.1f", c.offsetHz);
    }
    // two signals: the strongest, or the one near a given frequency
    {
        auto a = sig(Order::Bpsk, 48000, 1200, -8000, 25, 30);
        const auto b = sig(Order::Bpsk, 48000, 1200, 9000, 25, 30, 7);
        for (size_t i = 0; i < a.size() && i < b.size(); i++) a[i] += 0.5f * b[i];
        const Carrier s = findCarrier(a.data(), a.size(), 48000);
        CHECK(s.found && std::fabs(s.offsetHz + 8000) < 60, "strongest of two: %.1f", s.offsetHz);
        const Carrier n = findCarrier(a.data(), a.size(), 48000, true, 8500, 3000);
        CHECK(n.found && std::fabs(n.offsetHz - 9000) < 60, "nearest to 8500 Hz: %.1f", n.offsetHz);
        const Carrier far = findCarrier(a.data(), a.size(), 48000, true, 0, 1000);
        CHECK(!far.found, "nothing within 1 kHz of 0 Hz");
    }
    // noise only
    {
        const auto x = sig(Order::Bpsk, 48000, 1200, 0, -80, 20);
        const Carrier c = findCarrier(x.data(), x.size(), 48000);
        CHECK(!c.found, "noise only: found %d at %.1f", (int)c.found, c.offsetHz);
        const Carrier t = findCarrier(x.data(), 100, 48000);
        CHECK(!t.found, "too short");
    }
    return testSummary("test_pskdsp_channel");
}
