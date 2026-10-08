// Fax decoder under impairments: noise down to its failure point, mistuning of +-50 Hz, DC offset, 8-bit audio, low level,
// slow fading, dropouts of 20 ms in the tone, the phasing and the picture, and reset() in the middle of a transmission.
// Noise is white over 0 .. rate/2 (6 kHz), SNR relative to the 0.5 amplitude carrier: the fax occupies about 3.6 kHz of it.
#include "data/marine/fax/fax_util.h"
using namespace faxt;

struct Out { FaxImage im; FaxStatus st; Quality q; double goodRows = 0; };

static Out run(const Opts& o, int lines) {
    const auto a = makeAudio(o);
    FaxDecoder d;
    d.configure(o.rate);
    feedAll(d, a, 4096);
    Out r;
    uint64_t seq = 0;
    d.latestImage(r.im, seq);
    r.st = d.status();
    const FaxImage ref = faxTestChart(faxImageWidth(o.ioc), lines, o.seed);
    r.q = compare(r.im, ref, 1, 20);
    const auto rc = rowCorrelations(r.im, ref, 1);
    int good = 0, n = 0;
    for (double c : rc) if (c > -1.5) { n++; if (c > 0.9) good++; }
    r.goodRows = n ? static_cast<double>(good) / n : 0;
    return r;
}

