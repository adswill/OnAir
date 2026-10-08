// DAB receiver against the test transmitter (dab_gen): the real DabReceiver locks to the generated ensemble, reads the labels and the sub-channel table exactly,
// and decodes the audio of every service to the right tones (DAB+ AAC-LC at 48 and 32 kHz, a DAB+ melody, DAB MP2). Then the impairments: a C/N sweep, carrier
// offsets, clock offsets, the sample rates of the radios and chunk sizes from 1 to 65536 samples. The signal is rounded to 8 bits like the source does.
#include "dect2/dab.h"
#include "dect2/dab_gen.h"
#include "jobs.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
using namespace dect2;
using testjobs::jprintf;

static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL line %d: ", __LINE__); jprintf(__VA_ARGS__); jprintf("\n"); fails++; } } while (0)

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif
#ifndef SANITIZED
#define SANITIZED 0
#endif

static const int64_t kUtc0 = 1700000000;

struct Result {
    DabTelemetry tel;
    DabEnsemble ens;
    DabAudioStats st;
    std::vector<std::vector<uint8_t>> aus, frames;       // access units with a good CRC; logical frames of the selected sub-channel
    uint8_t hdr = 0;
    double seconds = 0;
    uint64_t locked = 0;                                  // frames decoded
};

static dabgen::TxConfig baseConfig() {
    dabgen::TxConfig tc;
    tc.utcSeconds = kUtc0;
    return tc;
}

// runs `seconds` of the signal through the receiver, `chunk` samples at a time
static Result run(const dabgen::TxConfig& tc, SynthConfig sc, double rate, double seconds, size_t chunk, int sel, bool keepAus = true) {
    Result res;
    auto syn = makeDabSynth(tc, sc, rate);
    if (!syn) { CHECK(false, "no generator at %.3f Msps", rate / 1e6); return res; }
    DabReceiver rx;
    rx.configure(rate);
    rx.audio().setSilent(true);
    if (keepAus) {
        rx.audio().setAuTap([&](const uint8_t* au, int n, uint8_t h) { res.aus.emplace_back(au, au + n); res.hdr = h; });
        rx.setFrameTap([&](int, const uint8_t* b, int n) { res.frames.emplace_back(b, b + n); });
    }
    rx.select(sel);
    const size_t total = (size_t)(seconds * rate);
    std::vector<cf32> x(chunk);
    size_t done = 0;
    while (done < total) {
        const size_t n = std::min(chunk, total - done);
        syn->generate(x.data(), n);
        for (size_t i = 0; i < n; i++) x[i] = cf32(std::round(x[i].real() * 127.f) / 127.f, std::round(x[i].imag() * 127.f) / 127.f);   // 8 bit like the source
        rx.feed(x.data(), n);
        done += n;
    }
    rx.telemetry(res.tel, 0);
    res.ens = rx.ensemble();
    res.st = rx.audio().stats();
    res.seconds = seconds;
    return res;
}

// ---------------------------------------------------------------- decoding with libavcodec, independent of the receiver
struct Pcm { std::vector<float> ch[2]; int rate = 0; };
static bool decodePackets(AVCodecID id, const std::vector<uint8_t>& asc, const std::vector<std::vector<uint8_t>>& pk, Pcm& out) {
    const AVCodec* codec = avcodec_find_decoder(id);
    if (!codec) return false;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!asc.empty()) {
        ctx->extradata = (uint8_t*)av_mallocz(asc.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(ctx->extradata, asc.data(), asc.size());
        ctx->extradata_size = (int)asc.size();
    }
    if (avcodec_open2(ctx, codec, nullptr) < 0) { avcodec_free_context(&ctx); return false; }
    AVPacket* p = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    for (const auto& a : pk) {
        av_new_packet(p, (int)a.size());
        memcpy(p->data, a.data(), a.size());
        if (avcodec_send_packet(ctx, p) < 0) { av_packet_unref(p); continue; }
        av_packet_unref(p);
        while (avcodec_receive_frame(ctx, f) == 0) {
            out.rate = f->sample_rate;
            for (int c = 0; c < std::min(2, f->ch_layout.nb_channels); c++)
                for (int i = 0; i < f->nb_samples; i++) {
                    float v;
                    switch (f->format) {
                    case AV_SAMPLE_FMT_FLTP: v = ((const float*)f->extended_data[c])[i]; break;
                    case AV_SAMPLE_FMT_S16P: v = ((const int16_t*)f->extended_data[c])[i] / 32768.f; break;
                    case AV_SAMPLE_FMT_S16: v = ((const int16_t*)f->extended_data[0])[i * f->ch_layout.nb_channels + c] / 32768.f; break;
                    case AV_SAMPLE_FMT_FLT: v = ((const float*)f->extended_data[0])[i * f->ch_layout.nb_channels + c]; break;
                    default: v = 0;
                    }
                    out.ch[c].push_back(v);
                }
            av_frame_unref(f);
        }
    }
    av_packet_free(&p); av_frame_free(&f); avcodec_free_context(&ctx);
    return true;
}

