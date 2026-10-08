// DRM receiver: generator -> receiver on clean and noisy signals. Checks what the receiver reports (mode, occupancy, services, label, text, time) and that
// the logical frames of the audio stream come out bit-exact, for every robustness mode, occupancy and constellation, at many input rates and chunk sizes.
#include "data/drm/testkit.h"
#include "jobs.h"
#include <atomic>
#include <cstdio>
#include <string>
using namespace drmtest;
using testjobs::jprintf;
static std::atomic<int> fails{0};
// the cases are independent: each one runs on its own thread, the output keeps the order of the cases
static testjobs::Jobs jobs;
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

static void report(const char* what, const Result& r) {
    const DrmTelemetry& t = r.tel;
    jprintf("%-34s lock %5.1f s  mode %c %4.1f kHz  FAC %llu/%llu SDC %llu/%llu  frames %llu exact %llu bad %llu gaps %llu  SNR %5.1f dB  CFO %+7.1f  SRO %+6.1f  %5.1fx\n", what, r.lockSecs, t.modeName,
           t.bandwidthKhz, (unsigned long long)t.facOk, (unsigned long long)t.facBad, (unsigned long long)t.sdcOk, (unsigned long long)t.sdcBad, (unsigned long long)r.frames,
           (unsigned long long)r.exact, (unsigned long long)r.bad, (unsigned long long)r.gaps, t.snrDb, t.cfoHz, t.sroPpm, r.rt());
}

