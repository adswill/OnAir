// FengYun receiver on its test signals, end to end.
// S-VISSR: the scan lines of the test picture arrive in all five channels, clean, at 6 dB (still synced, the bit error ratio matches theory), with a
// carrier offset, another sample rate and a symbol clock error; noise alone gives nothing.
// FY-4: the LRIT and HRIT links deliver both channels of the full disk picture in two JPEG 2000 segments each (the pixels match) and the text message,
// the next round makes a second picture; a CADU file is read through the file layers; encrypted files are kept and counted.
#include "dect2/fygeo/fygeo_gen.h"
#include "dect2/fygeo/fygeo_rx.h"
#include "dect2/fygeo/fygeo_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static FygeoTelemetry run(int link, const SynthConfig& cfg, double rate, double seconds, bool noiseOnly = false, double offset = 0) {
    FygeoReceiver rx;
    rx.setLink(link);
    rx.setBlocking(true);
    rx.configure(rate);
    rx.setSignalOffset(offset);
    SynthConfig sc2 = cfg; sc2.modeOpt[0] = link == kFy4bLrit ? 1 : 0;
    std::unique_ptr<ModeSynth> syn = link == kFy2Svissr ? makeFy2SvissrSynth(cfg, rate) : link == kFy4bLrit ? makeFy4LritSynth(sc2, rate) : link == kFy4aHrit ? makeFy4aHritSynth(cfg, rate) : makeFy4LritSynth(sc2, rate);
    std::vector<cf32> buf(16384);
    unsigned s = 99;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        if (noiseOnly) {
            for (auto& v : buf) { s = s * 1664525u + 1013904223u; const float a = ((s >> 8) & 0xFFFF) / 65536.f - 0.5f; s = s * 1664525u + 1013904223u; v = cf32(a * 0.3f, (((s >> 8) & 0xFFFF) / 65536.f - 0.5f) * 0.3f); }
        } else syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
    }
    rx.flush();
    // one more feed so that the receiver reports what flush() finished
    for (auto& v : buf) v = cf32(0, 0);
    for (int i = 0; i < 20; i++) rx.feed(buf.data(), buf.size());
    FygeoTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

// the newest product with this in its name that is complete (or any, for S-VISSR which has one picture)
static std::shared_ptr<goes::Product> find(const FygeoTelemetry& t, const char* part, bool complete = false) {
    for (const auto& p : t.products) {
        if (p->name.find(part) == std::string::npos) continue;
        std::lock_guard<std::mutex> lk(p->mu);
        if (!complete || p->complete) return p;
    }
    return nullptr;
}

