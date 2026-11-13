// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench: synthetic signals with known bits through the demodulators (FSK, ASK, AFSK, PSK, MSK, GMSK) and known tones through the audio ones.
#include "dect2/wb/wb_demod.h"
#include "dect2/wb/wb_gen.h"
#include "wb_test.h"
using namespace dect2::wb;
using dect2::cf32;

static double goertzel(const std::vector<float>& a, double f, double rate) {
    double c = 0, s = 0;
    for (size_t i = 0; i < a.size(); i++) { c += a[i] * std::cos(2 * M_PI * f * (double)i / rate); s += a[i] * std::sin(2 * M_PI * f * (double)i / rate); }
    return (c * c + s * s) / ((double)a.size() * (double)a.size()) * 4;   // amplitude^2
}

struct Case { const char* name; GenSpec g; DemodParams p; };

static void round(const Case& c, size_t maxErr = 0) {
    GenResult r = generate(c.g);
    DemodResult d = demodulateBaseband(r.x.data(), r.x.size(), r.rate, c.p);
    if (!d.ok) { CHECK(false, "%s: demod failed: %s", c.name, d.msg.c_str()); return; }
    size_t len = 0; int sh = 0; bool inv = false;
    // PSK without differential coding has a phase ambiguity: the inverted stream counts for BPSK only; QPSK / 8PSK are tested differentially
    const size_t err = bitErrors(r.bits, d.bits, 48, 32, &len, &sh, &inv);
    printf("  %-28s %zu bits, %zu errors (shift %d%s), snr %.1f dB, baud %.2f, cfo %.1f Hz, bits/sym %d\n", c.name, len, err, sh, inv ? ", inverted" : "", d.snrDb, d.baud, d.cfoHz, d.bitsPerSymbol);
    CHECK(len > 300 && err <= maxErr, "%s: %zu errors in %zu bits", c.name, err, len);
}

