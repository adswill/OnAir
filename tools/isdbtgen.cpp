// isdbtgen: makes a synthetic ISDB-T broadcast as an IQ recording. Nothing here is a real transmission: it is the transmitter chain of this
// project (Reed-Solomon, interleaving, convolutional coding, OFDM with TMCC) carrying the built-in test programme, followed by a channel model
// (carrier offset, echo, noise, sample-rate error) and written as 8-bit I/Q like a HackRF would record it. Use it to try the ISDB-T receiver.
//
//   isdbtgen --out sample.cs8 [--seconds 12] [--rate 10e6] [--mode 3] [--guard 1/8] [--partial] [--layer A:1:qpsk:2/3:2] [--layer B:12:64qam:3/4:1]
//            [--cfo 2500] [--ppm 0] [--snr 28] [--echo-us 2] [--echo-db 12] [--format cs8|cf32]
//
// A layer is name:segments:modulation:rate:interleaving (0..3). Without --layer: a one-segment layer A (QPSK 2/3, the picture phones use) and a
// twelve-segment layer B (64QAM 3/4) that carries the programme. The programme goes into the last layer given, the others carry null packets.
#include "dect2/demo_ts.h"
#include "dect2/exact_resampler.h"
#include "dect2/isdbt_gen.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::isdbt;

static int parseMod(const std::string& s) { return s == "dqpsk" ? kDqpsk : s == "qpsk" ? kQpsk : s == "16qam" ? k16Qam : k64Qam; }
static int parseRate(const std::string& s) { return s == "1/2" ? kR12 : s == "2/3" ? kR23 : s == "3/4" ? kR34 : s == "5/6" ? kR56 : kR78; }

