// LoRa sensitivity per spreading factor: the share of CRC-good frames against the SNR in the 125 kHz band, through the Signal Lab demodulator
// (the Channelizer / Demod pair the LoRa (raw) receiver uses), with an optional carrier offset and sample clock error.
// Usage: sens_lora_sf <sf> <snrFrom> <snrTo> <step> <frames> [cfoHz] [sroPpm] [bwHz]
// Simulated signals only: the generator's own LoRa modulator, complex white noise.
#include "dect2/loraraw/loraraw_gen.h"
#include "dect2/loraraw/loraraw_lab.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::lora;

static int run(int sf, double bw, double snr, int frames, double cfo, double sro, uint32_t seed, int* ghost) {
    const double rate = 4 * bw;
    LoraRawGenConfig g;
    g.rate = rate; g.dialOffsetHz = 0; g.snrDb = snr; g.loop = false; g.leadSec = 0.05; g.seed = seed; g.amp = 0.3; g.cfoHz = cfo; g.sroPpm = sro;
    double dur = g.leadSec + 0.2;
    for (int i = 0; i < frames; i++) {
        LoraRawGenFrame f;
        f.p.sf = sf; f.p.bwHz = bw; f.p.cr = 5; f.p.syncWord = 0x12; f.p.crc = true; f.p.preamble = 8; f.p.ldro = autoLdro(sf, bw);
        const std::string t = "OnAir LoRa sens frame " + std::to_string(i);
        f.payload.assign(t.begin(), t.end());
        f.p.implicitLen = (int)f.payload.size();
        f.gapSec = 0.3;
        dur += airSeconds(f.p, f.payload.size()) + f.gapSec;
        g.frames.push_back(f);
    }
    LoraRawGenerator gen(g);
    std::vector<cf32> x((size_t)(dur * rate));
    gen.generate(x.data(), x.size());
    LabSettings s;
    s.sf = sf; s.bwHz = bw; s.syncWord = 0x12; s.preamble = 8; s.ldro = -1;
    const LabResult r = labDemodulate(x.data(), x.size(), rate, s);
    int good = 0;
    for (const LabFrame& f : r.frames) {
        const std::string p(f.payload.begin(), f.payload.end());
        if (f.crcOk && p.compare(0, 21, "OnAir LoRa sens frame") == 0) good++;
        else if (f.crcOk) (*ghost)++;
    }
    return good;
}

int main(int argc, char** argv) {
    if (argc < 6) { printf("usage: sens_lora_sf sf from to step frames [cfoHz] [sroPpm] [bwHz]\n"); return 2; }
    const int sf = atoi(argv[1]);
    const double lo = atof(argv[2]), hi = atof(argv[3]), st = atof(argv[4]);
    const int n = atoi(argv[5]);
    const double cfo = argc > 6 ? atof(argv[6]) : 0, sro = argc > 7 ? atof(argv[7]) : 0, bw = argc > 8 ? atof(argv[8]) : 125e3;
    for (double snr = hi; snr >= lo - 1e-9; snr -= st) {
        int good = 0, tot = 0, ghost = 0;
        for (int rep = 0; rep < 4; rep++) { good += run(sf, bw, snr, n / 4, cfo, sro, 11 + (uint32_t)rep, &ghost); tot += n / 4; }
        printf("SF%d %6.1f dB: %3.0f %% (%d of %d), wrong-text CRC-good frames %d\n", sf, snr, 100.0 * good / tot, good, tot, ghost);
        fflush(stdout);
    }
    return 0;
}