static std::vector<uint8_t> aacAsc(int rate) {
    const uint32_t asc = (2u << 11) | ((uint32_t)(rate == 48000 ? 3 : 5) << 7) | (2u << 3) | (1u << 2);      // AAC-LC, rate, stereo, 960 frames
    return {(uint8_t)(asc >> 8), (uint8_t)asc};
}

static void measureTone(const std::vector<float>& x, size_t from, size_t n, double fs, double lo, double hi, double& hz, double& amp) {
    std::vector<double> w(n);
    double ws = 0;
    for (size_t i = 0; i < n; i++) { w[i] = 0.5 - 0.5 * std::cos(2 * M_PI * (double)i / (double)n); ws += w[i]; }
    double best = -1;
    for (double f = lo; f <= hi; f += 0.25) {
        double re = 0, im = 0;
        const double dph = 2 * M_PI * f / fs, sc = std::cos(dph), ss = std::sin(dph);
        double c = 1, s = 0;
        for (size_t i = 0; i < n; i++) {
            const double v = x[from + i] * w[i];
            re += v * c; im -= v * s;
            const double nc = c * sc - s * ss; s = s * sc + c * ss; c = nc;
        }
        const double m = 2 * std::sqrt(re * re + im * im) / ws;
        if (m > best) { best = m; hz = f; }
    }
    amp = best;
}

// ---------------------------------------------------------------- what the ensemble must look like
static void checkEnsemble(const Result& r, const dabgen::TxConfig& tc, const char* what) {
    dabgen::Transmitter tx(tc);                                   // the layout of the generator
    const DabEnsemble& e = r.ens;
    CHECK(r.tel.state == 2, "%s: not locked (state %d)", what, r.tel.state);
    CHECK(e.valid && e.eid == 0xCE15, "%s: ensemble id %04X valid %d", what, e.eid, e.valid);
    CHECK(e.label == "OnAir DAB", "%s: ensemble label \"%s\"", what, e.label.c_str());
    const auto& svc = tx.config().services;
    CHECK(e.services.size() == svc.size(), "%s: %zu services, expected %zu", what, e.services.size(), svc.size());
    for (size_t i = 0; i < svc.size(); i++) {
        auto it = e.services.find(svc[i].sid);
        CHECK(it != e.services.end(), "%s: service %04X missing", what, svc[i].sid);
        if (it == e.services.end()) continue;
        CHECK(it->second.label == svc[i].label, "%s: label of %04X is \"%s\", expected \"%s\"", what, svc[i].sid, it->second.label.c_str(), svc[i].label.c_str());
        const DabComponent* c = it->second.audio();
        CHECK(c && c->subId == svc[i].subId && c->ascty == (svc[i].dabPlus ? 63 : 0) && it->second.dabPlus() == svc[i].dabPlus, "%s: component of %04X", what, svc[i].sid);
        auto sub = e.subs.find(svc[i].subId);
        CHECK(sub != e.subs.end(), "%s: sub-channel %d missing", what, svc[i].subId);
        if (sub == e.subs.end()) continue;
        const auto& l = tx.layout()[i];
        CHECK(sub->second.start == l.start && sub->second.size == l.size && sub->second.bitrate == l.bitrate && sub->second.eep && sub->second.option == l.option && sub->second.level == l.level,
              "%s: sub-channel %d: start %d size %d bitrate %d EEP %d/%d", what, svc[i].subId, sub->second.start, sub->second.size, sub->second.bitrate, sub->second.option, sub->second.level);
    }
    CHECK(e.utc >= kUtc0 && e.utc <= kUtc0 + (int64_t)(r.seconds + 2), "%s: broadcast time %lld, expected about %lld", what, (long long)e.utc, (long long)kUtc0);
}

static double fibRatio(const Result& r) { const double n = (double)(r.tel.fibOk + r.tel.fibBad); return n > 0 ? (double)r.tel.fibOk / n : 0; }

