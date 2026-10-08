// Radiosonde through the engine, as the GUI and dect2cli use it: the built-in synthetic source plays the default test signal (three sondes,
// SynthConfig::mode = 15) at the mode's sample rate, the receiver runs on its own thread, telemetry comes through latestRx. 20 s; a retune
// in the middle must reset the receiver without the sequence number going back; no samples may be dropped.
#include "dect2/engine.h"
#include "dect2/modes.h"
#include "dect2/sonde_bits.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const SondeInfo* byType(const SondeTelemetry& t, const char* type) {
    for (const auto& s : t.sondes) if (s.type == type) return &s;
    return nullptr;
}

int main() {
    const ModeTuning* mt = modeTuningById("sonde");
    CHECK(mt && mt->stdMode == 15 && modeTuning(15) == mt, "tuning table");
    if (!mt) return 1;
    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "synthetic";
    TuneSettings tune;
    tune.centerHz = mt->defMhz * 1e6;
    tune.sampleRate = mt->sampleRate;
    tune.bandwidthMhz = mt->bandwidthMhz;
    tune.basebandFilterHz = mt->basebandHz;
    tune.synth.mode = mt->stdMode;
    tune.synth.snrDb = 25;
    tune.synth.cfoHz = 800;
    FileOptions fo;
    e.setStandard(15);
    CHECK(e.start(dev, tune, fo), "engine started");
    CHECK(e.activeStandard() == 14, "active standard %d", e.activeStandard());
    e.sonde().setCenterMhz(mt->defMhz);
    RxTelemetry t, last;
    uint64_t seq = 0, lastSeq = 0;
    bool seqOk = true, retuned = false, decoded = false;
    size_t maxBytes = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 20.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (e.latestRx(t, seq)) {
            seq = t.seq;
            if (t.standard == 14) {
                if (t.sonde.seq <= lastSeq && lastSeq != 0) seqOk = false;
                lastSeq = t.sonde.seq;
                last = t;
                decoded |= t.dataValid;
                size_t bytes = sizeof(SondeTelemetry) + t.sonde.carriers.size() * sizeof(SondeCarrier);
                for (const auto& s : t.sonde.sondes) bytes += sizeof(SondeInfo) + s.track.size() * sizeof(SondeTrackPoint);
                maxBytes = std::max(maxBytes, bytes);
            }
        }
        SondeTelemetry direct;
        e.sonde().telemetry(direct, 0);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!retuned && el > 7.0) { CHECK(e.retuneReset(tune), "retune"); retuned = true; }
    }
    e.latestRx(t, 0);
    const uint64_t dropped = e.droppedSamples();
    e.stop();
    printf("%s\n", sondeSummary(last.sonde).c_str());
    printf("state %d, SNR %.1f dB, frames %llu ok / %llu bad, channels %d, dropped %llu samples, largest report %zu bytes\n", last.state, last.dataSnrDb,
           (unsigned long long)last.blocksOk, (unsigned long long)last.blocksBad, last.sonde.channelsUsed, (unsigned long long)dropped, maxBytes);
    for (const auto& s : last.sonde.sondes)
        printf("  %s %s  %.3f MHz  %.0f m  frames %llu/%llu  %s\n", s.type.c_str(), s.serial.c_str(), s.freqHz / 1e6, s.altM, (unsigned long long)s.framesOk, (unsigned long long)s.framesBad, s.note.c_str());
    CHECK(decoded && last.standard == 14 && last.rateOk, "decoded %d standard %d rateOk %d", (int)decoded, last.standard, (int)last.rateOk);
    CHECK(last.state == last.sonde.state && last.blocksOk == last.sonde.blocksOk, "summary fields");
    CHECK(last.state == 2 && last.dataValid, "state %d at the end", last.state);
    const SondeInfo* r41 = byType(last.sonde, "RS41");
    CHECK(r41 && r41->active && r41->framesOk >= 4 && r41->hasPos && std::fabs(r41->lat - 25.2) < 0.01, "RS41 after the retune");
    if (r41) CHECK(std::fabs(r41->freqHz - 403.0e6 - 800.0) < 700.0, "RS41 frequency %.0f", r41->freqHz);
    if (makeDfmDecoder()) { const SondeInfo* d = byType(last.sonde, "DFM"); CHECK(d && d->framesOk >= 4 && d->hasPos, "DFM"); }
    if (makeM10Decoder()) { const SondeInfo* m = byType(last.sonde, "M10"); CHECK(m && m->framesOk >= 4 && m->hasPos, "M10"); }
    CHECK(seqOk, "telemetry sequence went backwards");
    CHECK(dropped == 0, "%llu samples dropped", (unsigned long long)dropped);
    CHECK(maxBytes < 100000, "report of %zu bytes", maxBytes);
    CHECK(last.sonde.carriers.size() >= 1, "no carriers");
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