int main(int argc, char** argv) {
    std::string out, format = "cs8";
    double seconds = 12, rate = 10e6, cfo = 2500, snr = 28, echoUs = 0, echoDb = 12, ppm = 0;
    Params p;
    p.mode = 3; p.guard = kGi8;
    bool haveLayer = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--out") out = next();
        else if (a == "--seconds") seconds = atof(next().c_str());
        else if (a == "--rate") rate = atof(next().c_str());
        else if (a == "--mode") p.mode = atoi(next().c_str());
        else if (a == "--guard") { const std::string g = next(); p.guard = g == "1/4" ? kGi4 : g == "1/8" ? kGi8 : g == "1/16" ? kGi16 : kGi32; }
        else if (a == "--partial") p.partial = true;
        else if (a == "--cfo") cfo = atof(next().c_str());
        else if (a == "--ppm") ppm = atof(next().c_str());
        else if (a == "--snr") snr = atof(next().c_str());
        else if (a == "--echo-us") echoUs = atof(next().c_str());
        else if (a == "--echo-db") echoDb = atof(next().c_str());
        else if (a == "--format") format = next();
        else if (a == "--layer") {
            // A:12:64qam:3/4:1
            std::string s = next();
            std::vector<std::string> f;
            size_t pos = 0;
            for (;;) { const size_t q = s.find(':', pos); f.push_back(s.substr(pos, q == std::string::npos ? q : q - pos)); if (q == std::string::npos) break; pos = q + 1; }
            if (f.size() < 4) { fprintf(stderr, "layer: name:segments:modulation:rate[:interleaving]\n"); return 1; }
            const int li = std::toupper(f[0][0]) - 'A';
            if (li < 0 || li > 2) { fprintf(stderr, "layer name must be A, B or C\n"); return 1; }
            if (!haveLayer) { for (auto& l : p.layer) l = Layer(); haveLayer = true; }
            p.layer[li].segments = atoi(f[1].c_str()); p.layer[li].mod = parseMod(f[2]); p.layer[li].rate = parseRate(f[3]); p.layer[li].ti = f.size() > 4 ? atoi(f[4].c_str()) : 0;
        } else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
    }
    if (out.empty()) { fprintf(stderr, "usage: isdbtgen --out file.cs8 (see the top of tools/isdbtgen.cpp)\n"); return 1; }
    if (!haveLayer) {
        p.partial = true;
        p.layer[0].segments = 1; p.layer[0].mod = kQpsk; p.layer[0].rate = kR23; p.layer[0].ti = std::min(3, 2);
        p.layer[1].segments = 12; p.layer[1].mod = k64Qam; p.layer[1].rate = kR34; p.layer[1].ti = 1;
    }
    std::string why;
    if (!p.valid(&why)) { fprintf(stderr, "impossible configuration: %s\n", why.c_str()); return 1; }
    int main_ = 0;
    for (int i = 0; i < 3; i++) if (p.layer[i].used()) main_ = i;
    printf("mode %d, guard %s, %d segments%s\n", p.mode, guardName(p.guard), p.totalSegments(), p.partial ? ", one-segment layer A" : "");
    for (int i = 0; i < 3; i++) if (p.layer[i].used()) printf("  layer %c: %d segment%s %s %s, interleaving %d, %.2f Mbit/s%s\n", 'A' + i, p.layer[i].segments, p.layer[i].segments == 1 ? "" : "s", modName(p.layer[i].mod), rateName(p.layer[i].rate), interleavingLength(p.mode, p.layer[i].ti), layerBitrate(p, i) / 1e6, i == main_ ? " (the programme)" : "");
    Generator gen(p, singleLayerSource(main_, demoTsSource(layerBitrate(p, main_) * 0.9)), 1);
    const int frames = std::max(2, (int)std::ceil(seconds / frameSeconds(p.mode, p.guard)));
    std::vector<cf32> sig, frame;
    for (int f = 0; f < frames; f++) { gen.nextFrame(frame); sig.insert(sig.end(), frame.begin(), frame.end()); }
    // channel: echo (in the signal's own sample rate), carrier offset, sample-rate error, noise
    if (echoUs > 0) {
        const size_t d = (size_t)std::llround(echoUs * 1e-6 * kSampleRate);
        const float g = (float)std::pow(10.0, -echoDb / 20.0);
        std::vector<cf32> e(sig.size());
        for (size_t i = 0; i < sig.size(); i++) e[i] = sig[i] + (i >= d ? g * sig[i - d] * cf32(0.6f, 0.8f) : cf32(0, 0));
        sig.swap(e);
    }
    ExactResampler rs;
    rs.configure(kSampleRate, rate * (1.0 + ppm * 1e-6));
    std::vector<cf32> radio;
    rs.process(sig.data(), sig.size(), radio);
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 1.f);
    const float sigma = (float)std::sqrt(std::pow(10.0, -snr / 10.0) / 2.0);
    const size_t lead = (size_t)(0.05 * rate);
    std::ofstream o(out, std::ios::binary);
    if (!o) { fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    const bool f32 = format == "cf32";
    const float scale = 22.f;   // like atsc3gen: the 8-bit recording uses a good part of the range without clipping
    std::vector<uint8_t> buf;
    const size_t total = lead + radio.size();
    for (size_t i = 0; i < total; i++) {
        cf32 v = i >= lead ? radio[i - lead] : cf32(0, 0);
        const double ph = 2 * M_PI * cfo * (double)i / rate;
        v *= cf32((float)std::cos(ph), (float)std::sin(ph));
        v += cf32(nd(rng), nd(rng)) * sigma;
        if (f32) { float t[2] = {v.real(), v.imag()}; buf.insert(buf.end(), (uint8_t*)t, (uint8_t*)t + 8); }
        else { buf.push_back((uint8_t)(int8_t)std::max(-127.f, std::min(127.f, std::round(v.real() * scale)))); buf.push_back((uint8_t)(int8_t)std::max(-127.f, std::min(127.f, std::round(v.imag() * scale)))); }
        if (buf.size() >= (1 << 20)) { o.write((const char*)buf.data(), (std::streamsize)buf.size()); buf.clear(); }
    }
    o.write((const char*)buf.data(), (std::streamsize)buf.size());
    printf("wrote %s: %.1f s, %zu samples at %.4f Msps, %s I/Q, carrier offset %.0f Hz, SNR %.0f dB%s\n", out.c_str(), (double)total / rate, total, rate / 1e6, f32 ? "32-bit float" : "8-bit signed (CS8)", cfo, snr, echoUs > 0 ? ", with an echo" : "");
    return 0;
}