// ---------------------------------------------------------------- tests
static void testAudio() {
    const dabgen::TxConfig tc = baseConfig();
    dabgen::Transmitter tx0(tc);
    SynthConfig sc;
    sc.snrDb = 30;
    // the services are independent: each one runs on its own thread (with a transmitter of its own), the output keeps the order of the services
    testjobs::Jobs jobs;
    for (size_t i = 0; i < tx0.config().services.size(); i++) jobs.add([=] {
        dabgen::Transmitter tx(tc);
        const dabgen::TxService& sv = tx.config().services[i];
        const Result r = run(tc, sc, 2.048e6, 8.0, 65536, sv.subId);
        checkEnsemble(r, tc, sv.label.c_str());
        jprintf("%s (sub %d, %d kbit/s): %llu frames, FIB ok %.1f%%, codec %s %d Hz %d ch, superframes %llu ok %llu bad, AU %llu ok %llu bad, RS %llu, PCM %llu\n", sv.label.c_str(), sv.subId, sv.bitrate,
               (unsigned long long)r.tel.frames, 100 * fibRatio(r), r.st.codec.c_str(), r.st.sampleRate, r.st.channels, (unsigned long long)r.st.superframesOk,
               (unsigned long long)r.st.superframesBad, (unsigned long long)r.st.auOk, (unsigned long long)r.st.auBad, (unsigned long long)r.st.rsCorrected, (unsigned long long)r.st.pcmFrames);
        CHECK(fibRatio(r) > 0.99, "%s: FIB ok %.3f", sv.label.c_str(), fibRatio(r));
        CHECK(r.st.sub == sv.subId && r.st.dabPlus == sv.dabPlus && r.st.bitrate == sv.bitrate, "%s: audio stats sub %d", sv.label.c_str(), r.st.sub);
        CHECK(r.st.decoding && r.st.pcmFrames > 4 * (uint64_t)sv.sampleRate, "%s: decoded %llu sample frames", sv.label.c_str(), (unsigned long long)r.st.pcmFrames);
        Pcm pcm;
        if (sv.dabPlus) {
            CHECK(r.st.codec == "AAC-LC" && r.st.sampleRate == sv.sampleRate && r.st.channels == 2, "%s: codec %s", sv.label.c_str(), r.st.codec.c_str());
            CHECK(r.st.superframesBad == 0 && r.st.auBad == 0 && r.st.rsCorrected == 0 && r.st.superframesOk > 50, "%s: superframes %llu ok %llu bad, AU bad %llu, RS %llu",
                  sv.label.c_str(), (unsigned long long)r.st.superframesOk, (unsigned long long)r.st.superframesBad, (unsigned long long)r.st.auBad, (unsigned long long)r.st.rsCorrected);
            // every access unit that came out is one the transmitter made
            std::set<std::vector<uint8_t>> mine(tx.accessUnits((int)i).begin(), tx.accessUnits((int)i).end());
            int foreign = 0;
            for (const auto& a : r.aus) foreign += !mine.count(a);
            CHECK(foreign == 0 && r.aus.size() > 200, "%s: %zu access units, %d not made by the transmitter", sv.label.c_str(), r.aus.size(), foreign);
            CHECK(((r.hdr >> 6) & 1) == (sv.sampleRate == 48000) && ((r.hdr >> 4) & 1) == 1 && ((r.hdr >> 5) & 1) == 0, "%s: superframe header %02X", sv.label.c_str(), r.hdr);
            CHECK(decodePackets(AV_CODEC_ID_AAC, aacAsc(sv.sampleRate), r.aus, pcm), "decoder");
        } else {
            CHECK(r.st.codec == "MP2" && r.st.sampleRate == 48000 && r.st.channels == 2, "%s: codec %s %d Hz %d ch", sv.label.c_str(), r.st.codec.c_str(), r.st.sampleRate, r.st.channels);
            std::set<std::vector<uint8_t>> mine(tx.mp2Frames((int)i).begin(), tx.mp2Frames((int)i).end());
            int foreign = 0;
            for (const auto& a : r.frames) foreign += !mine.count(a);
            CHECK(foreign == 0 && r.frames.size() > 250, "%s: %zu logical frames, %d not made by the transmitter", sv.label.c_str(), r.frames.size(), foreign);
            CHECK(decodePackets(AV_CODEC_ID_MP2, {}, r.frames, pcm), "decoder");
        }
        const size_t from = (size_t)(0.5 * sv.sampleRate), n = (size_t)(2.0 * sv.sampleRate);
        CHECK(pcm.ch[0].size() > from + n && pcm.ch[1].size() > from + n, "%s: decoded %zu samples", sv.label.c_str(), pcm.ch[0].size());
        if (pcm.ch[0].size() <= from + n) return;
        if (!sv.melody) {
            double hl = 0, al = 0, hr = 0, ar = 0, x = 0, ax = 0;
            measureTone(pcm.ch[0], from, n, sv.sampleRate, sv.leftHz - 100, sv.leftHz + 100, hl, al);
            measureTone(pcm.ch[1], from, n, sv.sampleRate, sv.rightHz - 100, sv.rightHz + 100, hr, ar);
            measureTone(pcm.ch[1], from, n, sv.sampleRate, sv.leftHz - 20, sv.leftHz + 20, x, ax);      // left tone in the right channel
            jprintf("   left %.2f Hz %.4f, right %.2f Hz %.4f (wanted %.0f / %.0f Hz at %.2f), left in right %.1f dB\n", hl, al, hr, ar, sv.leftHz, sv.rightHz, sv.amplitude, 20 * std::log10(ax / al + 1e-9));
            CHECK(std::fabs(hl - sv.leftHz) < 1.0 && std::fabs(hr - sv.rightHz) < 1.0, "%s: tones at %.2f and %.2f Hz", sv.label.c_str(), hl, hr);
            const double tol = sv.dabPlus ? 0.3 : 1.0;     // the MP2 encoder's quantiser is coarser
            CHECK(std::fabs(20 * std::log10(al / sv.amplitude)) < tol && std::fabs(20 * std::log10(ar / sv.amplitude)) < tol, "%s: levels %.4f %.4f, expected %.2f", sv.label.c_str(), al, ar, sv.amplitude);
            CHECK(20 * std::log10(ax / al + 1e-9) < -40, "%s: crosstalk %.1f dB", sv.label.c_str(), 20 * std::log10(ax / al + 1e-9));
        } else {
            // the tune: in windows of 0.3 s the strongest tone between 200 and 900 Hz is one of the notes of the tune (or an octave of it)
            static const double notes[] = {261.63, 293.66, 329.63, 349.23, 392.00};
            int good = 0, wins = 0;
            for (size_t w0 = from; w0 + (size_t)(0.3 * 48000) < pcm.ch[0].size() && wins < 24; w0 += (size_t)(0.3 * 48000), wins++) {
                double hz = 0, am = 0;
                measureTone(pcm.ch[0], w0, (size_t)(0.3 * 48000), 48000, 200, 900, hz, am);
                bool isNote = false;
                for (double nn : notes) for (double oct : {1.0, 2.0}) isNote |= std::fabs(hz - nn * oct) < 4.0;
                good += isNote;
            }
            jprintf("   melody: %d of %d windows hold a note of the tune\n", good, wins);
            CHECK(good >= wins * 0.8, "%s: only %d of %d windows hold a note of the tune", sv.label.c_str(), good, wins);
        }
        // the logical frames come out exactly as they went in, consecutive, none lost (the first frames wait for the time deinterleaver)
        const int loopLen = (int)tx.mp2Frames((int)i).size() ? 500 : 0;
        (void)loopLen;
        int bestBad = 1 << 30;
        std::vector<uint8_t> ref;
        for (int f0 = 0; f0 < 500 && bestBad; f0++) {
            int bad = 0;
            for (size_t k = 0; k < r.frames.size() && k < 400; k++) { tx.logicalFrame((int)i, f0 + (int64_t)k, ref); bad += ref != r.frames[k]; }
            bestBad = std::min(bestBad, bad);
        }
        CHECK(r.frames.size() > 250 && bestBad == 0, "%s: %zu logical frames, %d of the first 400 differ from the transmitted ones", sv.label.c_str(), r.frames.size(), bestBad);
    });
    jobs.run();
}

