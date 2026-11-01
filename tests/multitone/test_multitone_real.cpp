// Multitone on the real recording (Multitone_2015-12-31T01-37-17Z_27599.8kHz, the FM audio, first 8.2 s at 8 kHz, 8 bit): an FFSK burst with the tones
// f and 2f (about 293 and 586 Hz), then a tone sequence. What is checked is what is measured: the FFSK is found, every bit is sent twice, a 12-bit marker
// comes back every 24 bits and the words after it come in repeated pairs. No meaning is claimed for the words (the format is not public).
#include "dect2/multitone/multitone_rx.h"
#include "dect2/multitone/multitone_tel.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const std::string path = std::string(DECT2_SOURCE_DIR) + "/tests/data/multitone/owner_multitone_8k_s8.raw";
    std::ifstream f(path, std::ios::binary);
    CHECK(f.good(), "cannot open %s", path.c_str());
    if (!f.good()) return testSummary("multitone real");
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(raw.size() == 65600, "%zu samples", raw.size());
    // the audio as the 48 kHz discriminator signal the analysis works on: x6 by linear interpolation, peak 1 rad / sample
    std::vector<float> a(raw.size());
    for (size_t i = 0; i < raw.size(); i++) a[i] = (float)(int8_t)raw[i] / 100.f;
    auto disc = [&](double t0, double t1) {
        std::vector<float> d;
        for (size_t i = (size_t)(t0 * 8000); i + 1 < (size_t)(t1 * 8000) && i + 1 < a.size(); i++)
            for (int k = 0; k < 6; k++) d.push_back(a[i] + (a[i + 1] - a[i]) * (float)k / 6.f);
        return d;
    };
    const std::vector<float> fsk = disc(0.0, 2.95);
    const multitone::FskResult r = multitone::sliceFsk(fsk, 48000);
    CHECK(r.found && r.ffsk && r.nBits > 400, "FFSK found %d ffsk %d bits %d", r.found, r.ffsk, r.nBits);
    CHECK(std::fabs(r.hiHz - 586) < 12 && std::fabs(r.loHz - 293) < 6, "tones %.1f / %.1f Hz", r.loHz, r.hiHz);
    MtBurst b;
    b.baud = r.baud; b.ffsk = r.ffsk;
    multitone::analyseBits(r.bits, b);
    CHECK(b.dupBits == 2 && std::fabs(b.dataBaud - 293) < 6, "bits sent %d times, data rate %.1f", b.dupBits, b.dataBaud);
    CHECK(b.markerBits == 12 && b.markerGap == 24 && b.markerCount >= 8, "marker %d bits %s, %d times, every %d", b.markerBits, b.marker.c_str(), b.markerCount, b.markerGap);
    CHECK(b.frameWords.size() >= 8 && b.doubledWords >= 3 && b.distinctWords >= 3 && b.distinctWords <= 12, "%zu words, %d doubled, %d distinct", b.frameWords.size(), b.doubledWords, b.distinctWords);
    printf("  %s\n", b.verdict.c_str());
    // the tone part (4.3 .. 5.8 s): no data in it
    const multitone::FskResult t = multitone::sliceFsk(disc(4.4, 5.8), 48000);
    CHECK(!t.found, "a steady tone is not data");
    return testSummary("multitone real");
}
