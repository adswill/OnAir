// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// e-Czas streaming receiver: the test signal (eczas_gen.h) through EczasLiveReceiver with the impairments of a real long-wave channel: carrier offset,
// noise, level, fading, sample clock error, input rates from an 8 kHz audio recording to 2 Msps IQ; the time stamp and the computer clock offset
// (the receiver's clock origin is known here); noise alone and a bare carrier give nothing; every field of the packet; the calendar edges of the
// 30-bit counter; no allocation per sample.
#include "dect2/eczas/eczas_gen.h"
#include "dect2/eczas/eczas_live.h"
#include "dect2/eczas/eczas_rx.h"
#include "dect2/eczas/eczas_tel.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
#include "../common/test_util.h"

using namespace dect2;

static std::atomic<unsigned long long> gAllocs{0};
void* operator new(size_t n) { gAllocs++; if (void* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static int64_t unixOf(int y, int m, int d, int h, int mi, int s) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + h * 3600 + mi * 60 + s;
}

constexpr double kWarsawLat = 52.23, kWarsawLon = 21.01;

struct Result {
    EczasTelemetry t;
    int ok = 0, bad = 0;
    double biasMs = 0;      // mean of (frame start measured - frame start true) over the time packets
    double maxDevMs = 0;
    int wrongStamp = 0;
};

// secs of signal through the receiver in blocks; `audio`: real samples (the carrier is the tone at dial offset), else IQ
static Result run(EczasGenConfig gc, double secs, bool audio = false, bool mode2 = true) {
    Result r;
    gc.startUtc = (double)unixOf(2026, 8, 13, 17, 0, 0);
    EczasGenerator g(gc);
    EczasLiveReceiver rx;
    if (audio) rx.setAudioInput(true, gc.dialOffsetHz); else rx.setSignalOffset(gc.dialOffsetHz);
    rx.configure(gc.rate);
    if (mode2) rx.setClockMode(2, gc.startUtc);
    rx.setReceiverLocation(kWarsawLat, kWarsawLon, true);
    std::vector<cf32> buf(8192);
    const size_t total = (size_t)(secs * gc.rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        const size_t n = std::min(buf.size(), total - done);
        g.generate(buf.data(), n);
        rx.feed(buf.data(), n);
    }
    rx.telemetry(r.t, 0);
    double sum = 0;
    int cnt = 0;
    for (const auto& l : r.t.log) {
        if (l.kind == 0) {
            r.ok++;
            // the true start of a frame is the start of its slot: the stamp in the text is checked through the log's stream time
            int y, mo, d, h, mi, s;
            if (sscanf(l.text.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
                const double T = (double)unixOf(y, mo, d, h, mi, s);
                if (std::fmod(T, 3.0) != 0) r.wrongStamp++;
                const double trueStart = (T - gc.startUtc) + gc.delayMs * 1e-3;   // when it arrives in the samples
                const double dev = (l.streamSec - trueStart) * 1000.0 * (gc.clockPpm != 0 ? 1.0 : 1.0);
                sum += dev; cnt++;
                r.maxDevMs = std::max(r.maxDevMs, std::fabs(dev));
            }
        } else if (l.kind == 1) r.bad++;
    }
    r.biasMs = cnt ? sum / cnt : 0;
    return r;
}

int main() {
    // ---- the frame layer: every field, the edges of the counter
    {
        const int64_t edges[] = {unixOf(2000, 1, 1, 0, 0, 0), unixOf(2024, 2, 29, 23, 59, 57), unixOf(2026, 12, 31, 23, 59, 57), unixOf(2027, 1, 1, 0, 0, 0),
                                 unixOf(2028, 2, 29, 12, 0, 0), unixOf(2038, 1, 19, 3, 14, 6), unixOf(2100, 12, 31, 23, 59, 57), eczas::kEpochUtc + 3LL * 0x3FFFFFFF};
        for (int64_t u : edges) {
            const uint32_t per = (uint32_t)((u - eczas::kEpochUtc) / 3);
            for (int tz = 0; tz < 4; tz++)
                for (int fl = 0; fl < 8; fl++)
                    for (int sk = 0; sk < 4; sk++) {
                        uint8_t f[eczas::kFrameBytes];
                        eczas::encodeTimeFrameForTest(per, tz, (uint8_t)fl, (uint8_t)sk, f);
                        eczas::TimePacket p;
                        const bool ok = eczas::decodeTimeFrame(f, p);
                        CHECK(ok && p.utcSec == eczas::kEpochUtc + (int64_t)per * 3 && p.tzHours == tz && p.localSec == p.utcSec + 3600 * tz && p.txState == sk &&
                              p.leapAnnounce == !!(fl & 4) && p.leapSubtract == !!(fl & 2) && p.dstChangeSoon == !!(fl & 1), "fields tz %d flags %d sk %d at %lld", tz, fl, sk, (long long)u);
                    }
        }
        // the largest counter value the 30 bits hold
        uint8_t f[eczas::kFrameBytes];
        eczas::encodeTimeFrameForTest(0x3FFFFFFFu, 1, 0, 0, f);
        eczas::TimePacket p;
        CHECK(eczas::decodeTimeFrame(f, p) && p.periods3s == 0x3FFFFFFFu, "30-bit counter");
        int y, mo, d, h, mi, s;
        eczas::civilFromUnix(unixOf(2028, 2, 29, 23, 59, 59), y, mo, d, h, mi, s);
        CHECK(y == 2028 && mo == 2 && d == 29 && h == 23 && mi == 59 && s == 59, "calendar");
        // Poland's summer time: 2026-03-29 01:00 UTC to 2026-10-25 01:00 UTC
        CHECK(polandOffsetHours(unixOf(2026, 3, 29, 0, 59, 59)) == 1 && polandOffsetHours(unixOf(2026, 3, 29, 1, 0, 0)) == 2 &&
              polandOffsetHours(unixOf(2026, 10, 25, 0, 59, 59)) == 2 && polandOffsetHours(unixOf(2026, 10, 25, 1, 0, 0)) == 1, "Polish DST");
        // single bit errors in the data are caught by the check byte
        eczas::encodeTimeFrameForTest(1000, 2, 0, 0, f);
        int caught = 0;
        for (int b = 24; b < 96; b++) {
            uint8_t g2[eczas::kFrameBytes];
            std::memcpy(g2, f, sizeof g2);
            g2[b / 8] ^= (uint8_t)(1 << (7 - b % 8));
            if (b >= 64 && b < 88) continue;   // the Reed-Solomon bytes are not covered by the check byte
            eczas::TimePacket q;
            if (!eczas::decodeTimeFrame(g2, q)) caught++;
        }
        CHECK(caught == 48, "single bit errors caught: %d of 48", caught);
    }

    // ---- the test signal at the engine's rate and offset, clean
    {
        EczasGenConfig gc;
        gc.rate = 256000; gc.dialOffsetHz = -12000; gc.snrDb = 99; gc.delayMs = 0.63;
        const Result r = run(gc, 60);
        printf("  clean 256 ksps: %d time packets, %d bad, bias %+.1f ms, max %.1f ms, SNR %.1f dB, offset %+.1f ms (avg %+.1f), conf %d\n", r.ok, r.bad, r.biasMs, r.maxDevMs,
               r.t.snrDb, r.t.clockOffsetMs, r.t.clockOffsetAvgMs, (int)r.t.confirmed);
        CHECK(r.ok >= 4 && r.bad == 0 && r.wrongStamp == 0, "clean: %d ok %d bad %d wrong stamps", r.ok, r.bad, r.wrongStamp);
        CHECK(r.t.state == 2 && r.t.dataValid && r.t.confirmed, "state %d", r.t.state);
        CHECK(r.t.pkt.tzHours == 2 && r.t.pkt.timeMarker == eczas::kTimeMarker, "tz %d", r.t.pkt.tzHours);
        CHECK(std::fabs(r.biasMs) < 12 && r.maxDevMs < 25, "frame timing bias %+.1f ms, max %.1f ms", r.biasMs, r.maxDevMs);
        CHECK(r.t.clockValid && std::fabs(r.t.clockOffsetAvgMs) < 25, "computer clock offset %+.1f ms", r.t.clockOffsetAvgMs);
        CHECK(r.t.snrDb > 25, "SNR %.1f", r.t.snrDb);
        CHECK(r.t.otherType >= 5, "other slots %llu", (unsigned long long)r.t.otherType);
    }

    // ---- carrier offset (radio and crystal errors)
    for (double cfo : {-150.0, -80.0, -30.0, 0.0, 12.0, 45.0, 90.0, 140.0}) {
        EczasGenConfig gc;
        gc.rate = 256000; gc.dialOffsetHz = -12000; gc.cfoHz = cfo; gc.snrDb = 30;
        const Result r = run(gc, 60);
        printf("  carrier offset %+.0f Hz: %d ok, %d bad, tracked %+.1f Hz\n", cfo, r.ok, r.bad, r.t.cfoHz);
        CHECK(r.ok >= 4 && r.wrongStamp == 0, "offset %.0f Hz: %d packets", cfo, r.ok);
        CHECK(std::fabs(r.t.cfoHz - cfo) < 3, "offset %.0f Hz tracked as %.1f", cfo, r.t.cfoHz);
    }

    // ---- noise (carrier to noise in 100 Hz)
    for (double snr : {30.0, 22.0, 17.0}) {
        EczasGenConfig gc;
        gc.rate = 256000; gc.dialOffsetHz = -12000; gc.snrDb = snr;
        const Result r = run(gc, 120);
        printf("  carrier/noise %.0f dB: %d ok, %d bad, measured %.1f dB\n", snr, r.ok, r.bad, r.t.snrDb);
        CHECK(r.wrongStamp == 0, "SNR %.0f: a wrong stamp got through", snr);
        if (snr >= 22) CHECK(r.ok >= 8, "SNR %.0f dB: %d packets", snr, r.ok);   // the recordings (about 18 dB) decode one time packet in four; 17 dB is the edge
        CHECK(std::fabs(r.t.snrDb - snr) < 3.0, "SNR %.0f dB measured %.1f", snr, r.t.snrDb);
    }

    // ---- level (the AGC is the phase, not the amplitude) and fading
    for (double amp : {0.003, 0.9}) {
        EczasGenConfig gc;
        gc.rate = 256000; gc.dialOffsetHz = -12000; gc.snrDb = 40; gc.amp = amp;
        const Result r = run(gc, 60);
        CHECK(r.ok >= 4 && r.wrongStamp == 0, "amplitude %.3f: %d packets", amp, r.ok);
        CHECK(std::fabs(r.t.levelDb - 20 * std::log10(amp)) < 2, "level %.1f dBFS for amplitude %.3f", r.t.levelDb, amp);
    }
    {
        EczasGenConfig gc;
        gc.rate = 256000; gc.dialOffsetHz = -12000; gc.snrDb = 30; gc.amDepth = 0.6;
        const Result r = run(gc, 60);
        CHECK(r.ok >= 3 && r.wrongStamp == 0, "fading: %d packets", r.ok);
    }

    // ---- input rates: an 8 kHz and a 48 kHz audio recording with the carrier at 1 kHz, 48 / 250 ksps / 2 Msps IQ, a sample clock off by 60 ppm
    {
        struct C { double rate; bool audio; double off; double ppm; const char* name; } cs[] = {
            {8000, true, 1000, 0, "8 kHz audio"}, {48000, true, 1000, 0, "48 kHz audio"}, {48000, false, -12000, 0, "48 ksps IQ"}, {250000, false, -12000, 60, "250 ksps IQ, +60 ppm"},
            {2000000, false, -12000, -60, "2 Msps IQ, -60 ppm"}};
        for (const C& c : cs) {
            EczasGenConfig gc;
            gc.rate = c.rate; gc.dialOffsetHz = c.off; gc.snrDb = 30; gc.clockPpm = c.ppm;
            const Result r = run(gc, c.rate > 1e6 ? 40 : 60, c.audio);
            printf("  %s: %d ok, %d bad\n", c.name, r.ok, r.bad);
            if (c.rate >= 48000 || c.audio) CHECK(r.ok >= 3 && r.wrongStamp == 0, "%s: %d packets", c.name, r.ok);
        }
    }

    // ---- nothing: noise alone, a bare carrier, a keyed carrier under strong noise
    {
        EczasGenConfig gc;
        gc.rate = 256000; gc.dialOffsetHz = -12000; gc.snrDb = -10; gc.carrierOnly = true;
        Result r = run(gc, 120);
        CHECK(r.ok == 0 && r.t.state == 0, "noise: %d packets, state %d", r.ok, r.t.state);
        gc.snrDb = 30; gc.carrierOnly = true;
        r = run(gc, 60);
        CHECK(r.ok == 0 && r.t.state == 1 && r.t.snrDb > 20, "bare carrier: %d packets, state %d, SNR %.1f", r.ok, r.t.state, r.t.snrDb);
        gc.snrDb = 5; gc.carrierOnly = false;
        r = run(gc, 120);
        CHECK(r.wrongStamp == 0, "5 dB: a wrong stamp got through (%d)", r.wrongStamp);
    }

    // ---- every flag and offset through the whole path
    for (int tz = 0; tz < 4; tz++)
        for (int fl : {0, 7, 5, 2}) {
            EczasGenConfig gc;
            gc.rate = 48000; gc.dialOffsetHz = -12000; gc.snrDb = 40; gc.tzHours = tz; gc.flags = (uint8_t)fl; gc.txState = (uint8_t)((tz + fl) & 3); gc.timeSlotStride = 1; gc.otherSlots = false;
            const Result r = run(gc, 20);
            const auto& p = r.t.pkt;
            CHECK(r.ok >= 3 && p.tzHours == tz && p.leapAnnounce == !!(fl & 4) && p.leapSubtract == !!(fl & 2) && p.dstChangeSoon == !!(fl & 1) && p.txState == ((tz + fl) & 3),
                  "tz %d flags %d: %d packets, tz %d ls %d lss %d tzc %d sk %d", tz, fl, r.ok, p.tzHours, p.leapAnnounce, p.leapSubtract, p.dstChangeSoon, p.txState);
        }

    // ---- no allocation per sample (the 4 Hz reports allocate)
    {
        EczasGenConfig gc;
        gc.rate = 2000000; gc.dialOffsetHz = -12000; gc.snrDb = 30;
        EczasGenerator g(gc);
        EczasLiveReceiver rx;
        rx.setSignalOffset(-12000);
        rx.configure(gc.rate);
        std::vector<cf32> buf(65536);
        for (int i = 0; i < 4; i++) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
        const unsigned long long a0 = gAllocs.load();
        const int blocks = 60;
        for (int i = 0; i < blocks; i++) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
        const unsigned long long a1 = gAllocs.load();
        const double perMsample = (double)(a1 - a0) / ((double)blocks * (double)buf.size() / 1e6);
        printf("  allocations: %.1f per million samples\n", perMsample);
        CHECK(perMsample < 400, "%.0f allocations per million samples", perMsample);
    }
    return testSummary("eczas live");
}