// the full check of a run at the nominal signal: locked, ensemble exact, no bad FIBs, audio decoding without a bad access unit
static void checkClean(const Result& r, const dabgen::TxConfig& tc, const char* what) {
    checkEnsemble(r, tc, what);
    CHECK(fibRatio(r) > 0.995, "%s: FIB ok %.4f", what, fibRatio(r));
    CHECK(r.st.superframesOk > 20 && r.st.superframesBad == 0 && r.st.auBad == 0, "%s: superframes %llu ok %llu bad, AU %llu ok %llu bad", what, (unsigned long long)r.st.superframesOk,
          (unsigned long long)r.st.superframesBad, (unsigned long long)r.st.auOk, (unsigned long long)r.st.auBad);
}

static void testRates() {
    const dabgen::TxConfig tc = baseConfig();
    SynthConfig sc;
    sc.snrDb = 30;
    testjobs::Jobs jobs;
    for (double rate : {2.0e6, 2.048e6, 2.4e6, 4e6, 8e6, 10e6, 20e6}) jobs.add([=] {
        char what[64];
        snprintf(what, sizeof what, "%.3f Msps", rate / 1e6);
        const auto t0 = std::chrono::steady_clock::now();
        const Result r = run(tc, sc, rate, 4.0, 65536, 1, false);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        jprintf("%s: frames %llu, FIB ok %.2f%%, SNR %.1f dB, CFO %+.0f Hz, AU %llu/%llu, %.1f s for 4 s of signal\n", what, (unsigned long long)r.tel.frames, 100 * fibRatio(r), r.tel.snrDb, r.tel.cfoHz,
               (unsigned long long)r.st.auOk, (unsigned long long)r.st.auBad, el);
        checkClean(r, tc, what);
        CHECK(std::fabs(r.tel.cfoHz) < 50, "%s: carrier offset %.1f Hz", what, r.tel.cfoHz);
    });
    jobs.run();
}