// the fields of the default test signal as the receiver reports them
static void checkDefault(const char* what, const Result& r, int mode, int occ, int qam) {
    const DrmTelemetry& t = r.tel;
    CHECK(t.state == 2 && t.dataValid, "%s: not decoding (state %d)", what, t.state);
    CHECK(t.mode == mode && t.occupancy == occ, "%s: mode %d occupancy %d (sent %d, %d)", what, t.mode, t.occupancy, mode, occ);
    CHECK(t.mscQam == qam, "%s: MSC %d-QAM (sent %d)", what, t.mscQam, qam);
    CHECK(t.services.size() == 1, "%s: %zu services", what, t.services.size());
    if (t.services.size() == 1) {
        const DrmService& s = t.services[0];
        CHECK(s.label == "OnAir DRM", "%s: label '%s'", what, s.label.c_str());
        CHECK(s.id == 0xE12345, "%s: service id %06X", what, s.id);
        CHECK(s.audio && s.language == 5 && s.languageName == "eng", "%s: language %d '%s'", what, s.language, s.languageName.c_str());
        CHECK(s.country == "ae", "%s: country '%s'", what, s.country.c_str());
        CHECK(s.programmeType == 10 && s.programmeName == "Pop music", "%s: programme type %d '%s'", what, s.programmeType, s.programmeName.c_str());
        CHECK(s.audioCoding == 0 && s.audioRateHz == 24000 && s.audioMode == 0, "%s: audio %d %d Hz mode %d", what, s.audioCoding, s.audioRateHz, s.audioMode);
    }
    CHECK(t.timeValid && t.year == 2026 && t.month == 10 && t.day == 6 && t.hour == 12, "%s: time %04d-%02d-%02d %02d:%02d", what, t.year, t.month, t.day, t.hour, t.minute);
    CHECK(t.textMessage == "OnAir DRM test signal", "%s: text message '%s'", what, t.textMessage.c_str());
    CHECK(t.facBad == 0 && t.sdcBad == 0, "%s: FAC %llu bad, SDC %llu bad", what, (unsigned long long)t.facBad, (unsigned long long)t.sdcBad);
    CHECK(t.blocksOk > 0 && t.blocksBad == 0, "%s: audio frames %llu ok %llu bad", what, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    CHECK(r.exact >= 8 && r.bad == 0 && r.gaps == 0, "%s: %llu frames exact, %llu bad, %llu gaps", what, (unsigned long long)r.exact, (unsigned long long)r.bad, (unsigned long long)r.gaps);
    CHECK(r.seqBackwards == 0, "%s: telemetry sequence went backwards", what);
}

// the default signal at the rate the app asks the radio for
static void testRoundTrip() {
    jobs.add([] {
    RunOpt o; o.secs = 16;
    const Result r = runRx(synthGen(Sig{.snrDb = 30}, 2e6), 2e6, o);
    report("B 10 kHz 64-QAM, 2 Msps, 30 dB", r);
    checkDefault("round trip", r, 1, 3, 64);
    const DrmTelemetry& t = r.tel;
    CHECK(t.longInterleave == 1 && t.sdcQam == 16 && t.protA == 0 && t.protB == 1, "round trip: interleaver %d SDC %d-QAM protection %d/%d", t.longInterleave, t.sdcQam, t.protA, t.protB);
    CHECK(std::fabs(t.snrDb - 30) < 2.0, "round trip: SNR estimate %.1f dB (signal 30 dB)", t.snrDb);
    CHECK(std::fabs(t.cfoHz) < 2.0 && std::fabs(t.sroPpm) < 5.0, "round trip: CFO %.1f Hz, SRO %.1f ppm (none sent)", t.cfoHz, t.sroPpm);
    CHECK(t.quality > 0.8, "round trip: quality %.2f", t.quality);
    CHECK(t.chanDb.size() > 100 && t.cirDb.size() == 256 && !t.facConst.empty() && !t.sdcConst.empty() && !t.mscConst.empty(), "round trip: plots missing (%zu %zu %zu %zu %zu)", t.chanDb.size(),
          t.cirDb.size(), t.facConst.size(), t.sdcConst.size(), t.mscConst.size());
    CHECK(t.facConst.size() <= 1024 && t.sdcConst.size() <= 1024 && t.mscConst.size() <= 1024, "round trip: constellation lists are too long");
    CHECK(r.lockSecs > 0 && r.lockSecs < 12, "round trip: first audio after %.1f s", r.lockSecs);
    });
}

// every mode, occupancy, constellation, interleaver and protection at 48 kHz: exact frames
static void testModes() {
    struct C { const char* name; Sig s; int mode, occ, qam; };
    Sig b64; b64.snrDb = 35;
    auto mk = [&](int mode, int occ, int q16, int shortIl, int prot) { Sig s = b64; s.mode = mode; s.occ = occ; s.qam16 = q16; s.shortIl = shortIl; s.prot = prot; return s; };
    const C cs[] = {
        {"A 10 kHz 64-QAM", mk(1, 0, 0, 0, 0), 0, 3, 64}, {"A 9 kHz 64-QAM", mk(1, 3, 0, 0, 0), 0, 2, 64}, {"A 4.5 kHz 16-QAM", mk(1, 1, 1, 0, 0), 0, 0, 16},
        {"A 5 kHz 64-QAM short", mk(1, 2, 0, 1, 0), 0, 1, 64}, {"A 18 kHz 64-QAM", mk(1, 5, 0, 0, 0), 0, 4, 64}, {"A 20 kHz 16-QAM short", mk(1, 6, 1, 1, 0), 0, 5, 16},
        {"B 4.5 kHz 64-QAM", mk(2, 1, 0, 0, 0), 1, 0, 64}, {"B 9 kHz 16-QAM", mk(2, 3, 1, 0, 0), 1, 2, 16}, {"B 18 kHz 64-QAM short", mk(2, 5, 0, 1, 0), 1, 4, 64},
        {"B 20 kHz 64-QAM PL3", mk(2, 6, 0, 0, 4), 1, 5, 64}, {"C 10 kHz 64-QAM", mk(3, 0, 0, 0, 0), 2, 3, 64}, {"C 20 kHz 16-QAM", mk(3, 6, 1, 0, 0), 2, 5, 16},
        {"D 10 kHz 64-QAM", mk(4, 0, 0, 0, 0), 3, 3, 64}, {"D 20 kHz 16-QAM short", mk(4, 6, 1, 1, 0), 3, 5, 16},
    };
    for (const C& c : cs) jobs.add([=] {
        RunOpt o; o.secs = c.s.shortIl ? 12 : 16;
        const Result r = runRx(synthGen(c.s, 48000), 48000, o);
        report(c.name, r);
        const DrmTelemetry& t = r.tel;
        CHECK(t.state == 2 && t.dataValid, "%s: not decoding (state %d)", c.name, t.state);
        CHECK(t.mode == c.mode && t.occupancy == c.occ && t.mscQam == c.qam, "%s: mode %d occupancy %d %d-QAM", c.name, t.mode, t.occupancy, t.mscQam);
        CHECK(t.services.size() == 1 && t.services[0].label == "OnAir DRM", "%s: label", c.name);
        CHECK(r.exact >= 6 && r.bad == 0 && r.gaps == 0, "%s: %llu exact, %llu bad, %llu gaps", c.name, (unsigned long long)r.exact, (unsigned long long)r.bad, (unsigned long long)r.gaps);
        CHECK(t.textMessage == "OnAir DRM test signal" || r.exact < 12, "%s: text '%s'", c.name, t.textMessage.c_str());
    });
}

// input rates from 48 kHz up to 20 Msps, integer and not
static void testRates() {
    const double rates[] = {48000, 96000, 192000, 240000, 1000000, 2000000, 2400000, 3200000, 4000000, 6000000, 8000000, 10000000, 12500000, 16000000, 20000000};
    for (double rate : rates) jobs.add([=] {
        RunOpt o; o.secs = rate > 5e6 ? 12 : 14;
        Sig s; s.snrDb = 30;
        const Result r = runRx(synthGen(s, rate), rate, o);
        char what[64];
        snprintf(what, sizeof what, "B 10 kHz at %.3f Msps", rate / 1e6);
        report(what, r);
        checkDefault(what, r, 1, 3, 64);
    });
}

// chunks of every size: the receiver must give the same result
static void testChunks() {
    const size_t chunks[] = {1, 7, 1000, 4096, 65536};
    for (size_t chunk : chunks) jobs.add([=] {
        RunOpt o; o.secs = chunk == 1 ? 10 : 12; o.chunk = chunk;
        Sig s; s.snrDb = 30;
        const Result r = runRx(synthGen(s, 48000), 48000, o);
        char what[64];
        snprintf(what, sizeof what, "48 kHz, chunks of %zu", chunk);
        report(what, r);
        CHECK(r.tel.state == 2 && r.tel.dataValid, "%s: not decoding", what);
        CHECK(r.exact >= 6 && r.bad == 0 && r.gaps == 0, "%s: %llu exact %llu bad %llu gaps", what, (unsigned long long)r.exact, (unsigned long long)r.bad, (unsigned long long)r.gaps);
    });
    // the same at 2 Msps: odd sizes must not change what comes out
    for (size_t chunk : {(size_t)7, (size_t)4099, (size_t)65536}) jobs.add([=] {
        RunOpt o; o.secs = 11; o.chunk = chunk;
        Sig s; s.snrDb = 30;
        const Result r = runRx(synthGen(s, 2e6), 2e6, o);
        char what[64];
        snprintf(what, sizeof what, "2 Msps, chunks of %zu", chunk);
        report(what, r);
        CHECK(r.tel.state == 2 && r.tel.dataValid && r.exact >= 6 && r.bad == 0 && r.gaps == 0, "%s: not clean", what);
    });
}

int main() {
    testRoundTrip();
    testModes();
    testRates();
    testChunks();
    jobs.run();
    if (fails) { jprintf("%d check(s) failed\n", fails.load()); return 1; }
    jprintf("OK\n");
    return 0;
}
