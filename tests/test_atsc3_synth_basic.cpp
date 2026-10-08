// The ATSC 3.0 test signal without a receiver: the programme (fragmented MP4 made in memory), the level, the channel model (noise, carrier offset, clock
// offset, echo), independence of the chunk size, the speed.
#include "../core/src/atsc3_synth_content.h"
#include "atsc3_sim.h"
#include "dect2/atsc3_synth.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static double wall() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static std::vector<cf32> make(const SynthConfig& cfg, double rate, size_t n, size_t chunk) {
    auto s = makeAtsc3Synth(cfg, rate);
    std::vector<cf32> v;
    if (!s) return v;
    v.resize(n);
    for (size_t off = 0; off < n; off += chunk) s->generate(v.data() + off, std::min(chunk, n - off));
    return v;
}

// packets of a track: init segment, then `cycles` loops of its fragments
struct Demuxed { long packets = 0; bool increasing = true; long long first = 0, last = 0, firstOfSecondLoop = -1; double seconds = 0; };
static bool demux(const atsc3synth::Content& c, bool video, int cycles, Demuxed& d) {
    std::vector<uint8_t> f = video ? c.vinit : c.ainit;
    for (int cy = 0; cy < cycles; cy++)
        for (int s = 0; s < c.slots; s++) { auto fr = atsc3synth::shiftedFragment(c, video, s, (uint64_t)cy); f.insert(f.end(), fr.begin(), fr.end()); }
    sim::MemIn mi{f.data(), f.size()};
    unsigned char* iobuf = (unsigned char*)av_malloc(1 << 16);
    AVIOContext* io = avio_alloc_context(iobuf, 1 << 16, 0, &mi, sim::memRead, nullptr, nullptr);
    AVFormatContext* ic = avformat_alloc_context();
    ic->pb = io;
    bool ok = avformat_open_input(&ic, "", av_find_input_format("mov"), nullptr) >= 0;
    if (ok) {
        AVPacket* pk = av_packet_alloc();
        long long prev = INT64_MIN;
        int64_t secondLoop = (int64_t)(video ? c.vLoopTicks : c.aLoopTicks);
        while (av_read_frame(ic, pk) >= 0) {
            const long long t = pk->dts;
            if (d.packets == 0) d.first = t;
            if (prev != INT64_MIN && t <= prev) d.increasing = false;
            if (t >= secondLoop && d.firstOfSecondLoop < 0) d.firstOfSecondLoop = t;
            prev = t; d.last = t; d.packets++;
            av_packet_unref(pk);
        }
        av_packet_free(&pk);
        avformat_close_input(&ic);
    }
    av_freep(&io->buffer);
    avio_context_free(&io);
    return ok;
}

