// RS41: two sondes at once (far apart and 20 kHz apart), a carrier that drifts 2 kHz in a minute.
#include "dect2/sonde_testkit.h"
using namespace dect2;
using namespace dect2::sondetest;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    for (double spacing : {450000.0, 100000.0, 40000.0, 20000.0}) {
        SynthConfig c; c.mode = 15; c.snrDb = 30; c.modeOpt[0] = 1; c.modeOpt[1] = 2; c.modeVal[2] = spacing;
        Result r = run(c, 8e6, 10);
        const SondeInfo* a = find(r.tel, "N4750123");
        const SondeInfo* b = find(r.tel, "N4750456");
        CHECK(a && b, "spacing %.0f: found %d %d", spacing, a != nullptr, b != nullptr);
        if (!a || !b) continue;
        printf("spacing %6.0f Hz: ok %llu / %llu, bad %llu / %llu, offsets %.0f %.0f Hz\n", spacing, (unsigned long long)a->framesOk, (unsigned long long)b->framesOk,
               (unsigned long long)a->framesBad, (unsigned long long)b->framesBad, a->offsetHz, b->offsetHz);
        CHECK(a->framesOk >= 7 && b->framesOk >= 7, "spacing %.0f: frames %llu %llu", spacing, (unsigned long long)a->framesOk, (unsigned long long)b->framesOk);
        CHECK(a->framesBad <= 1 && b->framesBad <= 1, "spacing %.0f: bad frames %llu %llu", spacing, (unsigned long long)a->framesBad, (unsigned long long)b->framesBad);
        CHECK(std::fabs((b->offsetHz - a->offsetHz) - spacing) < 600, "spacing %.0f: measured %.0f", spacing, b->offsetHz - a->offsetHz);
        CHECK(r.tel.channelsUsed == 2, "channels %d", r.tel.channelsUsed);
    }
    { // carrier drifts +2 kHz in 60 s
        SynthConfig c; c.mode = 15; c.snrDb = 25; c.modeOpt[0] = 1; c.modeOpt[1] = 1; c.modeVal[1] = 2000.0;
        Result r = run(c, 2e6, 66);
        const SondeInfo* a = find(r.tel, "N4750123");
        CHECK(a != nullptr, "drift: not found");
        if (a) {
            printf("drift: ok %llu bad %llu, offset at the end %.0f Hz (expected %.0f)\n", (unsigned long long)a->framesOk, (unsigned long long)a->framesBad, a->offsetHz, 2000.0 * 66 / 60);
            CHECK(a->framesOk >= 62, "drift: frames ok %llu", (unsigned long long)a->framesOk);
            CHECK(std::fabs(a->offsetHz - 2200.0) < 400.0, "drift: offset %.0f", a->offsetHz);
            CHECK(a->calDone == 51 && a->hasTemp && a->hasHumidity, "calibration complete: %d temp %d", a->calDone, (int)a->hasTemp);
            CHECK(a->hasTemp && std::fabs(a->tempC - (-56.5)) < 0.3, "temperature %.2f", a->tempC);
            CHECK(a->hasHumidity && std::fabs(a->humidity - 3.0) < 0.2, "humidity %.2f", a->humidity);
            CHECK(a->trackIncluded && a->track.size() > 20 && a->track.size() <= 600, "track of %zu points", a->track.size());
            if (a->track.size() > 20) {
                CHECK(a->track.front().tempC < -900 && std::fabs(a->track.back().tempC + 56.5) < 0.3, "track temperature %.2f ... %.2f", a->track.front().tempC, a->track.back().tempC);
                CHECK(a->track.back().humidity >= 0 && a->track.back().humidity < 10, "track humidity %.2f", a->track.back().humidity);
            }
        }
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
