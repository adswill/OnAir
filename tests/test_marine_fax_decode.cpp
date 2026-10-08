// Fax decoder: round trip from the test source for each IOC / line rate / audio rate, chunk-size independence,
// overrides, the line cap and two transmissions in a row.
#include "data/marine/fax/fax_util.h"
using namespace faxt;

static FaxImage decodeOne(const std::vector<float>& a, double rate, size_t chunk, FaxStatus* st = nullptr,
                          void (*setup)(FaxDecoder&) = nullptr) {
    FaxDecoder d;
    d.configure(rate);
    if (setup) setup(d);
    feedAll(d, a, chunk);
    FaxImage im; uint64_t seq = 0;
    d.latestImage(im, seq);
    if (st) *st = d.status();
    return im;
}

static void roundTrip(const char* name, Opts o, double minCorr) {
    const auto a = makeAudio(o);
    FaxStatus st;
    FaxImage im = decodeOne(a, o.rate, 4096, &st);
    const FaxImage ref = faxTestChart(faxImageWidth(o.ioc), o.lines, o.seed);
    const Quality q = compare(im, ref, 1);
    printf("%-22s width %d height %d (want %d) lpm %d ioc %d corr %.3f drift %.2f px slant %.1f ppm\n", name, im.width, im.height,
           o.lines + 1, st.lpm, st.ioc, q.meanCorr, q.drift, st.slantPpm);
    FCHECK(im.width == ref.width, "%s: width %d", name, im.width);
    FCHECK(im.height == o.lines + 1, "%s: height %d, want %d (the white line after the phasing comes first)", name, im.height, o.lines + 1);
    FCHECK(st.lpm == o.lpm && st.ioc == o.ioc, "%s: lpm %d ioc %d", name, st.lpm, st.ioc);
    FCHECK(st.state == 3, "%s: state %d, want stopped", name, st.state);
    FCHECK(q.ok && q.meanCorr > minCorr, "%s: row correlation %.3f below %.2f", name, q.meanCorr, minCorr);
    FCHECK(std::fabs(q.drift) < 1.0, "%s: drift %.2f px", name, q.drift);
}

