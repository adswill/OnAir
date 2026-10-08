// Known answers for the weather fax layer, from fldigi src/wefax/wefax.cxx (which follows HAMFAX / ITU-T T.3 / WMO 386):
//  - ioc_to_width(): width = IOC * pi cut to an integer
//  - APT start tone 300 Hz (IOC 576) or 675 Hz (IOC 288), APT stop tone 450 Hz for 5 s (m_stop_duration = 5)
//  - black 1500 Hz, white 2300 Hz; the carrier is 1900 Hz, deviation 400 Hz
//  - phasing line (tx state TXPHASING): white for the first 2.5 % and the last 2.5 % of the line, black in between;
//    after the phasing one white line (ENDPHASING), then the picture, top row first, left pixel first
// plus the "1500 cycles in 5 s" figure for the 300 Hz start tone from the research notes (5 s x 300 Hz).
// The second half builds a transmission in the test, written from that description and not from the generator, and
// checks that the decoder finds the width, the line rate and the picture edges at the right places.
#include "data/marine/fax/fax_util.h"
using namespace faxt;

// Rising zero crossings with linear interpolation (times in samples).
static std::vector<double> crossings(const std::vector<float>& a, size_t from, size_t to) {
    std::vector<double> t;
    for (size_t i = from + 1; i < to; i++)
        if (a[i - 1] < 0 && a[i] >= 0) t.push_back((i - 1) + (0 - a[i - 1]) / (a[i] - a[i - 1]));
    return t;
}

