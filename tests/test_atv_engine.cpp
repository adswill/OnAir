// Analog TV through the engine: the synthetic source -> Engine -> telemetry, pictures and sound, in real time.
// First the card the app plays by default (PAL B/G, test card, 1 kHz tone with gaps, a carrier offset and noise) for about 15 seconds, then SECAM D/K
// and NTSC M for about 8 seconds each, started from the same engine, and a retune in the middle.
#include "dect2/atv_testkit.h"
#include "dect2/engine.h"
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Expect {
    const char* label;
    int sysOpt, colourOpt;          // SynthConfig modeOpt[0] and [1]
    double snr, cfo, seconds;
    const char* system;
    int sysCode, colCode;           // for the picture to compare with
    double spacingMhz;
};

static void scenario(Engine& e, const Expect& x, bool first) {
    std::vector<float> audio;
    e.atv().setSilent(true);
    e.atv().setAudioTap([&](const float* l, const float*, size_t n) { audio.insert(audio.end(), l, l + n); });
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "synthetic";
    TuneSettings tune;
    const ModeTuning* mt = modeTuning(10);
    CHECK(mt != nullptr && mt->stdMode == 10, "analog TV in the tuning table");
    tune.bandwidthMhz = mt->bandwidthMhz; tune.sampleRate = mt->sampleRate; tune.basebandFilterHz = mt->basebandHz; tune.centerHz = mt->defMhz * 1e6;
    tune.synth.mode = 10; tune.synth.snrDb = x.snr; tune.synth.cfoHz = x.cfo;
    tune.synth.modeOpt[0] = x.sysOpt; tune.synth.modeOpt[1] = x.colourOpt;
    FileOptions fo;
    e.setStandard(10);
    CHECK(e.start(dev, tune, fo), "%s: engine started", x.label);
    CHECK(e.activeStandard() == 9, "%s: analog TV is the active standard (%d)", x.label, e.activeStandard());
    RxTelemetry t;
    uint64_t seq = 0, fseq = 0, pictures = 0;
    bool locked = false;
    int lockedAt = -1;
    std::shared_ptr<const AtvFrame> last;
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t fieldsAtLock = 0, fieldsLater = 0;
    double lockedSecs = 0;
    for (int i = 0; i < 1000; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (e.latestRx(t, seq)) {
            seq = t.seq;
            if (t.standard == 9 && t.atv.state == 2 && !locked) { locked = true; lockedAt = i * 50; fieldsAtLock = t.atv.fieldCount; lockedSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
            if (locked) fieldsLater = t.atv.fieldCount;
        }
        while (auto f = e.atv().frame(fseq)) { last = f; pictures++; }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::duration<double>(x.seconds)) break;
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    e.latestRx(t, 0);
    printf("%s: %s\n", x.label, atvSummary(t.atv).c_str());
    printf("   locked after %d ms, %llu fields in %.1f s since, %llu pictures, %zu audio samples, dropped %llu, sound %s (%.0f Hz from the vision carrier)\n", lockedAt,
           (unsigned long long)(fieldsLater - fieldsAtLock), elapsed - lockedSecs, (unsigned long long)pictures, audio.size(), (unsigned long long)e.droppedSamples(),
           t.atv.soundPresent ? "yes" : "no", t.atv.soundHz - t.atv.visionHz);
    CHECK(t.standard == 9, "%s: standard %d", x.label, t.standard);
    CHECK(locked && lockedAt < 4000, "%s: locked after %d ms", x.label, lockedAt);
    CHECK(t.atv.state == 2 && t.state == t.atv.state && t.dataValid == t.atv.dataValid, "%s: state %d, copied %d", x.label, t.atv.state, t.state);
    CHECK(t.atv.system == x.system && t.atv.colour, "%s: system '%s' colour %d", x.label, t.atv.system.c_str(), (int)t.atv.colour);
    CHECK(std::fabs(t.cfoHz - x.cfo) < 400 && std::fabs(t.atv.cfoHz - x.cfo) < 400, "%s: carrier offset %.0f Hz (sent %.0f)", x.label, t.cfoHz, x.cfo);
    CHECK(t.dataSnrDb > 15 && t.dataSnrDb < 45, "%s: video SNR %.1f dB", x.label, t.dataSnrDb);
    CHECK(t.rateOk, "%s: the sample rate is accepted", x.label);
    const double rate = (double)(fieldsLater - fieldsAtLock) / std::max(0.1, elapsed - lockedSecs);
    const double nominal = x.sysCode == kAtvM ? 59.94 : 50.0;
    CHECK(rate > nominal * 0.92 && rate < nominal * 1.08, "%s: field rate %.1f per second of wall time (%.2f expected): the engine does not keep up, or runs fast", x.label, rate, nominal);
    // the picture count depends on how often this thread gets the CPU: not checked on the shared CI machines
    if (!std::getenv("CI")) CHECK(pictures > nominal * 0.4 * (x.seconds - 2), "%s: %llu pictures", x.label, (unsigned long long)pictures);
    CHECK(e.droppedSamples() == 0, "%s: %llu samples dropped", x.label, (unsigned long long)e.droppedSamples());
    CHECK(t.atv.blocksBad < 4 && t.atv.blocksOk > nominal * (x.seconds - 3), "%s: fields ok %llu bad %llu", x.label, (unsigned long long)t.atv.blocksOk, (unsigned long long)t.atv.blocksBad);
    CHECK(last && last->colour, "%s: picture", x.label);
    CHECK(std::fabs(t.atv.soundHz - t.atv.visionHz - x.spacingMhz * 1e6) < 3000 && t.atv.soundPresent, "%s: sound carrier %.0f Hz from the vision carrier", x.label, t.atv.soundHz - t.atv.visionHz);
    if (last) {
        AtvFormat f; atvMakeFormat(x.sysCode, x.colCode, f);
        CHECK(last->width == f.picW && last->height == f.picH, "%s: picture size %d x %d", x.label, last->width, last->height);
        AtvCard card(f);
        double c[8][3];
        atvkit::barColours(*last, card, c);
        double worst = 0;
        for (int i = 0; i < 8; i++) { float rgb[3]; atvEbuBar(i, rgb); for (int k = 0; k < 3; k++) worst = std::max(worst, std::fabs(c[i][k] - rgb[k])); }
        CHECK(worst < 0.13, "%s: colour bars error %.3f at %.0f dB", x.label, worst, x.snr);
    }
    CHECK(audio.size() > 48000 * (x.seconds - 4), "%s: %zu audio samples", x.label, audio.size());
    if (audio.size() > 48000 * 6) {
        // the tone has a gap of 0.2 s in every 1.5 s: look at a stretch that is clear of it
        const double hz = atvkit::peakTone(audio, 48000 * 3, 48000 * 3 + 9600);
        const double a1 = atvkit::toneAmp(audio, 1000, 48000 * 3, 48000 * 3 + 9600);
        printf("   sound: strongest tone %.1f Hz, 1 kHz amplitude %.3f\n", hz, a1);
        CHECK(std::fabs(hz - 1000) < 20 && std::fabs(a1 - 0.5) < 0.08, "%s: tone at %.1f Hz amplitude %.3f", x.label, hz, a1);
    }
    e.stop();
    e.atv().setAudioTap(nullptr);
    (void)first;
}

int main() {
    Engine e;
    scenario(e, {"PAL B/G", 0, 0, 32, 12000, 15, "PAL B/G 625/50", kAtvG, kAtvPal, 5.5}, true);
    scenario(e, {"SECAM D/K", 3, 3, 35, -30000, 8, "SECAM D/K 625/50", kAtvDK, kAtvSecam, 6.5}, false);
    scenario(e, {"NTSC M", 4, 0, 35, 5000, 8, "NTSC M 525/59.94", kAtvM, kAtvNtsc, 4.5}, false);
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