int main() {
    // IOC and line rates (fldigi wefax.cxx: IOC 576 and 288, start tones 300 and 675 Hz; LPM 60 .. 240).
    { Opts o; o.lines = 150; roundTrip("ioc576 lpm120 12k", o, 0.98); }
    { Opts o; o.ioc = 288; o.lpm = 60; o.lines = 50; roundTrip("ioc288 lpm60 12k", o, 0.97); }
    { Opts o; o.lpm = 90; o.lines = 100; roundTrip("ioc576 lpm90 12k", o, 0.98); }
    { Opts o; o.lpm = 100; o.lines = 100; roundTrip("ioc576 lpm100 12k", o, 0.98); }
    { Opts o; o.lpm = 180; o.lines = 150; roundTrip("ioc576 lpm180 12k", o, 0.95); }
    { Opts o; o.lpm = 240; o.lines = 200; roundTrip("ioc576 lpm240 12k", o, 0.90); }
    { Opts o; o.ioc = 288; o.lpm = 120; o.lines = 100; roundTrip("ioc288 lpm120 12k", o, 0.97); }
    // Audio rates, including ones that do not divide the line length.
    for (double r : {8000.0, 11025.0, 16000.0, 22050.0, 44100.0, 48000.0}) {
        Opts o; o.rate = r; o.lines = 80;
        char nm[40]; snprintf(nm, sizeof nm, "ioc576 lpm120 %.0f", r);
        roundTrip(nm, o, r < 9000 ? 0.95 : 0.97);
    }
    { Opts o; o.rate = 8000; o.lpm = 90; o.lines = 80; roundTrip("ioc576 lpm90 8k", o, 0.95); }

    // The result does not depend on the chunk size.
    {
        Opts o; o.lines = 12; o.phasingS = 12;
        const auto a = makeAudio(o);
        const FaxImage ref = decodeOne(a, o.rate, 4096);
        for (size_t chunk : {size_t(1), size_t(7), size_t(1000), size_t(65536)}) {
            const FaxImage im = decodeOne(a, o.rate, chunk);
            FCHECK(im.width == ref.width && im.height == ref.height && im.pix == ref.pix, "chunk %zu: image differs from chunk 4096", chunk);
        }
        printf("chunk sizes 1, 7, 1000, 65536 give the same %dx%d image\n", ref.width, ref.height);
    }

    // Overrides: the right values change nothing; a wrong line rate finds no phasing and no image.
    {
        Opts o; o.lines = 40; o.phasingS = 12;
        const auto a = makeAudio(o);
        const FaxImage ref = decodeOne(a, o.rate, 4096);
        const FaxImage same = decodeOne(a, o.rate, 4096, nullptr, [](FaxDecoder& d) { d.setLpm(120); d.setIoc(576); });
        FCHECK(same.pix == ref.pix && same.height == o.lines + 1, "fixed lpm/ioc equal to the real ones: height %d", same.height);
        FaxStatus st;
        const FaxImage wrong = decodeOne(a, o.rate, 4096, &st, [](FaxDecoder& d) { d.setLpm(60); });
        FCHECK(wrong.height == 0, "lpm forced to 60 on a 120 lpm signal gave an image of %d rows", wrong.height);
        const FaxImage w288 = decodeOne(a, o.rate, 4096, &st, [](FaxDecoder& d) { d.setIoc(288); });
        FCHECK(w288.width == 904, "ioc forced to 288: width %d", w288.width);
    }

    // The line cap stops the image there.
    {
        Opts o; o.lines = 60; o.phasingS = 12;
        const auto a = makeAudio(o);
        FaxStatus st;
        const FaxImage im = decodeOne(a, o.rate, 4096, &st, [](FaxDecoder& d) { d.setMaxLines(25); });
        FCHECK(im.height == 25 && st.lines == 25 && st.state == 3, "cap 25: height %d lines %d state %d", im.height, st.lines, st.state);
    }

    // Joining a transmission late: without the start tone the phasing alone is enough (IOC falls back to 576 unless set);
    // joining in the picture gives nothing until the next transmission.
    {
        Opts o; o.lines = 60; o.phasingS = 20; o.seconds = 80;
        const auto a = makeAudio(o);
        const FaxImage ref = faxTestChart(1809, 60, 1);
        const size_t skip = static_cast<size_t>(9 * o.rate);      // the tone ends at 5 s, the phasing at 25 s
        FaxStatus st;
        const FaxImage im = decodeOne(std::vector<float>(a.begin() + skip, a.end()), o.rate, 4096, &st);
        const Quality q = compare(im, ref, 1);
        printf("joined in the phasing: %dx%d lpm %d corr %.3f\n", im.width, im.height, st.lpm, q.meanCorr);
        FCHECK(im.height == 61 && st.lpm == 120 && q.meanCorr > 0.97, "late join in the phasing: height %d lpm %d corr %.3f", im.height, st.lpm, q.meanCorr);
        const size_t mid = static_cast<size_t>(40 * o.rate);       // in the picture (starts at 25.5 s, ends at 55.5 s)
        const FaxImage im2 = decodeOne(std::vector<float>(a.begin() + mid, a.begin() + static_cast<size_t>(60 * o.rate)), o.rate, 4096, &st);
        FCHECK(im2.height == 0 && st.state == 3, "joined in the picture: height %d state %d (the stop tone ends it)", im2.height, st.state);
    }

    // Two transmissions in a row (the source loops): the second replaces the first, the sequence number moves.
    {
        Opts o; o.lines = 60; o.phasingS = 12; o.seconds = 115;
        const auto a = makeAudio(o);
        FaxDecoder d;
        d.configure(o.rate);
        uint64_t seq = 0; FaxImage im; int changes = 0, entered = 0, last = -1;
        for (size_t i = 0; i < a.size(); i += 4096) {
            d.push(a.data() + i, std::min<size_t>(4096, a.size() - i));
            if (d.latestImage(im, seq)) changes++;
            const int s = d.status().state;
            if (s == 2 && last != 2) entered++;
            last = s;
        }
        printf("loop: image changes %d, images started %d, final image %dx%d\n", changes, entered, im.width, im.height);
        FCHECK(entered == 2, "images started: %d, want 2", entered);
        FCHECK(changes > 100, "image updates: %d", changes);
        FCHECK(im.height == 61, "last image height %d", im.height);
        const FaxImage ref = faxTestChart(1809, 60, 1);
        const Quality q = compare(im, ref, 1);
        FCHECK(q.ok && q.meanCorr > 0.97, "last image correlation %.3f", q.meanCorr);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
