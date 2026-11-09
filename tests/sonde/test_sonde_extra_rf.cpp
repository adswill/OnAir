// iMet, iMet-54, MRZ and iMS-100 through the whole receiver: serial, position, carrier offset, noise. Simulated signals only.
#include "dect2/sonde/sonde_testkit.h"
using namespace dect2;
using namespace dect2::sondetest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct T { int mask; const char* type; const char* serial; double offHz; };

int main() {
    const T all[] = {
        {64, "iMet", nullptr, -0.3e6},
        {128, "iMet-54", "iMet54-100123", 1.7e6},
        {256, "MRZ", "MRZ-11223-4455", 0.8e6},
        {512, "iMS-100", "12345678", -1.7e6},
    };
    for (const auto& t : all) {
        printf("%s\n", t.type);
        for (double cfo : {0.0, 20000.0, -20000.0}) {
            SynthConfig c; c.mode = 15; c.snrDb = 30; c.modeOpt[0] = t.mask; c.modeOpt[1] = 1; c.cfoHz = cfo;
            Result r = run(c, 8e6, 16);
            const SondeInfo* s = t.serial ? find(r.tel, t.serial) : (r.tel.sondes.empty() ? nullptr : &r.tel.sondes[0]);
            CHECK(s != nullptr, "%s cfo %.0f: nothing decoded", t.type, cfo);
            if (!s) continue;
            CHECK(s->framesOk >= 2 && s->framesBad == 0, "%s cfo %.0f: ok %llu bad %llu", t.type, cfo, (unsigned long long)s->framesOk, (unsigned long long)s->framesBad);
            CHECK(s->type == t.type, "%s: type %s", t.type, s->type.c_str());
            if (s->hasPos) CHECK(std::fabs(s->lat - 25) < 1.5, "%s: lat %.3f", t.type, s->lat);
            else CHECK(false, "%s cfo %.0f: no position", t.type, cfo);
            CHECK(std::fabs(s->offsetHz - (t.offHz + cfo)) < 2500.0, "%s offset %.0f", t.type, s->offsetHz);   // 8N1 is not balanced: its mean offsets the AFC
        }
        for (double snr : {22.0, 18.0}) {
            SynthConfig c; c.mode = 15; c.snrDb = snr; c.modeOpt[0] = t.mask; c.modeOpt[1] = 1;
            Result r = run(c, 8e6, 16);
            const SondeInfo* s = t.serial ? find(r.tel, t.serial) : (r.tel.sondes.empty() ? nullptr : &r.tel.sondes[0]);
            printf("  snr %.0f: ok %llu\n", snr, (unsigned long long)(s ? s->framesOk : 0));
            CHECK(s && s->framesOk >= 1, "%s snr %.0f", t.type, snr);
        }
    }
    return fails ? 1 : 0;
}
