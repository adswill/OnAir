// isdbtrx: decodes an ISDB-T IQ recording offline and writes the transport stream.
//   isdbtrx recording.cs8 --rate 10e6 [--format cs8|cu8|cf32] [--out out.ts]
#include "dect2/isdbt_rx.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace dect2;

int main(int argc, char** argv) {
    std::string path, out, format = "cs8";
    double rate = 10e6;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = atof(argv[++i]);
        else if (a == "--format" && i + 1 < argc) format = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (path.empty()) path = a;
    }
    if (path.empty()) { fprintf(stderr, "usage: isdbtrx recording --rate 10e6 [--format cs8|cu8|cf32] [--out out.ts]\n"); return 1; }
    std::ifstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    IsdbtReceiver rx;
    rx.configure(rate);
    std::vector<uint8_t> ts;
    long packets = 0, errors = 0;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
        for (size_t i = 0; i < n; i++) { packets++; if (p[i * 188 + 1] & 0x80) errors++; }
        ts.insert(ts.end(), p, p + n * 188);
    });
    const size_t bps = format == "cf32" ? 8 : 2, block = 1 << 18;
    std::vector<unsigned char> raw(block * bps);
    std::vector<cf32> x(block);
    long total = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (f) {
        f.read((char*)raw.data(), (std::streamsize)raw.size());
        const size_t got = (size_t)f.gcount() / bps;
        if (!got) break;
        for (size_t i = 0; i < got; i++) {
            if (format == "cf32") { float re, im; memcpy(&re, &raw[i * 8], 4); memcpy(&im, &raw[i * 8 + 4], 4); x[i] = cf32(re, im); }
            else if (format == "cu8") x[i] = cf32(((int)raw[2 * i] - 127.5f) / 127.5f, ((int)raw[2 * i + 1] - 127.5f) / 127.5f);
            else x[i] = cf32((int8_t)raw[2 * i] / 128.f, (int8_t)raw[2 * i + 1] / 128.f);
        }
        rx.feed(x.data(), got);
        total += (long)got;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    RxTelemetry t;
    rx.telemetry(t, 0);
    printf("read %.2f s of signal in %.1f s (%.1fx real time)\n", total / rate, secs, total / rate / std::max(1e-9, secs));
    printf("ISDB-T: mode %d, guard interval index %d, TMCC %s, carrier offset %.1f Hz, SNR %.1f dB, clock %.1f ppm\n", t.isdbt.mode, t.giIdx, t.isdbt.tmccOk ? "decoded" : "not found", t.cfoHz, t.dataSnrDb, t.sroPpm);
    for (int i = 0; i < 3; i++) {
        const auto& L = t.isdbt.layer[i];
        if (!L.segments) continue;
        printf("  layer %c: %d segments, modulation %d, rate %d, interleaving %d: %llu packets (%llu clean, %llu corrected, %llu failed)\n", 'A' + i, L.segments, L.mod, L.rate, L.ti,
               (unsigned long long)L.packets, (unsigned long long)L.rsClean, (unsigned long long)L.rsCorrected, (unsigned long long)L.rsFailed);
    }
    printf("transport stream: %ld packets, %ld with errors, %zu bytes\n", packets, errors, ts.size());
    if (!out.empty()) { std::ofstream o(out, std::ios::binary); o.write((const char*)ts.data(), (std::streamsize)ts.size()); printf("written to %s\n", out.c_str()); }
    return ts.empty() ? 2 : 0;
}