int main() {
    // 1. SNR sweep.
    printf("SNR sweep (100 lines, IOC 576, 120 lpm, 12 kHz audio):\n  SNR dB | lock | rows | drift px | row corr | rows >0.9 | level SNR est\n");
    double lockFloor = 99, readableFloor = 99;
    for (double snr : {40.0, 30.0, 25.0, 20.0, 15.0, 12.0, 10.0, 8.0, 6.0, 4.0, 2.0, 0.0, -3.0, -6.0}) {
        Opts o; o.lines = 100; o.snrDb = snr;
        const Out r = run(o, 100);
        const bool lock = r.st.lpm == 120 && r.im.height == 101 && std::fabs(r.q.drift) < 1.0 && std::fabs(r.st.slantPpm) < 5;
        if (lock) lockFloor = snr;
        if (r.q.meanCorr > 0.7) readableFloor = snr;
        printf("  %6.1f | %-4s | %4d | %8.2f | %8.3f | %9.2f | %6.1f\n", snr, lock ? "yes" : "no", r.im.height, r.q.drift, r.q.meanCorr, r.goodRows, r.st.snrDb);
        if (snr >= 20) FCHECK(r.q.meanCorr > 0.93 && lock, "SNR %.0f dB: corr %.3f lock %d", snr, r.q.meanCorr, lock);
        else if (snr >= 10) FCHECK(r.q.meanCorr > 0.70 && lock, "SNR %.0f dB: corr %.3f lock %d", snr, r.q.meanCorr, lock);
        else if (snr >= 2) FCHECK(lock, "SNR %.0f dB: line lock lost (height %d drift %.2f slant %.1f)", snr, r.im.height, r.q.drift, r.st.slantPpm);
    }
    printf("line lock and slant estimate hold down to %.0f dB; picture correlation stays above 0.7 down to %.0f dB\n", lockFloor, readableFloor);

    // 2. Mistuning: every tone shifted by up to 50 Hz (a USB tuning error).
    for (double mt : {-50.0, -30.0, 30.0, 50.0}) {
        Opts o; o.lines = 100; o.mistuneHz = mt;
        const Out r = run(o, 100);
        printf("mistune %+4.0f Hz: black level %.1f Hz (want %.0f), corr %.3f, rows %d, tone %.1f\n", mt, r.st.blackHz, 1500 + mt, r.q.meanCorr, r.im.height, r.st.toneHz);
        FCHECK(r.im.height == 101 && r.q.meanCorr > 0.98, "mistune %+.0f: height %d corr %.3f", mt, r.im.height, r.q.meanCorr);
        FCHECK(std::fabs(r.st.blackHz - (1500 + mt)) < 3.0, "mistune %+.0f: black level %.1f", mt, r.st.blackHz);
    }
    {
        Opts o; o.lines = 100; o.mistuneHz = 50; o.ppm = 30; o.snrDb = 20;
        const Out r = run(o, 100);
        printf("mistune +50 Hz, +30 ppm, 20 dB SNR: corr %.3f drift %.2f slant %.1f\n", r.q.meanCorr, r.q.drift, r.st.slantPpm);
        FCHECK(r.im.height == 101 && r.q.meanCorr > 0.9 && std::fabs(r.q.drift) < 1.0, "combined: height %d corr %.3f drift %.2f", r.im.height, r.q.meanCorr, r.q.drift);
    }

    // 3. DC offset, 8-bit audio, low level, fading.
    {
        Opts o; o.lines = 100; o.dc = 0.25;
        const Out r = run(o, 100);
        printf("DC offset 0.25: corr %.3f rows %d\n", r.q.meanCorr, r.im.height);
        FCHECK(r.im.height == 101 && r.q.meanCorr > 0.97, "DC: height %d corr %.3f", r.im.height, r.q.meanCorr);
        Opts q; q.lines = 100; q.quantBits = 8;
        const Out rq = run(q, 100);
        printf("8-bit audio: corr %.3f rows %d\n", rq.q.meanCorr, rq.im.height);
        FCHECK(rq.im.height == 101 && rq.q.meanCorr > 0.97, "8 bit: height %d corr %.3f", rq.im.height, rq.q.meanCorr);
        Opts l; l.lines = 100; l.gain = 0.01;
        const Out rl = run(l, 100);
        printf("level -40 dB: corr %.3f rows %d\n", rl.q.meanCorr, rl.im.height);
        FCHECK(rl.im.height == 101 && rl.q.meanCorr > 0.97, "low level: height %d corr %.3f", rl.im.height, rl.q.meanCorr);
        Opts f; f.lines = 100; f.fadeDepth = 0.9; f.fadeHz = 0.3; f.snrDb = 30;
        const Out rf = run(f, 100);
        printf("fading (envelope 0.1 .. 1.9, 0.3 Hz) with 30 dB mean SNR: corr %.3f, rows >0.9 %.2f, rows %d\n", rf.q.meanCorr, rf.goodRows, rf.im.height);
        FCHECK(rf.im.height == 101 && std::fabs(rf.q.drift) < 1.0 && rf.goodRows > 0.6, "fading: height %d drift %.2f good rows %.2f", rf.im.height, rf.q.drift, rf.goodRows);
    }

    // 4. Dropouts of 20 ms: in the start tone (t = 2 s), the phasing (t = 20 s) and the picture (t = 60 s, 61.3 s, 62 s).
    {
        Opts o; o.lines = 100; o.gaps = {2.0, 20.0, 20.5, 60.0, 61.3, 62.0};
        const Out r = run(o, 100);
        printf("20 ms dropouts: rows %d, drift %.2f, rows >0.9 %.3f, slant %.2f\n", r.im.height, r.q.drift, r.goodRows, r.st.slantPpm);
        FCHECK(r.im.height == 101 && std::fabs(r.q.drift) < 1.0, "dropouts: height %d drift %.2f", r.im.height, r.q.drift);
        FCHECK(r.goodRows > 0.95, "dropouts: only %.2f of the rows are good", r.goodRows);
    }
    {   // A longer one in the picture (0.3 s) costs about one row and nothing else.
        Opts o; o.lines = 100; o.gaps = {70.0}; o.gapS = 0.3;
        const Out r = run(o, 100);
        printf("0.3 s dropout in the picture: rows %d, drift %.2f, rows >0.9 %.3f\n", r.im.height, r.q.drift, r.goodRows);
        FCHECK(r.im.height == 101 && std::fabs(r.q.drift) < 1.0 && r.goodRows > 0.95, "long dropout: height %d drift %.2f good %.2f", r.im.height, r.q.drift, r.goodRows);
    }

    // 5. reset() in mid-picture: the decoder starts over and decodes the next transmission (the source loops).
    {
        Opts o; o.lines = 40; o.phasingS = 12; o.seconds = 100;
        const auto a = makeAudio(o);
        FaxDecoder d;
        d.configure(o.rate);
        const size_t cut = static_cast<size_t>(25 * o.rate);       // picture runs from 17.5 s to 37.5 s
        feedAll(d, std::vector<float>(a.begin(), a.begin() + cut), 4096);
        FCHECK(d.status().state == 2 && d.status().lines > 5, "before reset: state %d lines %d", d.status().state, d.status().lines);
        d.reset();
        FCHECK(d.status().state == 0 && d.status().lines == 0, "after reset: state %d lines %d", d.status().state, d.status().lines);
        FaxImage im; uint64_t seq = 0;
        d.latestImage(im, seq);
        FCHECK(im.height == 0, "image not cleared by reset (height %d)", im.height);
        feedAll(d, std::vector<float>(a.begin() + cut, a.end()), 1234);
        d.latestImage(im, seq);
        const FaxImage ref = faxTestChart(1809, 40, 1);
        const Quality q = compare(im, ref, 1, 8);
        printf("reset mid-picture: next transmission decoded, %dx%d rows, corr %.3f, state %d\n", im.width, im.height, q.meanCorr, d.status().state);
        FCHECK(im.height == 41 && q.meanCorr > 0.9, "after reset: height %d corr %.3f", im.height, q.meanCorr);
        // reset in the middle of the phasing and of a tone, then a clean run.
        for (double at : {10.0, 3.0}) {
            FaxDecoder e; e.configure(o.rate);
            const size_t c2 = static_cast<size_t>(at * o.rate);
            feedAll(e, std::vector<float>(a.begin(), a.begin() + c2), 700);
            e.reset();
            feedAll(e, std::vector<float>(a.begin() + c2, a.end()), 700);
            e.latestImage(im, seq = 0);
            FCHECK(im.height == 41, "reset at %.0f s: height %d", at, im.height);
        }
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
