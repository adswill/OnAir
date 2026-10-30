// LoRa demodulator of the Signal Lab (loraraw_lab.h): fixed settings (SF, bandwidth, coding rate, sync word, header mode, CRC, LDRO, inverted IQ,
// offset) on frames made by the LoRa (raw) test generator, and the automatic search for SF, bandwidth, coding rate and sync word.
#include "dect2/loraraw/loraraw_gen.h"
#include "dect2/loraraw/loraraw_lab.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::lora;
#include "../common/test_util.h"

struct Case { int sf; double bw; int cr; int sync; bool crc; bool implicitHdr; bool ldro; };

static std::vector<cf32> make(const Case& c, const std::string& text, double rate, double offsetHz, double snrDb, bool invertIq, int nFrames = 2) {
    LoraRawGenConfig g;
    g.rate = rate; g.dialOffsetHz = offsetHz; g.snrDb = snrDb; g.loop = false; g.leadSec = 0.05; g.seed = 3;
    g.amp = 0.3;
    for (int i = 0; i < nFrames; i++) {
        LoraRawGenFrame f;
        f.p.sf = c.sf; f.p.bwHz = c.bw; f.p.cr = c.cr; f.p.syncWord = (uint8_t)c.sync; f.p.crc = c.crc; f.p.implicitHdr = c.implicitHdr;
        f.p.ldro = c.ldro; f.p.preamble = 8;
        f.payload.assign(text.begin(), text.end());
        f.payload.push_back((uint8_t)('0' + i));
        f.p.implicitLen = (int)f.payload.size();
        f.snrOffDb = 0;
        f.gapSec = 0.4;
        g.frames.push_back(f);
    }
    double dur = g.leadSec + 0.2;
    for (const auto& f : g.frames) dur += airSeconds(f.p, f.payload.size()) + f.gapSec;
    LoraRawGenerator gen(g);
    const size_t n = (size_t)(dur * rate);
    std::vector<cf32> x(n);
    gen.generate(x.data(), n);
    if (invertIq) for (auto& v : x) v = std::conj(v);
    return x;
}

static int goodFrames(const LabResult& r, const std::string& text) {
    int k = 0;
    for (const LabFrame& f : r.frames) {
        const std::string s(f.payload.begin(), f.payload.end());
        if (s.compare(0, text.size(), text) == 0 && f.crcOk) k++;
    }
    return k;
}