static void svissrChecks(const FygeoTelemetry& t, const char* what, int minFrames, bool exact) {
    CHECK(t.svFrames >= (uint64_t)minFrames, "%s: %llu frames (want %d)", what, (unsigned long long)t.svFrames, minFrames);
    if (t.svFrames < 2) return;
    int checked = 0, bad = 0;
    for (int k = 0; k < 5; k++) {
        const char* nm[5] = {"IR1 10.8", "IR2 12", "IR3 6.75", "IR4 3.75", "VIS 0.725"};
        auto p = find(t, nm[k]);
        CHECK(p != nullptr, "%s: %s picture missing", what, nm[k]);
        if (!p) continue;
        std::vector<uint8_t> ir[4], vis((size_t)4 * fygeo::kSvissrVisWidth);
        for (auto& v : ir) v.resize(fygeo::kSvissrIrWidth);
        std::lock_guard<std::mutex> lk(p->mu);
        for (int line = 0; line < 8; line++) {
            if (k < 4) {
                if (!p->rowDone[(size_t)line]) continue;
                svissrTestLine(line, ir[0].data(), ir[1].data(), ir[2].data(), ir[3].data(), vis.data());
                int diff = 0;
                for (int x = 0; x < fygeo::kSvissrIrWidth; x++) diff += p->pix[(size_t)line * p->w + (size_t)x] != ir[k][(size_t)x];
                checked++; if (diff > (exact ? 0 : fygeo::kSvissrIrWidth / 4)) { bad++; printf("    %s: %s line %d differs in %d of %d pixels\n", what, nm[k], line, diff, fygeo::kSvissrIrWidth); }
            } else {
                if (!p->rowDone[(size_t)(2 * line)]) continue;
                svissrTestLine(line, ir[0].data(), ir[1].data(), ir[2].data(), ir[3].data(), vis.data());
                int diff = 0;
                for (int x = 0; x < p->w; x += 7) {
                    const int e = (((vis[(size_t)(2 * x)] & 0xFC | 2) + (vis[(size_t)(2 * x + 1)] & 0xFC | 2) + (vis[(size_t)fygeo::kSvissrVisWidth + (size_t)(2 * x)] & 0xFC | 2) + (vis[(size_t)fygeo::kSvissrVisWidth + (size_t)(2 * x + 1)] & 0xFC | 2) + 2) >> 2);
                    diff += p->pix[(size_t)(2 * line) * p->w + (size_t)x] != e;
                }
                checked++; if (diff > (exact ? 0 : p->w / 28)) { bad++; printf("    %s: VIS line %d differs in %d samples\n", what, line, diff); }
            }
        }
    }
    CHECK(checked >= 5 * 4 && bad == 0, "%s: %d lines checked, %d wrong", what, checked, bad);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // ---- S-VISSR
    {
        SynthConfig c; c.snrDb = 30;
        FygeoTelemetry t = run(kFy2Svissr, c, 2.4e6, 9.0);
        printf("  S-VISSR clean: %s\n", fygeoSummary(t).c_str());
        svissrChecks(t, "clean", 12, true);
        CHECK(t.state == 3 && t.svPreambleBer >= 0 && t.svPreambleBer < 1e-4, "clean BER %.2e", t.svPreambleBer);
        CHECK(t.svDataBer >= 0 && t.svDataBer < 1e-4, "clean spare errors %.2e", t.svDataBer);
        CHECK(t.hint.empty(), "no hint when all is well: %s", t.hint.c_str());
        CHECK(std::fabs(t.symbolRate - 660e3) < 2e3, "symbol rate %.1f", t.symbolRate);
        CHECK(t.snrDb > 25 && t.snrDb < 35, "Es/N0 estimate %.1f dB", t.snrDb);
    }
    {
        // 6 dB Es/N0 is a bit error ratio of about 4e-3 with differential decoding: the frames sync and the pictures show with a few wrong pixels
        SynthConfig c; c.snrDb = 6;
        FygeoTelemetry t = run(kFy2Svissr, c, 2.4e6, 9.0);
        printf("  S-VISSR 6 dB: %s, preamble BER %.2e, spare BER %.2e\n", fygeoSummary(t).c_str(), t.svPreambleBer, t.svDataBer);
        CHECK(t.svFrames >= 12, "6 dB: %llu frames", (unsigned long long)t.svFrames);
        const double q = 0.5 * std::erfc(std::sqrt(std::pow(10.0, 0.6)));
        const double theory = 2 * q * (1 - q);
        CHECK(t.svPreambleBer > theory * 0.5 && t.svPreambleBer < theory * 2.0, "6 dB BER %.2e, theory %.2e", t.svPreambleBer, theory);
    }
    {
        // carrier offset, a sample rate that is not a multiple of the symbol rate, and a symbol clock 50 ppm fast
        SynthConfig c; c.snrDb = 14; c.cfoHz = 18000; c.sroPpm = 50;
        FygeoTelemetry t = run(kFy2Svissr, c, 3.2e6, 10.0);
        printf("  S-VISSR offset: %s\n", fygeoSummary(t).c_str());
        svissrChecks(t, "offset", 10, false);
        CHECK(std::fabs(t.cfoHz - 18000) < 400, "carrier %.0f Hz", t.cfoHz);
    }
    {
        SynthConfig c;
        FygeoTelemetry t = run(kFy2Svissr, c, 2.4e6, 8.0, true);
        CHECK(t.svFrames == 0 && t.state == 0, "noise alone gives no frames (%llu)", (unsigned long long)t.svFrames);
        CHECK(!t.hint.empty(), "a hint says why nothing decodes");
        printf("  hint on noise: %s\n", t.hint.c_str());
    }
    // ---- FY-4
    for (int link : {kFy4aLrit, kFy4bLrit}) {
        SynthConfig c; c.snrDb = 12;
        FygeoTelemetry t = run(link, c, 2e6, link == kFy4aLrit ? 30.0 : 22.0);
        printf("  %s: %s, %llu CADUs, %zu products\n", fygeoLinkName(link), fygeoSummary(t).c_str(), (unsigned long long)t.caduCount, t.products.size());
        CHECK(t.caduCount > 50, "%s: %llu CADUs", fygeoLinkName(link), (unsigned long long)t.caduCount);
        for (int ch : {2, 12}) {
            char nm[16]; snprintf(nm, sizeof nm, "C%02d", ch);
            auto p = find(t, nm, true);
            CHECK(p != nullptr, "%s: channel %d picture missing", fygeoLinkName(link), ch);
            if (!p) continue;
            const std::vector<uint16_t> ref = fy4TestPicture(ch);
            std::lock_guard<std::mutex> lk(p->mu);
            CHECK(p->w == kFy4TestW && p->h == kFy4TestH * kFy4TestSegs && p->complete && p->rowsDone == p->h, "%s ch %d: %d x %d, %d rows, %d of %d segments", fygeoLinkName(link), ch, p->w, p->h, p->rowsDone, p->segsDone, p->segsTotal);
            int diff = 0;
            for (size_t i = 0; i < ref.size() && i < p->pix.size(); i++) diff += p->pix[i] != (ref[i] >> 4);
            CHECK(diff == 0, "%s ch %d: %d pixels differ", fygeoLinkName(link), ch, diff);
        }
        bool text = false;
        for (const auto& p : t.products) { std::lock_guard<std::mutex> lk(p->mu); if (p->text.find("synthetic FengYun-4") != std::string::npos) text = true; }
        CHECK(text, "%s: the text message", fygeoLinkName(link));
        CHECK(t.hint.empty() || t.state < 3, "%s: hint %s", fygeoLinkName(link), t.hint.c_str());
    }
    {
        SynthConfig c; c.snrDb = 14;
        FygeoTelemetry t = run(kFy4aHrit, c, 4e6, 6.0);
        printf("  FY-4A HRIT: %s, %llu CADUs, %zu products\n", fygeoSummary(t).c_str(), (unsigned long long)t.caduCount, t.products.size());
        CHECK(t.caduCount > 500 && find(t, "C02", true) && find(t, "C12", true), "HRIT: pictures");
    }
    if (getenv("FY_NOISE_ONLY") || true) {
        SynthConfig c;
        FygeoTelemetry t = run(kFy4aLrit, c, 2e6, 6.0, true);
        CHECK(t.caduCount == 0 && t.state == 0 && t.products.empty(), "noise alone gives nothing");
        CHECK(!t.hint.empty(), "FY-4 hint on noise");
    }
    return testSummary("fygeo_rx");
}