int main() {
    FCHECK(faxImageWidth(576) == 1809, "width for IOC 576: %d", faxImageWidth(576));
    FCHECK(faxImageWidth(288) == 904, "width for IOC 288: %d", faxImageWidth(288));

    // Tone cycles in the generator: 300 Hz x 5 s = 1500, 675 Hz x 5 s = 3375, 450 Hz x 5 s = 2250.
    for (int ioc : {576, 288}) {
        const double fs = 12000;
        FaxAudioSource src(fs, ioc, 120, 2, 1);
        src.setTiming(5, 1, 5, 1);
        std::vector<float> a(static_cast<size_t>(13.5 * fs));
        src.generate(a.data(), a.size());
        const size_t s1 = static_cast<size_t>(5 * fs);
        const auto c1 = crossings(a, 0, s1);
        const double want1 = ioc == 576 ? 1500 : 3375;
        FCHECK(std::fabs(static_cast<double>(c1.size()) - want1) <= 1, "ioc %d start tone cycles %zu, want %.0f", ioc, c1.size(), want1);
        // The stop tone starts after 5 s tone + 1 s phasing (2 lines) + 1 white line + 2 picture lines = 7.5 s.
        const auto c2 = crossings(a, static_cast<size_t>(7.5 * fs), static_cast<size_t>(12.5 * fs));
        FCHECK(std::fabs(static_cast<double>(c2.size()) - 2250) <= 1, "stop tone cycles %zu, want 2250", c2.size());
        // The black signal after it: 1500 Hz.
        const auto c3 = crossings(a, static_cast<size_t>(12.6 * fs), static_cast<size_t>(13.4 * fs));
        FCHECK(std::fabs(static_cast<double>(c3.size()) - 1200) <= 2, "black signal cycles %zu, want 1200", c3.size());
        printf("ioc %d: start tone %zu cycles, stop tone %zu, black %zu\n", ioc, c1.size(), c2.size(), c3.size());
    }

    // Phasing line shape in the generator: 2.5 % white, 95 % black, 2.5 % white.
    {
        const double fs = 12000;
        FaxAudioSource src(fs, 576, 120, 2, 1);
        src.setTiming(1, 4, 1, 1);               // 8 phasing lines of 0.5 s
        std::vector<float> a(static_cast<size_t>(6 * fs));
        src.generate(a.data(), a.size());
        const double P = fs * 0.5, t0 = 1 * fs;
        const auto c = crossings(a, static_cast<size_t>(t0 - 2), static_cast<size_t>(t0 + 8 * P));
        // Frequency from the spacing of crossings; white = above 1900 Hz.
        double white = 0, total = 0, firstWhiteEnd = -1, lastWhiteStart = -1;
        for (size_t i = 1; i < c.size(); i++) {
            const double dt = c[i] - c[i - 1];
            const double f = fs / dt;
            const double mid = 0.5 * (c[i] + c[i - 1]) - t0;
            if (mid < 0 || mid >= 3 * P) continue;       // look at the first 3 lines
            total += dt;
            if (f > 1900) {
                white += dt;
                const double ph = std::fmod(mid, P) / P;
                if (mid < P && ph < 0.05) firstWhiteEnd = std::max(firstWhiteEnd, std::fmod(mid, P) / P);
                if (mid < P && ph > 0.95 && lastWhiteStart < 0) lastWhiteStart = ph;
            }
        }
        const double frac = white / total;
        printf("phasing line: white %.4f of the line, first white to %.4f, last white from %.4f\n", frac, firstWhiteEnd, lastWhiteStart);
        FCHECK(std::fabs(frac - 0.05) < 0.004, "white fraction of a phasing line %.4f, want 0.05", frac);
        FCHECK(firstWhiteEnd > 0.02 && firstWhiteEnd < 0.03, "white at the start ends at %.4f of the line, want 0.025", firstWhiteEnd);
        FCHECK(lastWhiteStart > 0.97 && lastWhiteStart < 0.98, "white at the end starts at %.4f of the line, want 0.975", lastWhiteStart);
    }

    // An independent transmitter (integer sample arithmetic, written from the description above).
    {
        const int fs = 12000, lpm = 120, rows = 40, W = faxImageWidth(576);
        const int spl = fs * 60 / lpm;           // 6000 samples per line
        // Nine vertical bands of the picture: black, white, black, white, mid grey, dark, light, black, white.
        const int lev[9] = {0, 255, 0, 255, 128, 64, 192, 0, 255};
        const int bandW = W / 9;
        auto pixel = [&](int col) { return lev[std::min(8, col / bandW)]; };
        std::vector<float> a;
        double phase = 0;
        auto put = [&](double f) { phase += 2 * M_PI * f / fs; a.push_back(static_cast<float>(0.5 * std::sin(phase))); };
        for (int i = 0; i < 5 * fs; i++) put(300);                                   // APT start
        for (int line = 0; line < 20; line++)                                        // phasing, 20 lines (m_tx_phasing_lin)
            for (int i = 0; i < spl; i++) put((i < spl / 40 || i >= spl - spl / 40) ? 2300 : 1500);
        for (int i = 0; i < spl; i++) put(2300);                                     // ENDPHASING
        for (int row = 0; row < rows; row++)
            for (int i = 0; i < spl; i++) put(1500 + 800.0 * pixel(static_cast<int>(static_cast<int64_t>(i) * W / spl)) / 255.0);
        for (int i = 0; i < 5 * fs; i++) put(450);                                   // APT stop
        for (int i = 0; i < 2 * fs; i++) put(1500);
        FaxDecoder d;
        d.configure(fs);
        feedAll(d, a, 5000);
        FaxImage im; uint64_t seq = 0;
        FCHECK(d.latestImage(im, seq), "no image from the independent transmission");
        const FaxStatus st = d.status();
        printf("independent TX: state %d ioc %d lpm %d width %d height %d (want %d rows + 1 white line)\n", st.state, st.ioc, st.lpm, im.width, im.height, rows);
        FCHECK(st.state == 3 && st.ioc == 576 && st.lpm == 120, "state %d ioc %d lpm %d", st.state, st.ioc, st.lpm);
        FCHECK(im.width == W && im.height == rows + 1, "image %dx%d", im.width, im.height);
        if (im.width == W && im.height == rows + 1) {
            // Row 0 is the white line; then the 40 rows of bands. Grey levels inside the bands and the edge positions.
            double worst = 0, edgeErr = 0;
            int nEdge = 0;
            for (int r = 5; r < rows - 5; r++) {
                const uint8_t* row = &im.pix[static_cast<size_t>(r + 1) * W];
                for (int b = 0; b < 9; b++) {
                    double m = 0; int n = 0;
                    for (int x = b * bandW + 12; x < (b + 1) * bandW - 12; x++) { m += row[x]; n++; }
                    worst = std::max(worst, std::fabs(m / n - lev[b]));
                }
                // 50 % crossings of the 0 -> 255 and 255 -> 0 edges at bands 0|1, 1|2, 2|3.
                for (int e = 1; e <= 3; e++) {
                    const int x0 = e * bandW;
                    double pos = -1;
                    for (int x = x0 - 25; x < x0 + 25; x++) {
                        const double a0 = row[x], a1 = row[x + 1];
                        if ((a0 - 127.5) * (a1 - 127.5) < 0) { pos = x + (127.5 - a0) / (a1 - a0) + 0.5; break; }
                    }
                    if (pos >= 0) { edgeErr = std::max(edgeErr, std::fabs(pos - x0)); nEdge++; }
                }
            }
            printf("independent TX: worst grey error %.1f, worst edge position error %.2f px over %d edges\n", worst, edgeErr, nEdge);
            FCHECK(worst < 6.0, "grey level error %.1f", worst);
            FCHECK(nEdge == 3 * (rows - 10) && edgeErr < 2.0, "edges found %d, worst position error %.2f px", nEdge, edgeErr);
            const uint8_t* white = &im.pix[0];
            int dark = 0;
            for (int x = 0; x < W; x++) if (white[x] < 200) dark++;
            FCHECK(dark < 20, "row 0 (the white line after the phasing) has %d dark pixels", dark);
        }
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