int main() {
    // ---- the programme
    const double tc0 = wall();
    auto content = atsc3synth::getContent(0, 700);
    const double tEncode = wall() - tc0;
    CHECK(content != nullptr, "the programme is encoded");
    if (!content) { printf("atsc3 synth basic: FAILED\n"); return 1; }
    printf("  programme made in %.2f s: %s (%s), %d slots, video %.0f kbit/s, audio %.0f kbit/s, time scales %u and %u\n", tEncode, content->videoEncoder.c_str(), content->videoCodec.c_str(), content->slots,
           content->videoBitrate / 1e3, content->audioBitrate / 1e3, content->vTimescale, content->aTimescale);
    int others = 0;
    for (int pref : {2, 3}) {   // the other encoders: how long they take, and that they work
        const double t0 = wall();
        auto c2 = atsc3synth::getContent(pref, 700);
        printf("  with codec %d: %s, made in %.2f s, video %.0f kbit/s\n", pref, c2 ? c2->videoEncoder.c_str() : "none", wall() - t0, c2 ? c2->videoBitrate / 1e3 : 0.0);
        // none: no encoder of that kind in this FFmpeg (the Windows build has no H.264 encoder: no x264, and no NVENC under Wine)
        if (c2) { others++; CHECK(c2->slots == 8, "the programme with another video encoder"); }
    }
    CHECK(others > 0, "no other video encoder works");
    CHECK(content->slots == 8 && content->vseg.size() == 8 && content->aseg.size() == 8, "eight fragments of each");
    CHECK(content->vinit.size() > 8 && !memcmp(&content->vinit[4], "ftyp", 4) && !memcmp(&content->ainit[4], "ftyp", 4), "init segments");
    bool frag = true;
    for (int i = 0; i < 8; i++) frag = frag && !memcmp(&content->vseg[i][4], "moof", 4) && !memcmp(&content->aseg[i][4], "moof", 4);
    CHECK(frag, "fragments start with a moof");
    for (bool video : {true, false}) {
        Demuxed d;
        CHECK(demux(*content, video, 2, d), "the fragments open as MP4");
        printf("  %s: %ld packets in two loops, time %lld .. %lld, second loop starts at %lld\n", video ? "video" : "audio", d.packets, d.first, d.last, d.firstOfSecondLoop);
        CHECK(d.packets == (video ? 400 : 750), video ? "200 pictures per loop" : "375 sound frames per loop");
        CHECK(d.increasing, "decoding times only go up, also across the loop");
        CHECK(d.first == 0 && d.firstOfSecondLoop == (video ? content->vLoopTicks : content->aLoopTicks), "the second loop continues the time of the first");
    }

    // ---- level and peak
    {
        SynthConfig cfg; cfg.snrDb = 25;
        auto s = makeAtsc3Synth(cfg, 10e6);
        CHECK(s != nullptr, "generator");
        std::vector<cf32> b(65536);
        double e = 0, pk = 0;
        size_t tot = 0;
        double busy = 0;
        while (tot < 20 * 10000000u) {
            const double t0 = wall();
            s->generate(b.data(), b.size());
            busy += wall() - t0;
            for (auto& v : b) { const double m = std::norm(v); e += m; pk = std::max(pk, std::sqrt(m)); }
            tot += b.size();
        }
        const double rms = std::sqrt(e / tot);
        printf("  20 s at 10 Msps: rms %.3f, peak %.3f, %.1f times real time on one core\n", rms, pk, 20.0 / busy);
        CHECK(rms > 0.19 && rms < 0.25, "rms between 0.19 and 0.25");
        CHECK(pk < 0.95, "peak below about 0.9");
        CHECK(20.0 / busy > 1.5, "faster than real time by a safe margin");
    }

    // ---- noise: with the same programme, the difference between a noisy and a clean signal is the noise, at the power the SNR says (in the 6.144 MHz of the
    // native rate, so more at higher sample rates)
    for (double rate : {6.144e6, 10e6, 20e6}) {
        SynthConfig a, b;
        a.snrDb = 150; b.snrDb = 12;
        const size_t n = (size_t)(rate * 1.5);
        auto x = make(a, rate, n, 65536), y = make(b, rate, n, 65536);
        const double np = std::pow(10.0, -12.0 / 10.0) * rate / 6.144e6;
        const double ka = 0.2, kb = 0.2 / std::sqrt(1.0 + np);   // the scale of each
        double ne = 0, se = 0;
        for (size_t i = 0; i < n; i++) { cf32 d = y[i] / (float)kb - x[i] / (float)ka; ne += std::norm(d); se += std::norm(x[i] / (float)ka); }
        ne /= n; se /= n;
        printf("  noise at %.3f Msps: signal power %.3f, noise power %.4f (expected %.4f), SNR %.2f dB\n", rate / 1e6, se, ne, np, 10 * std::log10(se / ne));
        CHECK(std::fabs(ne / np - 1.0) < 0.06, "noise power as the SNR says");
        CHECK(std::fabs(se - 1.0) < 0.1, "unit signal power");
    }

    // ---- carrier offset: the phase of the offset signal against the clean one grows at 2 pi f / rate per sample
    for (double cfo : {3000.0, -2345.0}) {
        SynthConfig a, b;
        a.snrDb = b.snrDb = 150; b.cfoHz = cfo;
        const double rate = 8e6;
        const size_t n = 1000000;
        auto x = make(a, rate, n, 4096), y = make(b, rate, n, 3333);
        double worst = 0;
        long used = 0;
        for (size_t i = 0; i < n; i++) {
            if (std::abs(x[i]) < 0.1f) continue;
            const double ph = std::arg(y[i] * std::conj(x[i]));
            const double want = 2 * M_PI * cfo * (double)i / rate;
            worst = std::max(worst, std::fabs(std::remainder(ph - want, 2 * M_PI)));
            used++;
        }
        printf("  carrier offset %+.0f Hz: worst phase error %.4f rad over %ld samples\n", cfo, worst, used);
        CHECK(used > 1000 && worst < 0.01, "carrier offset");
    }

    // ---- clock offset: +100 ppm moves the signal by 150 samples after 1.5 million
    {
        SynthConfig a, b;
        a.snrDb = b.snrDb = 150; b.sroPpm = 100;
        const double rate = 10e6;
        const size_t n = 1600000, at = 1500000, len = 20000;
        auto x = make(a, rate, n, 65536), y = make(b, rate, n, 65536);
        int best = 0;
        double bestV = -1;
        for (int lag = -300; lag <= 300; lag++) {
            double acc = 0;
            cf32 s(0, 0);
            for (size_t i = at; i < at + len; i++) s += y[i + lag] * std::conj(x[i]);
            acc = std::abs(s);
            if (acc > bestV) { bestV = acc; best = lag; }
        }
        printf("  clock offset +100 ppm: the signal sits %d samples later after 1.5 M samples (expected 150)\n", best);
        CHECK(std::abs(best - 150) <= 2, "clock offset");
    }

    // ---- echo: the signal plus a copy 300 samples later, 10 dB down
    {
        SynthConfig a, b;
        a.snrDb = b.snrDb = 150; b.echoDb = 10; b.echoDelay = 300;
        auto x = make(a, 10e6, 400000, 65536), y = make(b, 10e6, 400000, 65536);
        const double kb = 0.2 / std::sqrt(1.0 + 0.1), ka = 0.2;
        double err = 0;
        const cf32 g = cf32(std::cos(0.8f), std::sin(0.8f)) * (float)std::pow(10.0, -0.5);
        for (size_t i = 1000; i < 399000; i++) err += std::norm(y[i] / (float)kb - (x[i] / (float)ka + x[i - 300] / (float)ka * g));
        err /= 398000;
        printf("  echo: error power %.2e\n", err);
        CHECK(err < 1e-3, "echo");
    }

    // ---- chunk sizes give the same signal
    {
        SynthConfig cfg; cfg.snrDb = 18; cfg.cfoHz = 1234; cfg.sroPpm = 7; cfg.echoDb = 14;
        const double rate = 9e6;
        auto ref = make(cfg, rate, 600000, 65536);
        for (size_t chunk : {(size_t)1, (size_t)7, (size_t)4096, (size_t)100000}) {
            auto v = make(cfg, rate, chunk == 1 ? 120000 : 600000, chunk);
            double worst = 0;
            for (size_t i = 0; i < v.size(); i++) worst = std::max(worst, (double)std::abs(v[i] - ref[i]));
            printf("  chunk %6zu: largest difference %.2e\n", chunk, worst);
            CHECK(worst < 1e-4, "the chunk size does not change the signal");
        }
    }

    // ---- bad requests
    {
        SynthConfig cfg;
        cfg.modeOpt[0] = 9; cfg.modeOpt[1] = 99; cfg.modeOpt[3] = 77; cfg.modeOpt[4] = 42; cfg.modeOpt[5] = -3;   // nonsense values mean the defaults
        CHECK(makeAtsc3Synth(cfg, 10e6) != nullptr, "nonsense options fall back to the defaults");
        CHECK(makeAtsc3Synth(SynthConfig(), 0) == nullptr, "no sample rate, no generator");
        CHECK(makeAtsc3Synth(SynthConfig(), 6.144e6) != nullptr, "the native rate");
        auto low = makeAtsc3Synth(SynthConfig(), 5e6);
        CHECK(low != nullptr, "a rate below the native one");
        if (low) { std::vector<cf32> v(100000); low->generate(v.data(), v.size()); double e = 0; for (auto& x : v) e += std::norm(x); CHECK(e > 0 && std::isfinite(e), "it makes samples"); }
        CHECK(makeAtsc3Synth(SynthConfig(), 40e6) == nullptr, "a rate more than four times the native one is refused");
    }
    printf(fails ? "atsc3 synth basic: FAILED\n" : "atsc3 synth basic: ok\n");
    return fails ? 1 : 0;
}