int main() {
    {
        GenSpec g; g.kind = KFsk2; g.rate = 19200; g.baud = 1200; g.nbits = 1500; g.snrDb = 25; g.offsetHz = 300;
        DemodParams p; p.kind = KFsk2; p.baud = 1200;
        round({"2-FSK 1200 Bd +300 Hz", g, p});
        g.kind = KMsk; g.baud = 2400; g.rate = 38400; g.offsetHz = -150; p.kind = KMsk; p.baud = 2400;
        round({"MSK 2400 Bd", g, p});
        g.kind = KGmsk; g.nbits = 1500; p.kind = KGmsk;
        round({"GMSK 2400 Bd BT 0.5", g, p});
        g.kind = KFsk4; g.rate = 38400; g.baud = 2400; g.snrDb = 30; g.offsetHz = 200; p.kind = KFsk4; p.baud = 2400;
        round({"4-FSK 2400 Bd", g, p});
        g.kind = KFsk4; g.baud = 4800; g.rate = 76800; p.baud = 4800;
        round({"4-FSK 4800 Bd (P25-like)", g, p});
    }
    {
        GenSpec g; g.kind = KAsk; g.rate = 16000; g.baud = 1000; g.nbits = 1200; g.snrDb = 25;
        DemodParams p; p.kind = KAsk; p.baud = 1000;
        round({"OOK 1000 Bd", g, p});
    }
    {   // AFSK: Bell 202 audio (real), and the same on an FM carrier
        GenSpec g; g.kind = KAfsk; g.rate = 48000; g.baud = 1200; g.nbits = 1500; g.snrDb = 30; g.markHz = 1200; g.spaceHz = 2200;
        DemodParams p; p.kind = KAfsk; p.baud = 1200; p.markHz = 1200; p.spaceHz = 2200;
        round({"AFSK 1200 Bd Bell 202 audio", g, p});
        g.baud = 300; g.markHz = 1270; g.spaceHz = 1070; p.baud = 300; p.markHz = 1270; p.spaceHz = 1070;
        round({"AFSK 300 Bd Bell 103", g, p});
        g.baud = 1200; g.markHz = 1200; g.spaceHz = 2200; p.baud = 1200; p.markHz = 1200; p.spaceHz = 2200;
        g.afskOnFm = true; g.rate = 96000; g.snrDb = 30; p.fmFirst = true;
        round({"AFSK 1200 Bd on an FM carrier", g, p});
    }
    {   // PSK with a carrier offset and differential coding
        GenSpec g; g.kind = KBpsk; g.rate = 38400; g.baud = 2400; g.nbits = 3000; g.snrDb = 30; g.offsetHz = 90; g.differential = true;
        DemodParams p; p.kind = KBpsk; p.baud = 2400; p.differential = true;
        round({"DBPSK 2400 Bd +90 Hz", g, p});
        g.kind = KQpsk; g.nbits = 6000; p.kind = KQpsk;
        round({"DQPSK 2400 Bd +90 Hz", g, p});
        g.kind = KPsk8; g.nbits = 9000; g.snrDb = 36; p.kind = KPsk8;
        round({"D8PSK 2400 Bd +90 Hz", g, p});
        // BPSK without differential coding: the recovered stream is right up to an inversion
        g.kind = KBpsk; g.nbits = 3000; g.snrDb = 30; g.differential = false; p.kind = KBpsk; p.differential = false;
        round({"BPSK 2400 Bd (sign free)", g, p});
        // a different roll-off and the boxcar filter
        g.kind = KQpsk; g.nbits = 6000; g.differential = true; g.rolloff = 0.5; p.kind = KQpsk; p.differential = true; p.rolloff = 0.5;
        round({"DQPSK roll-off 0.5", g, p});
    }
    {   // the baud rate a little off, and a sample rate that is not a multiple of the baud rate: the timing loop follows
        GenSpec g; g.kind = KFsk2; g.rate = 20000; g.baud = 1203; g.nbits = 2500; g.snrDb = 25;
        DemodParams p; p.kind = KFsk2; p.baud = 1200;
        round({"2-FSK baud 0.25 % off, sps 16.6", g, p});
        GenSpec q; q.kind = KQpsk; q.rate = 36000; q.baud = 2400; q.nbits = 6000; q.snrDb = 30; q.differential = true; q.offsetHz = -60;
        DemodParams pq; pq.kind = KQpsk; pq.baud = 2395; pq.differential = true;
        round({"DQPSK baud 0.2 % off, sps 15", q, pq});
    }
    {   // noise: the symbols carry on, only SNR drops; errors are allowed but the demodulator must not fail
        GenSpec g; g.kind = KFsk2; g.rate = 19200; g.baud = 1200; g.nbits = 2000; g.snrDb = 12;
        DemodParams p; p.kind = KFsk2; p.baud = 1200;
        GenResult r = generate(g);
        DemodResult d = demodulateBaseband(r.x.data(), r.x.size(), r.rate, p);
        size_t len = 0;
        const size_t err = bitErrors(r.bits, d.bits, 48, 32, &len);
        printf("  2-FSK at 12 dB: %zu errors in %zu bits\n", err, len);
        CHECK(d.ok && err < len / 20, "noisy FSK: %zu / %zu", err, len);
    }
    {   // audio demodulators: a 1 kHz tone comes out at 1 kHz
        const double rate = 96000;
        const size_t N = (size_t)rate;   // one second
        // build the signals directly
        std::vector<cf32> am(N), fm(N), usb(N), lsb(N);
        double ph = 0;
        for (size_t i = 0; i < N; i++) {
            const double t = (double)i / rate;
            am[i] = cf32((float)(0.5 * (1 + 0.8 * std::cos(2 * M_PI * 1000 * t))), 0);
            fm[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
            ph += 2 * M_PI * 3000 * std::cos(2 * M_PI * 1000 * t) / rate;
            const double B = 3000;
            usb[i] = cf32((float)std::cos(2 * M_PI * (-B / 2 + 1000) * t), (float)std::sin(2 * M_PI * (-B / 2 + 1000) * t));
            lsb[i] = cf32((float)std::cos(2 * M_PI * (B / 2 - 1000) * t), (float)std::sin(2 * M_PI * (B / 2 - 1000) * t));
        }
        DemodParams p;
        struct A { const char* n; int kind; std::vector<cf32>* x; };
        for (A a : {A{"AM", KAm, &am}, A{"FM", KFm, &fm}, A{"USB", KUsb, &usb}, A{"LSB", KLsb, &lsb}}) {
            p.kind = a.kind; p.bwHz = 3000;
            DemodResult d = demodulateBaseband(a.x->data(), a.x->size(), rate, p);
            CHECK(d.ok && d.audio.size() > 40000, "%s: audio %zu", a.n, d.audio.size());
            if (!d.ok) continue;
            double total = 0;
            for (float v : d.audio) total += (double)v * v;
            total = total / (double)d.audio.size() * 2;   // amplitude^2 of a sine with that power
            const double at = goertzel(d.audio, 1000, 48000);
            printf("  %-4s tone share at 1 kHz: %.3f\n", a.n, at / std::max(total, 1e-12));
            CHECK(at > 0.8 * total, "%s: the 1 kHz tone holds %.2f of the audio", a.n, at / std::max(total, 1e-12));
        }
    }
    {   // from a recording: the band is extracted first (a real audio file, an IQ file with the signal off to the side and a second signal beside it)
        GenSpec g; g.kind = KAfsk; g.rate = 48000; g.baud = 1200; g.nbits = 1500; g.snrDb = 30; g.leadSec = 0.05; g.tailSec = 0.05;
        GenResult r = generate(g);
        auto src = Source::fromSamples(r.x, r.rate, true, 0, "afsk");
        DemodParams p; p.kind = KAfsk; p.baud = 1200; p.markHz = 1200; p.spaceHz = 2200;
        DemodResult d = demodulate(*src, r.signalStart, r.signalEnd, p);   // the whole band
        size_t len = 0;
        size_t err = d.ok ? bitErrors(r.bits, d.bits, 48, 32, &len) : 99999;
        printf("  recording, AFSK whole audio band: %zu errors in %zu bits\n", err, len);
        CHECK(d.ok && err == 0, "afsk from a recording: %s", d.msg.c_str());
        p.centreHz = 1700; p.bwHz = 2200;   // the band that holds both tones
        d = demodulate(*src, r.signalStart, r.signalEnd, p);
        err = d.ok ? bitErrors(r.bits, d.bits, 48, 32, &len) : 99999;
        printf("  recording, AFSK band 600 .. 2800 Hz: %zu errors in %zu bits\n", err, len);
        CHECK(d.ok && err == 0, "afsk in a band: %s", d.msg.c_str());
        // 2-FSK at +40 kHz beside a QPSK signal at -55 kHz; the band picks one
        GenSpec a; a.kind = KFsk2; a.rate = 250000; a.baud = 9600; a.nbits = 3000; a.snrDb = 25; a.offsetHz = 40000; a.seed = 4;
        GenSpec b; b.kind = KQpsk; b.rate = 250000; b.baud = 12500; b.nbits = 6000; b.snrDb = 200; b.offsetHz = -55000; b.differential = true; b.seed = 5;
        GenResult ra = generate(a), rb = generate(b);
        std::vector<cf32> mix(std::max(ra.x.size(), rb.x.size()), cf32(0, 0));
        for (size_t i = 0; i < ra.x.size(); i++) mix[i] += ra.x[i];
        for (size_t i = 0; i < rb.x.size(); i++) mix[i] += 0.5f * rb.x[i];
        auto ms = Source::fromSamples(mix, 250000, false, 0, "two signals");
        DemodParams pf; pf.kind = KFsk2; pf.baud = 9600; pf.centreHz = 40000; pf.bwHz = 30000;
        d = demodulate(*ms, 0, ra.x.size(), pf);
        err = d.ok ? bitErrors(ra.bits, d.bits, 48, 32, &len) : 99999;
        printf("  recording, 2-FSK beside a QPSK signal: %zu errors in %zu bits\n", err, len);
        CHECK(d.ok && err == 0, "fsk in a mix: %s", d.msg.c_str());
        DemodParams pq; pq.kind = KQpsk; pq.baud = 12500; pq.centreHz = -55000; pq.bwHz = 25000; pq.differential = true;
        d = demodulate(*ms, 0, rb.x.size(), pq);
        err = d.ok ? bitErrors(rb.bits, d.bits, 48, 32, &len) : 99999;
        printf("  recording, QPSK beside a 2-FSK signal: %zu errors in %zu bits, snr %.1f dB\n", err, len, d.snrDb);
        CHECK(d.ok && err == 0, "qpsk in a mix: %s", d.msg.c_str());
    }
    {   // plots and eye: filled in
        GenSpec g; g.kind = KFsk2; g.rate = 19200; g.baud = 1200; g.nbits = 800; g.snrDb = 30;
        DemodParams p; p.kind = KFsk2; p.baud = 1200;
        GenResult r = generate(g);
        DemodResult d = demodulateBaseband(r.x.data(), r.x.size(), r.rate, p);
        CHECK(d.ok && !d.instFreq.empty() && d.instFreq.size() == d.instPhase.size() && d.eyeTraces > 20 && d.eyeLen == 33 && d.eye.size() == (size_t)d.eyeTraces * d.eyeLen && d.soft.size() == d.symbols,
              "plots: %zu %d %d", d.instFreq.size(), d.eyeTraces, d.eyeLen);
        CHECK(std::fabs(d.devHz - 0.6 * 1200) < 0.15 * 0.6 * 1200, "deviation %.1f Hz", d.devHz);
    }
    printf(fails ? "FAILED\n" : "wb_demod ok\n");
    return fails ? 1 : 0;
}