int main() {
    const std::string tx = "OnAir LoRa lab";
    // fixed settings
    const struct { Case c; double rate; double off; bool inv; const char* name; } fixed[] = {
        {{7, 125e3, 5, 0x12, true, false, false}, 500e3, 0, false, "SF7 125k 4/5"},
        {{9, 250e3, 7, 0x34, true, false, false}, 500e3, 15e3, false, "SF9 250k 4/7 sync 34 offset"},
        {{8, 500e3, 8, 0x2B, true, false, false}, 1e6, 0, false, "SF8 500k 4/8 sync 2B"},
        {{8, 62.5e3, 6, 0x12, true, false, false}, 250e3, 0, false, "SF8 62.5k 4/6"},
        {{10, 125e3, 5, 0x12, true, false, false}, 500e3, 0, true, "SF10 125k inverted IQ"},
        {{11, 125e3, 5, 0x12, true, false, true}, 250e3, 0, false, "SF11 125k LDRO"},
        {{8, 125e3, 6, 0x12, true, true, false}, 250e3, 0, false, "SF8 125k implicit header 4/6"},
    };
    for (const auto& f : fixed) {
        const std::vector<cf32> x = make(f.c, tx, f.rate, f.off, 30, f.inv);
        LabSettings s;
        s.sf = f.c.sf; s.bwHz = f.c.bw; s.cr = f.c.cr; s.syncWord = f.c.sync; s.crc = f.c.crc; s.implicitHdr = f.c.implicitHdr;
        s.implicitLen = (int)tx.size() + 1; s.ldro = f.c.ldro ? 1 : -1; s.invertIq = f.inv; s.offsetHz = f.off; s.preamble = 8;
        const LabResult r = labDemodulate(x.data(), x.size(), f.rate, s);
        CHECK(goodFrames(r, tx) == 2, "%s: %d of 2 frames (%zu decoded, %llu preambles)", f.name, goodFrames(r, tx), r.frames.size(), (unsigned long long)r.stats.preambles);
        if (!f.c.implicitHdr && !r.frames.empty()) CHECK(r.frames[0].cr == f.c.cr, "%s: coding rate %d", f.name, r.frames[0].cr);
    }
    {   // the wrong sync word finds nothing; a wrong IQ sense finds nothing
        const std::vector<cf32> x = make({7, 125e3, 5, 0x12, true, false, false}, tx, 500e3, 0, 30, false);
        LabSettings s; s.sf = 7; s.bwHz = 125e3; s.syncWord = 0x34;
        CHECK(labDemodulate(x.data(), x.size(), 500e3, s).frames.empty(), "wrong sync word gives no frame");
        s.syncWord = 0x12; s.invertIq = true;
        CHECK(labDemodulate(x.data(), x.size(), 500e3, s).frames.empty(), "wrong IQ sense gives no frame");
    }
    // automatic search
    const struct { Case c; double rate; double off; bool inv; double snr; const char* name; } autos[] = {
        {{7, 125e3, 5, 0x12, true, false, false}, 500e3, 0, false, 30, "SF7 125k"},
        {{9, 250e3, 7, 0x34, true, false, false}, 500e3, 20e3, false, 30, "SF9 250k 4/7 sync 34 offset 20k"},
        {{10, 125e3, 8, 0x2B, true, false, false}, 500e3, -30e3, true, 30, "SF10 125k 4/8 sync 2B inverted"},
        {{8, 500e3, 6, 0x12, false, false, false}, 1e6, 0, false, 30, "SF8 500k 4/6 no CRC"},
        {{9, 62.5e3, 5, 0x12, true, false, false}, 250e3, 0, false, 30, "SF9 62.5k"},
    };
    for (const auto& a : autos) {
        const std::vector<cf32> x = make(a.c, tx, a.rate, a.off, a.snr, a.inv);
        const LabDetect d = labAutoDetect(x.data(), x.size(), a.rate);
        CHECK(d.found, "auto %s: not found (%s)", a.name, d.note.c_str());
        if (!d.found) continue;
        CHECK(d.settings.sf == a.c.sf && std::fabs(d.settings.bwHz - a.c.bw) < 1 && d.settings.cr == a.c.cr && d.settings.syncWord == a.c.sync &&
              d.settings.crc == a.c.crc && d.settings.invertIq == a.inv,
              "auto %s: found SF%d %.0f 4/%d sync %02X crc %d inv %d", a.name, d.settings.sf, d.settings.bwHz, d.settings.cr, d.settings.syncWord, d.settings.crc, d.settings.invertIq);
        // and the settings it found decode the frames
        const LabResult r = labDemodulate(x.data(), x.size(), a.rate, d.settings);
        CHECK(goodFrames(r, tx) == 2 || (!a.c.crc && (int)r.frames.size() == 2), "auto %s: settings decode %d frames", a.name, (int)r.frames.size());
    }
    {   // noise alone
        std::vector<cf32> x(500000);
        unsigned s = 1;
        for (auto& v : x) { s = s * 1664525u + 1013904223u; const float a = ((s >> 8) & 0xFFFF) / 32768.f - 1; s = s * 1664525u + 1013904223u; v = {a * 0.1f, (((s >> 8) & 0xFFFF) / 32768.f - 1) * 0.1f}; }
        CHECK(!labAutoDetect(x.data(), x.size(), 500e3).found, "noise alone: nothing found");
    }
    return testSummary("loraraw_lab");
}
