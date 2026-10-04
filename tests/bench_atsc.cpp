// Where does the ATSC receiver lose against an ideal one? Packet error rate against SNR, two ways:
//   ideal : the field encoder's symbols plus Gaussian noise go straight into the Viterbi + Reed-Solomon decoder (no front end at all)
//   chain : the generated 8-VSB signal with the same noise goes through the whole receiver (timing, carrier, equaliser, decoder)
// SNR is the data power over the noise power in the 5.38 MHz Nyquist band, the way the generator defines it.
//   bench_atsc [from to step] [fields]
#include "dect2/atsc.h"
#include "dect2/atsc_gen.h"
#include "dect2/atsc_rx.h"
#include "dect2/dvbt_gen.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
using namespace dect2;

// The equalised levels of every field of the receiver for one noise level.
static std::vector<std::vector<float>> levelsFor(double snr, int fields) {
    atsc::ChannelConfig cc; cc.snrDb = snr;
    const double rate = 10e6;
    atsc::Generator gen(dvbt::testTsSource(), cc, rate, 3);
    AtscReceiver rx;
    rx.configure(rate);
    rx.setBlocking(true);
    std::vector<std::vector<float>> all;
    rx.setLevelTap([&](const std::vector<float>& lv) { all.push_back(lv); });
    std::vector<cf32> blk;
    const size_t total = (size_t)(rate * (fields + 14) * atsc::kFieldSyms / atsc::kSymbolRate);
    for (size_t done = 0; done < total; done += blk.size()) { blk.clear(); gen.generate(1 << 15, blk); rx.feed(blk.data(), blk.size()); }
    rx.flush();
    return all;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "ideal") {   // bench_atsc ideal from to step fields: the decoder alone, many fields
        const double from = atof(argv[2]), to = atof(argv[3]), step = atof(argv[4]);
        const int fields = atoi(argv[5]);
        for (double snr = from; snr <= to + 1e-9; snr += step) {
            atsc::FieldEncoder enc; atsc::FieldDecoder dec;
            auto src = dvbt::testTsSource();
            std::mt19937 rng(11);
            std::normal_distribution<float> nd(0.f, (float)std::sqrt(21.0 / std::pow(10.0, snr / 10.0)));
            std::vector<uint8_t> ts(312 * 188), syms((size_t)atsc::kFieldSyms), out(312 * 188);
            std::vector<float> lv((size_t)atsc::kFieldSyms);
            long pk = 0, bad = 0;
            for (int f = 0; f < fields + 4; f++) {
                for (int p = 0; p < 312; p++) src(&ts[p * 188]);
                enc.encode(ts.data(), f & 1, syms.data());
                for (size_t i = 0; i < syms.size(); i++) lv[i] = atsc::levelOf(syms[i]) + nd(rng);
                atsc::FieldStats st;
                const int n = dec.decode(lv.data(), out.data(), &st);
                if (f < 4) continue;
                pk += n; for (int k = 0; k < n; k++) if (out[k * 188 + 1] & 0x80) bad++;
            }
            printf("ideal %.2f dB: %ld bad of %ld packets (%.5f%%)\n", snr, bad, pk, 100.0 * bad / std::max(1L, pk));
            fflush(stdout);
        }
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "fresh") {   // bench_atsc fresh snr fields gain: the receiver's equalised levels through a fresh decoder
        const double snr = atof(argv[2]); const int fields = atoi(argv[3]); const double g = atof(argv[4]);
        const auto lv = levelsFor(snr, fields);
        atsc::FieldDecoder fd; long pk = 0, bad = 0; std::vector<uint8_t> out(312 * 188);
        for (size_t f = 0; f < lv.size(); f++) {
            std::vector<float> v(lv[f]); for (auto& x : v) x = (float)(x / g);
            atsc::FieldStats st; const int np = fd.decode(v.data(), out.data(), &st);
            if (f < 14) continue;
            pk += np; for (int k = 0; k < np; k++) if (out[k * 188 + 1] & 0x80) bad++;
        }
        printf("fresh decoder on the receiver's levels (SNR %.2f, gain %.3f): %ld bad of %ld packets (%.4f%%)\n", snr, g, bad, pk, 100.0 * bad / std::max(1L, pk));
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "hybrid") {   // bench_atsc hybrid var fields: the receiver's noise-free levels plus our own Gaussian noise
        const double var = atof(argv[2]); const int fields = atoi(argv[3]);
        const auto lv = levelsFor(99, fields);
        std::mt19937 rng(5); std::normal_distribution<float> nd(0.f, (float)std::sqrt(var));
        atsc::FieldDecoder fd; long pk = 0, bad = 0; std::vector<uint8_t> out(312 * 188);
        for (size_t f = 0; f < lv.size(); f++) {
            std::vector<float> v(lv[f]); for (auto& x : v) x += nd(rng);
            atsc::FieldStats st; const int np = fd.decode(v.data(), out.data(), &st);
            if (f < 14) continue;
            pk += np; for (int k = 0; k < np; k++) if (out[k * 188 + 1] & 0x80) bad++;
        }
        printf("noise-free levels of the receiver + Gaussian noise (variance %.3f): %ld bad of %ld packets (%.4f%%)\n", var, bad, pk, 100.0 * bad / std::max(1L, pk));
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "shuffle") {   // bench_atsc shuffle snr fields: what in the receiver's noise hurts the decoder?
        const double snr = atof(argv[2]); const int fields = atoi(argv[3]);
        const auto clean = levelsFor(99, fields), noisy = levelsFor(snr, fields);
        const size_t n = std::min(clean.size(), noisy.size());
        std::mt19937 rng(9);
        const char* names[] = {"as it is (gain put back)", "shuffled over the whole field", "shuffled inside each segment", "power of every segment made equal"};
        for (int mode = 0; mode < 4; mode++) {
            atsc::FieldDecoder fd; long pk = 0, bad = 0; std::vector<uint8_t> out(312 * 188);
            double g = 0, gx = 0; for (size_t f = 12; f < n; f++) for (int i = atsc::kSegSyms; i < atsc::kFieldSyms; i++) { g += noisy[f][(size_t)i] * clean[f][(size_t)i]; gx += clean[f][(size_t)i] * clean[f][(size_t)i]; }
            g /= gx;
            for (size_t f = 0; f < n; f++) {
                std::vector<float> v(clean[f]), e(clean[f].size());
                for (size_t i = 0; i < e.size(); i++) e[i] = (float)(noisy[f][i] / g - clean[f][i]);
                if (mode == 1) { std::vector<size_t> idx; for (size_t i = atsc::kSegSyms; i < e.size(); i++) idx.push_back(i); std::vector<float> t; for (auto i : idx) t.push_back(e[i]); std::shuffle(t.begin(), t.end(), rng); for (size_t k = 0; k < idx.size(); k++) e[idx[k]] = t[k]; }
                if (mode >= 2) for (int sg = 1; sg < atsc::kFieldSegs; sg++) {
                    float* r = &e[(size_t)sg * atsc::kSegSyms + 4];
                    if (mode == 2) std::shuffle(r, r + 828, rng);
                    else { double p = 0; for (int k = 0; k < 828; k++) p += r[k] * r[k]; const double sc = std::sqrt(0.597 / (p / 828)); for (int k = 0; k < 828; k++) r[k] = (float)(r[k] * sc); }
                }
                for (size_t i = 0; i < v.size(); i++) v[i] += e[i];
                atsc::FieldStats st; const int np = fd.decode(v.data(), out.data(), &st);
                if (f < 14) continue;
                pk += np; for (int k = 0; k < np; k++) if (out[k * 188 + 1] & 0x80) bad++;
            }
            printf("  %-36s %ld bad of %ld (%.4f%%)\n", names[mode], bad, pk, 100.0 * bad / std::max(1L, pk));
        }
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "levels") {   // bench_atsc levels from to step: the noise on the equalised symbols
        const double from = argc > 2 ? atof(argv[2]) : 15, to = argc > 3 ? atof(argv[3]) : 18, step = argc > 4 ? atof(argv[4]) : 1;
        const auto clean = levelsFor(99, 30);
        {   // the equaliser's own error with no noise at all: distance of the clean levels from the nearest level
            double dd = 0; long c = 0;
            for (size_t f = 12; f < clean.size(); f++)
                for (int i = atsc::kSegSyms; i < atsc::kFieldSyms; i++) {
                    if (i % atsc::kSegSyms < 4) continue;
                    const double v = clean[f][(size_t)i], nearest = std::max(-7.0, std::min(7.0, 2.0 * std::floor(v * 0.5) + 1.0));
                    dd += (v - nearest) * (v - nearest); c++;
                }
            printf("clean run: %zu fields, error of the equalised symbols without noise %.4f (SNR %.1f dB)\n", clean.size(), dd / c, 10 * std::log10(21.0 / (dd / c)));
        }
        for (double snr = from; snr <= to + 1e-9; snr += step) {
            const auto noisy = levelsFor(snr, getenv("BENCH_FIELDS") ? 150 : 30);
            const size_t n = std::min(clean.size(), noisy.size());
            double sum = 0; long cnt = 0;
            for (size_t f = 12; f < n; f++)
                for (int i = atsc::kSegSyms; i < atsc::kFieldSyms; i++) { if (i % atsc::kSegSyms < 4) continue; const double e = noisy[f][(size_t)i] - clean[f][(size_t)i]; sum += e * e; cnt++; }
            // gain and bias of the noisy levels against the clean ones, how Gaussian and how white the noise is
            double sxy = 0, sxx = 0, mx = 0, my = 0, c1 = 0, k4 = 0;
            for (size_t f = 12; f < n; f++)
                for (int i = atsc::kSegSyms; i < atsc::kFieldSyms; i++) { if (i % atsc::kSegSyms < 4) continue; mx += clean[f][(size_t)i]; my += noisy[f][(size_t)i]; }
            mx /= cnt; my /= cnt;
            for (size_t f = 12; f < n; f++)
                for (int i = atsc::kSegSyms; i < atsc::kFieldSyms; i++) {
                    if (i % atsc::kSegSyms < 4) continue;
                    const double x = clean[f][(size_t)i] - mx, y = noisy[f][(size_t)i] - my;
                    sxy += x * y; sxx += x * x;
                    const double e = noisy[f][(size_t)i] - clean[f][(size_t)i];
                    k4 += e * e * e * e;
                    if (i % atsc::kSegSyms >= 5) c1 += e * (noisy[f][(size_t)i - 1] - clean[f][(size_t)i - 1]);
                }
            {   // is the noise the same everywhere? by position in the field and in the segment
                double bySeg[8] = {}, byPos[8] = {}; long cs[8] = {}, cp[8] = {};
                for (size_t f = 12; f < n; f++)
                    for (int i = atsc::kSegSyms; i < atsc::kFieldSyms; i++) {
                        const int sp = i % atsc::kSegSyms; if (sp < 4) continue;
                        const double e = noisy[f][(size_t)i] - clean[f][(size_t)i];
                        const int sg = i / atsc::kSegSyms - 1;
                        bySeg[sg * 8 / 312] += e * e; cs[sg * 8 / 312]++;
                        byPos[sp * 8 / 832] += e * e; cp[sp * 8 / 832]++;
                    }
                printf("  noise by position in the field:  "); for (int k = 0; k < 8; k++) printf(" %.3f", bySeg[k] / cs[k]); printf("\n");
                printf("  noise by position in the segment:"); for (int k = 0; k < 8; k++) printf(" %.3f", byPos[k] / cp[k]); printf("\n");
            }
            if (getenv("BENCH_FIELDS")) {   // the noise of every field: is there a field that stands out?
                for (size_t f = 12; f < n; f++) {
                    double s2 = 0; long c = 0, big = 0;
                    for (int i = atsc::kSegSyms; i < atsc::kFieldSyms; i++) { if (i % atsc::kSegSyms < 4) continue; const double e = noisy[f][(size_t)i] - clean[f][(size_t)i]; s2 += e * e; c++; if (std::fabs(e) > 3.0) big++; }
                    printf("    field %zu: noise %.3f, %ld samples beyond 3.0\n", f, s2 / c, big);
                }
            }
            if (getenv("BENCH_ACF")) {   // correlation of the noise between symbols at distance 1..30
                std::vector<double> acf(31, 0.0); std::vector<long> an(31, 0);
                for (size_t f = 12; f < n; f++)
                    for (int i = atsc::kSegSyms + 40; i < atsc::kFieldSyms; i++) {
                        if (i % atsc::kSegSyms < 4) continue;
                        const double e0 = noisy[f][(size_t)i] - clean[f][(size_t)i];
                        for (int d = 0; d <= 30; d++) { const int j = i - d; if (j % atsc::kSegSyms < 4 || j < atsc::kSegSyms) continue; acf[d] += e0 * (noisy[f][(size_t)j] - clean[f][(size_t)j]); an[d]++; }
                    }
                printf("  noise correlation at lags 1..30:");
                for (int d = 1; d <= 30; d++) printf(" %.3f", (acf[d] / an[d]) / (acf[0] / an[0]));
                printf("\n");
            }
            {   // mean and spread of the noisy level for each of the eight clean levels
                double m[8] = {}, q[8] = {}; long c[8] = {};
                for (size_t f = 12; f < n; f++)
                    for (int i = atsc::kSegSyms; i < atsc::kFieldSyms; i++) {
                        if (i % atsc::kSegSyms < 4) continue;
                        const double cl = clean[f][(size_t)i];
                        const int k = (int)std::lround((std::max(-7.0, std::min(7.0, cl)) + 7) / 2);
                        const double e = noisy[f][(size_t)i] - (2 * k - 7);
                        m[k] += e; q[k] += e * e; c[k]++;
                    }
                printf("  per level (-7..+7): mean error / error power:");
                for (int k = 0; k < 8; k++) printf("  %+.2f/%.3f", m[k] / c[k], q[k] / c[k] - (m[k] / c[k]) * (m[k] / c[k]));
                printf("\n");
            }
            if (getenv("BENCH_DUMP")) {   // the noise, segment by segment (828 data symbols each), for looking at it in other tools
                FILE* fp = fopen(getenv("BENCH_DUMP"), "wb");
                for (size_t f = 12; f < n; f++)
                    for (int sg = 1; sg < atsc::kFieldSegs; sg++) {
                        float row[828];
                        for (int k = 0; k < 828; k++) { const int i = sg * atsc::kSegSyms + 4 + k; row[k] = noisy[f][(size_t)i] - clean[f][(size_t)i]; }
                        fwrite(row, 4, 828, fp);
                    }
                fclose(fp);
            }
            {   // how much does the noise power change from segment to segment? (white Gaussian noise: 0.049)
                std::vector<double> ps; for (size_t f = 12; f < n; f++) for (int sg = 1; sg < atsc::kFieldSegs; sg++) { double p = 0; for (int k = 0; k < 828; k++) { const int i = sg * atsc::kSegSyms + 4 + k; const double e = noisy[f][(size_t)i] - clean[f][(size_t)i]; p += e * e; } ps.push_back(p / 828); }
                double m = 0, q = 0, mx = 0; for (double v : ps) { m += v; mx = std::max(mx, v); } m /= ps.size(); for (double v : ps) q += (v - m) * (v - m);
                printf("  noise power per segment: spread %.3f, largest %.2f times the mean\n", std::sqrt(q / ps.size()) / m, mx / m);
            }
            const double gain = sxy / sxx, var = sum / cnt;
            {   // the same levels through a fresh decoder, outside the receiver
                atsc::FieldDecoder fd; long pk = 0, bad = 0;
                std::vector<uint8_t> out(312 * 188);
                for (size_t f = 0; f < noisy.size(); f++) {
                    atsc::FieldStats st2;
                    const int np = fd.decode(noisy[f].data(), out.data(), &st2);
                    if (f < 14) continue;
                    pk += np; for (int k = 0; k < np; k++) if (out[k * 188 + 1] & 0x80) bad++;
                }
                printf("  the levels through a fresh decoder: %ld bad of %ld packets\n", bad, pk);
                // and put back to the nominal gain first
                double sxy2 = 0, sxx2 = 0;
                for (size_t f = 12; f < n; f++) for (int i = atsc::kSegSyms; i < atsc::kFieldSyms; i++) { if (i % atsc::kSegSyms < 4) continue; sxy2 += noisy[f][(size_t)i] * clean[f][(size_t)i]; sxx2 += clean[f][(size_t)i] * clean[f][(size_t)i]; }
                const double g2 = sxy2 / sxx2;
                atsc::FieldDecoder fd2; long pk2 = 0, bad2 = 0;
                for (size_t f = 0; f < noisy.size(); f++) {
                    std::vector<float> v(noisy[f]); for (auto& x : v) x = (float)(x / g2);
                    atsc::FieldStats st3;
                    const int np = fd2.decode(v.data(), out.data(), &st3);
                    if (f < 14) continue;
                    pk2 += np; for (int k = 0; k < np; k++) if (out[k * 188 + 1] & 0x80) bad2++;
                }
                printf("  the same, put back to the nominal gain (/%.4f): %ld bad of %ld packets\n", g2, bad2, pk2);
            }
            printf("injected %.1f dB: noise %.3f (SNR %.2f dB), gain %.4f, mean shift %.3f, kurtosis %.2f, lag-1 correlation %.3f\n", snr, var, 10 * std::log10(21.0 / var), gain, my - mx, (k4 / cnt) / (var * var), (c1 / cnt) / var);
        }
        return 0;
    }
    const double from = argc > 1 ? atof(argv[1]) : 14.0, to = argc > 2 ? atof(argv[2]) : 17.0, step = argc > 3 ? atof(argv[3]) : 0.5;
    const int fields = argc > 4 ? atoi(argv[4]) : 40;
    printf("%6s | %-34s | %-34s\n", "SNR dB", "ideal (decoder only)", "whole receiver");
    for (double snr = from; snr <= to + 1e-9; snr += step) {
        // ---- ideal
        long idealPk = 0, idealBad = 0;
        {
            atsc::FieldEncoder enc;
            atsc::FieldDecoder dec;
            auto src = dvbt::testTsSource();
            std::mt19937 rng(7);
            const double sigma = std::sqrt(21.0 / std::pow(10.0, snr / 10.0));
            std::normal_distribution<float> nd(0.f, (float)sigma);
            std::vector<uint8_t> ts(312 * 188), syms((size_t)atsc::kFieldSyms), out(312 * 188);
            std::vector<float> lv((size_t)atsc::kFieldSyms);
            for (int f = 0; f < fields + 4; f++) {
                for (int p = 0; p < 312; p++) src(&ts[p * 188]);
                enc.encode(ts.data(), f & 1, syms.data());
                for (size_t i = 0; i < syms.size(); i++) lv[i] = atsc::levelOf(syms[i]) + nd(rng);
                atsc::FieldStats st;
                const int n = dec.decode(lv.data(), out.data(), &st);
                if (f < 4) continue;   // the de-interleaver fills
                idealPk += n;
                for (int k = 0; k < n; k++) if (out[k * 188 + 1] & 0x80) idealBad++;
            }
        }
        // ---- chain
        long chainPk = 0, chainBad = 0;
        double telSnr = 0, telData = 0;
        {
            atsc::ChannelConfig cc; cc.snrDb = snr;
            const double rate = 10e6;
            atsc::Generator gen(dvbt::testTsSource(), cc, rate, 3);
            AtscReceiver rx;
            rx.configure(rate);
            rx.setBlocking(true);
            long seen = 0;   // the first fields are the receiver finding its way: they do not count
            rx.setPacketCallback([&](const uint8_t* p, size_t n, double) { for (size_t k = 0; k < n; k++, seen++) { if (seen < 312 * 14) continue; chainPk++; if (p[k * 188 + 1] & 0x80) chainBad++; } });
            std::vector<cf32> blk;
            const size_t total = (size_t)(rate * (fields + 14) * atsc::kFieldSyms / atsc::kSymbolRate);
            for (size_t done = 0; done < total + (size_t)rate / 2; done += blk.size()) { blk.clear(); gen.generate(1 << 15, blk); rx.feed(blk.data(), blk.size()); }
            rx.flush();
            AtscTelemetry t; uint64_t seq = 0; rx.telemetry(t, seq); telSnr = t.snrDb; telData = t.dataSnrDb;
        }
        printf("%6.2f | %6ld packets, %6ld bad (%8.5f%%) | %6ld packets, %6ld bad (%8.5f%%)  reported SNR %.1f data %.1f\n", snr, idealPk, idealBad, idealPk ? 100.0 * idealBad / idealPk : 0.0, chainPk, chainBad, chainPk ? 100.0 * chainBad / chainPk : 0.0, telSnr, telData);
        fflush(stdout);
    }
}