static void testSnr() {
    const dabgen::TxConfig tc = baseConfig();
    jprintf("C/N sweep at 2.048 Msps (C/N in the 2.048 MHz sample band; the 1.536 MHz ensemble sees 1.25 dB more):\n   C/N   locked  FIB ok   ensemble  labels  audio AU ok / bad\n");
    double lastGoodFic = 99, lastGoodAudio = 99;
    const std::vector<double> snrs = {25.0, 20.0, 15.0, 12.0, 10.0, 8.0, 6.0, 5.0, 4.0, 3.0, 2.0, 1.0, 0.0, -2.0};
    // the runs are independent: they go side by side, the table is written in order afterwards
    std::vector<Result> runs(snrs.size());
    testjobs::Jobs jobs;
    for (size_t k = 0; k < snrs.size(); k++) jobs.add([&, k] {
        SynthConfig sc;
        sc.snrDb = snrs[k];
        runs[k] = run(tc, sc, 2.048e6, 10.0, 65536, 1, false);
    });
    jobs.run();
    for (size_t k = 0; k < snrs.size(); k++) {
        const double snr = snrs[k];
        const Result& r = runs[k];
        bool labels = r.ens.valid && r.ens.label == "OnAir DAB" && r.ens.services.size() == 4;
        for (const auto& kv : r.ens.services) labels &= !kv.second.label.empty();
        jprintf("  %5.1f   %-6s %6.1f%%   %-8s  %-6s  %llu / %llu\n", snr, r.tel.state == 2 ? "yes" : "no", 100 * fibRatio(r), r.ens.valid ? "yes" : "no", labels ? "all" : "no", (unsigned long long)r.st.auOk,
               (unsigned long long)r.st.auBad);
        const bool ficOk = r.tel.state == 2 && fibRatio(r) > 0.95 && labels;
        const bool audioOk = r.st.auOk > 400 && r.st.auBad * 20 < r.st.auOk;
        if (ficOk) lastGoodFic = snr;
        if (audioOk) lastGoodAudio = snr;
        if (snr >= 6) CHECK(ficOk && audioOk, "C/N %.0f dB: FIC ok %d, audio ok %d (FIB ok %.3f, AU %llu / %llu)", snr, ficOk, audioOk, fibRatio(r), (unsigned long long)r.st.auOk, (unsigned long long)r.st.auBad);
        if (snr <= -2) CHECK(!ficOk, "noise has no effect at %.0f dB C/N (FIB ok %.3f)", snr, fibRatio(r));
    }
    jprintf("lowest C/N of the sweep with a working FIC (FIB ok > 95%% and every label): %.0f dB; audio of 48 kbit/s EEP 3-A (AU error rate < 5%%): %.0f dB\n", lastGoodFic, lastGoodAudio);
}

