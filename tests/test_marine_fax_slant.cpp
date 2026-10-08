// Slant: an audio clock error of up to +-50 ppm is measured from the phasing lines and corrected, so that over 800 lines
// the picture drifts by less than a pixel. Also the manual correction (auto off) and a manual change in mid-picture.
// A clock error is made by running the test source at rate * (1 + ppm * 1e-6) while the decoder believes in the nominal rate.
// 1 ppm over one line is 1e-6 * 6000 samples = 0.006 sample = 0.0018 pixel; over 800 lines 1.45 pixels, so 30 ppm is 43 pixels
// of slant if nothing corrects it.
#include "data/marine/fax/fax_util.h"
using namespace faxt;

struct Result { Quality q; FaxStatus st; int height = 0; };

static Result run(const Opts& o, void (*setup)(FaxDecoder&) = nullptr, double changeAtS = -1, double newPpm = 0, bool autoOn = true) {
    const auto a = makeAudio(o);
    FaxDecoder d;
    d.configure(o.rate);
    d.setAutoSlant(autoOn);
    if (setup) setup(d);
    size_t i = 0;
    const size_t changeAt = changeAtS >= 0 ? static_cast<size_t>(changeAtS * o.rate) : a.size();
    while (i < a.size()) {
        if (i == changeAt) d.setSlantPpm(newPpm);
        size_t n = std::min<size_t>(4096, a.size() - i);
        if (i < changeAt && i + n > changeAt) n = changeAt - i;
        d.push(a.data() + i, n);
        i += n;
    }
    FaxImage im; uint64_t seq = 0;
    d.latestImage(im, seq);
    const FaxImage ref = faxTestChart(faxImageWidth(o.ioc), o.lines, o.seed);
    Result r;
    r.q = compare(im, ref, 1, 40);
    r.st = d.status();
    r.height = im.height;
    return r;
}

int main() {
    // Clock error sweep, 800 lines, both signs.
    for (double ppm : {-50.0, -30.0, -10.0, 0.0, 10.0, 30.0, 50.0}) {
        Opts o; o.lines = 800; o.ppm = ppm;
        const Result r = run(o);
        printf("%+5.0f ppm: estimate %+7.2f ppm, drift over 800 lines %+.2f px, corr %.3f, height %d\n", ppm, r.st.slantPpm, r.q.drift, r.q.meanCorr, r.height);
        FCHECK(r.q.ok && r.height == 801, "%+.0f ppm: image height %d", ppm, r.height);
        FCHECK(std::fabs(r.q.drift) < 1.0, "%+.0f ppm: drift %.2f px over 800 lines", ppm, r.q.drift);
        FCHECK(std::fabs(r.st.slantPpm - ppm) < 2.0, "%+.0f ppm: estimated %+.2f", ppm, r.st.slantPpm);
        FCHECK(r.q.meanCorr > 0.97, "%+.0f ppm: correlation %.3f", ppm, r.q.meanCorr);
    }
    // The same at 8 kHz audio and with noise (15 dB audio SNR).
    {
        Opts o; o.lines = 800; o.ppm = 30; o.rate = 8000;
        const Result r = run(o);
        printf("30 ppm at 8 kHz: estimate %+.2f, drift %+.2f px\n", r.st.slantPpm, r.q.drift);
        FCHECK(std::fabs(r.q.drift) < 1.0 && std::fabs(r.st.slantPpm - 30) < 2.0, "8 kHz: drift %.2f estimate %.2f", r.q.drift, r.st.slantPpm);
        Opts n; n.lines = 800; n.ppm = -30; n.snrDb = 15;
        const Result rn = run(n);
        printf("-30 ppm at 15 dB SNR: estimate %+.2f, drift %+.2f px, corr %.3f\n", rn.st.slantPpm, rn.q.drift, rn.q.meanCorr);
        FCHECK(std::fabs(rn.q.drift) < 1.0 && std::fabs(rn.st.slantPpm + 30) < 2.0, "noisy: drift %.2f estimate %.2f", rn.q.drift, rn.st.slantPpm);
        Opts m; m.lines = 800; m.ppm = 30; m.snrDb = 8;
        const Result rm = run(m);
        printf("+30 ppm at 8 dB SNR: estimate %+.2f, drift %+.2f px, corr %.3f\n", rm.st.slantPpm, rm.q.drift, rm.q.meanCorr);
        FCHECK(std::fabs(rm.q.drift) < 2.5, "8 dB: drift %.2f px", rm.q.drift);
    }
    // Correction off: the slant shows (about 1.5 px per 1 ppm per 800 lines = 44 px at 30 ppm, row shift grows linearly).
    {
        Opts o; o.lines = 800; o.ppm = 30;
        const Result r = run(o, nullptr, -1, 0, false);
        // The drift is measured between the middle of the first and of the last fifth of the rows, 576 rows apart:
        // 30e-6 * 1809 px * 576 rows = 31.3 px (44 px between the first and the last row).
        const double want = 30e-6 * 1809 * 576;
        printf("30 ppm, auto off, manual 0: drift %+.1f px (expected %.1f in size)\n", r.q.drift, want);
        FCHECK(std::fabs(std::fabs(r.q.drift) - want) < 0.1 * want, "uncorrected drift %.1f px, expected %.1f", r.q.drift, want);
        // Manual value: same correction as the measured one.
        const Result m = run(o, [](FaxDecoder& d) { d.setSlantPpm(30); }, -1, 0, false);
        printf("30 ppm, auto off, manual 30: drift %+.2f px, slant %.1f\n", m.q.drift, m.st.slantPpm);
        FCHECK(std::fabs(m.q.drift) < 1.5, "manual correction: drift %.2f px", m.q.drift);
        // Manual trim on top of auto: +10 added to the measured 30 leaves a residual slant of 10 ppm = 10.4 px.
        const Result t = run(o, [](FaxDecoder& d) { d.setSlantPpm(10); });
        const double wantT = 10e-6 * 1809 * 576;
        printf("30 ppm, auto on, manual trim 10: drift %+.1f px (expected %.1f in size)\n", t.q.drift, wantT);
        FCHECK(std::fabs(std::fabs(t.q.drift) - wantT) < 0.1 * wantT, "trim: drift %.1f px, expected %.1f", t.q.drift, wantT);
        // Changing the manual value in the middle of the picture moves the rows already received too.
        // Picture starts at 5 + 30 + 0.5 s; change at 5 + 30 + 0.5 + 200 s (line 400).
        const Result c = run(o, nullptr, 235.5, 30, false);
        printf("30 ppm, auto off, manual set to 30 at line 400: drift %+.2f px\n", c.q.drift);
        FCHECK(std::fabs(c.q.drift) < 2.0, "manual change in mid-picture: drift %.2f px", c.q.drift);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
