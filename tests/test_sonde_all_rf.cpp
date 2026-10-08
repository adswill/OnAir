// All types at once: the default three sondes, then six (two RS41, two DFM, M10, M20), then seven with an RS92, in one band.
// Every sonde must be found on its own channel, with the position and speed of the test flight.
#include "dect2/sonde_testkit.h"
using namespace dect2;
using namespace dect2::sondetest;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Want { const char* serial; const char* type; double alt0, vv, lat; bool pos; };

static void expect(const Result& r, const Want* w, int n, const char* what) {
    for (int i = 0; i < n; i++) {
        const SondeInfo* s = find(r.tel, w[i].serial);
        CHECK(s != nullptr, "%s: %s %s not found", what, w[i].type, w[i].serial);
        if (!s) continue;
        CHECK(s->type == w[i].type, "%s: type %s", what, s->type.c_str());
        CHECK(s->framesOk >= (s->type == "DFM" ? 4u : 6u) && s->framesBad <= 2, "%s: %s: %llu ok %llu bad", what, w[i].serial, (unsigned long long)s->framesOk, (unsigned long long)s->framesBad);
        if (w[i].pos) {
            CHECK(s->hasPos && std::fabs(s->lat - w[i].lat) < 0.01 && s->hasVel && std::fabs(s->vSpeed - w[i].vv) < 0.3 && std::fabs(s->hSpeed - 10.0) < 0.3,
                  "%s: %s: position/velocity %.4f %.2f %.2f", what, w[i].serial, s->lat, s->vSpeed, s->hSpeed);
            int bad = 0;
            for (const auto& tp : s->track) if (std::fabs(tp.altM - (w[i].alt0 + w[i].vv * (tp.unixT - 1780272000.0))) > 10.0) bad++;
            // only the 5 most recently heard sondes carry their track in a report
            if (s->trackIncluded) CHECK(bad == 0 && s->track.size() >= 3, "%s: %s: %d track points off of %zu", what, w[i].serial, bad, s->track.size());
            else CHECK(s->track.empty(), "%s: %s: track without trackIncluded", what, w[i].serial);
        }
    }
}

int main() {
    const Want w3[3] = {{"N4750123", "RS41", 12000, 5, 25.20, true}, {"17012345", "DFM", 22000, -15, 25.30, true}, {"310-2-11329", "M10", 8000, 5, 25.10, true}};
    {
        SynthConfig c; c.mode = 15; c.snrDb = 28; c.cfoHz = 2500;
        Result r = run(c, 8e6, 14);
        expect(r, w3, 3, "three sondes");
        CHECK(r.tel.channelsUsed == 3 && r.tel.sondes.size() == 3, "channels %d sondes %zu", r.tel.channelsUsed, r.tel.sondes.size());
        CHECK(r.tel.state == 2 && r.tel.dataValid && r.tel.carriers.size() == 3, "state %d carriers %zu", r.tel.state, r.tel.carriers.size());
        for (const auto& s : r.tel.sondes) CHECK(s.channel >= 0 && s.active && std::fabs(s.freqHz - (403.0e6 + 2500 + (s.type == "RS41" ? 0 : s.type == "DFM" ? -800e3 : 1100e3))) < 800.0, "%s frequency %.0f", s.serial.c_str(), s.freqHz);
        printf("three sondes: rtf %.0f, frames ok %llu bad %llu\n", r.rtf, (unsigned long long)r.tel.blocksOk, (unsigned long long)r.tel.blocksBad);
        if (!kSanitized) CHECK(r.rtf > 3.0, "real-time factor %.1f", r.rtf);
    }
    {
        const Want w6[6] = {{"N4750123", "RS41", 12000, 5, 25.20, true}, {"17012345", "DFM", 22000, -15, 25.30, true}, {"310-2-11329", "M10", 8000, 5, 25.10, true},
                            {"N4750456", "RS41", 5000, 5, 24.90, true}, {"211-4-01234", "M20", 15000, 5, 25.00, true}, {"19076543", "DFM", 3000, 4, 25.40, true}};
        SynthConfig c; c.mode = 15; c.snrDb = 28; c.modeOpt[0] = 15; c.modeOpt[1] = 6;
        Result r = run(c, 8e6, 16);
        expect(r, w6, 6, "six sondes");
        CHECK(r.tel.channelsUsed == 6, "channels %d", r.tel.channelsUsed);
        { int withTrack = 0; for (const auto& s : r.tel.sondes) withTrack += s.trackIncluded ? 1 : 0; CHECK(withTrack == 5, "%d sondes carry a track", withTrack); }
        printf("six sondes: rtf %.0f, frames ok %llu bad %llu\n", r.rtf, (unsigned long long)r.tel.blocksOk, (unsigned long long)r.tel.blocksBad);
        if (!kSanitized) CHECK(r.rtf > 3.0, "real-time factor %.1f", r.rtf);
    }
    {
        SynthConfig c; c.mode = 15; c.snrDb = 28; c.modeOpt[0] = 31; c.modeOpt[1] = 7;
        Result r = run(c, 10e6, 14);
        const Want w7[2] = {{"N4750123", "RS41", 12000, 5, 25.20, true}, {"P4953934", "RS92", 0, 0, 0, false}};
        expect(r, w7, 2, "seven sondes");
        CHECK(r.tel.channelsUsed == 7 && r.tel.sondes.size() == 7, "channels %d sondes %zu", r.tel.channelsUsed, r.tel.sondes.size());
        printf("seven sondes: rtf %.0f, frames ok %llu bad %llu\n", r.rtf, (unsigned long long)r.tel.blocksOk, (unsigned long long)r.tel.blocksBad);
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