static void testOffsets() {
    const dabgen::TxConfig tc = baseConfig();
    testjobs::Jobs jobs;
    for (double rate : {2.048e6, 10e6}) {
        for (double cfo : {-5000.0, -4500.0, -3500.0, -2500.0, -2490.0, -1500.0, -1234.5, 0.0, 700.0, 1500.0, 2500.0, 2510.0, 3300.0, 3500.0, 4500.0, 5000.0}) jobs.add([=] {   // 1.5, 2.5, ... spacings: half a carrier spacing is where the integer search ties
            SynthConfig sc;
            sc.snrDb = 25; sc.cfoHz = cfo;
            const Result r = run(tc, sc, rate, 5.0, 65536, 1, false);
            char what[96];
            snprintf(what, sizeof what, "%.3f Msps, carrier offset %+.1f Hz", rate / 1e6, cfo);
            jprintf("%s: locked %d, estimated %+.1f Hz, FIB ok %.2f%%, AU bad %llu\n", what, r.tel.state == 2, r.tel.cfoHz, 100 * fibRatio(r), (unsigned long long)r.st.auBad);
            checkClean(r, tc, what);
            CHECK(std::fabs(r.tel.cfoHz - cfo) < 25, "%s: the receiver reads %+.1f Hz", what, r.tel.cfoHz);
        });
        for (double ppm : {-20.0, -5.0, 5.0, 20.0}) jobs.add([=] {
            SynthConfig sc;
            sc.snrDb = 25; sc.sroPpm = ppm; sc.cfoHz = 800;
            const Result r = run(tc, sc, rate, 12.0, 65536, 1, false);
            char what[96];
            snprintf(what, sizeof what, "%.3f Msps, clock offset %+.0f ppm", rate / 1e6, ppm);
            jprintf("%s: locked %d, frames %llu, FIB ok %.2f%%, AU %llu ok %llu bad\n", what, r.tel.state == 2, (unsigned long long)r.tel.frames, 100 * fibRatio(r), (unsigned long long)r.st.auOk, (unsigned long long)r.st.auBad);
            checkClean(r, tc, what);
        });
    }
    jobs.run();
}

static void testChunks() {
    const dabgen::TxConfig tc = baseConfig();
    SynthConfig sc;
    sc.snrDb = 25; sc.cfoHz = 1234; sc.sroPpm = 8;
    uint64_t refFrames = 0, refFibOk = 0, refAu = 0;
    const std::vector<size_t> chunks = {65536, 4096, 7, 1};
    // the runs are independent: they go side by side, the comparison with the first one is made in order afterwards
    std::vector<Result> runs(chunks.size());
    std::vector<double> secs(chunks.size());
    testjobs::Jobs jobs;
    for (size_t k = 0; k < chunks.size(); k++) jobs.add([&, k] {
        const auto t0 = std::chrono::steady_clock::now();
        runs[k] = run(tc, sc, 2.048e6, 1.2, chunks[k], 1, true);
        secs[k] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    });
    jobs.run();
    for (size_t k = 0; k < chunks.size(); k++) {
        const size_t chunk = chunks[k];
        const Result& r = runs[k];
        const double el = secs[k];
        jprintf("chunks of %zu samples: frames %llu, FIB ok %llu bad %llu, AU %llu ok (%.1f s)\n", chunk, (unsigned long long)r.tel.frames, (unsigned long long)r.tel.fibOk, (unsigned long long)r.tel.fibBad,
               (unsigned long long)r.st.auOk, el);
        CHECK(r.tel.state == 2 && r.tel.fibBad == 0 && r.ens.valid, "chunk %zu: state %d, bad FIBs %llu", chunk, r.tel.state, (unsigned long long)r.tel.fibBad);
        if (chunk == 65536) { refFrames = r.tel.frames; refFibOk = r.tel.fibOk; refAu = r.st.auOk; CHECK(refFrames >= 10 && refAu > 0, "reference run: %llu frames", (unsigned long long)refFrames); }
        else CHECK(r.tel.frames == refFrames && r.tel.fibOk == refFibOk && r.st.auOk == refAu, "chunks of %zu samples change what the receiver decodes (%llu / %llu / %llu against %llu / %llu / %llu)", chunk,
                   (unsigned long long)r.tel.frames, (unsigned long long)r.tel.fibOk, (unsigned long long)r.st.auOk, (unsigned long long)refFrames, (unsigned long long)refFibOk, (unsigned long long)refAu);
    }
}

int main() {
    testAudio();
    testRates();
    testSnr();
    testOffsets();
    testChunks();
    if (fails) { jprintf("%d FAILED\n", fails.load()); return 1; }
    jprintf("test_dab_rx: all passed\n");
    return 0;
}
