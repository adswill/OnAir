// LR-FHSS: the test signal (lrfhss_gen.h) through the receiver (lrfhss_rx.h). Every LoRaWAN LR-FHSS data rate without noise (EU868 DR8 - DR11,
// US915 DR5 - DR6), then noise down to the sensitivity, carrier offset and transmitter clock error, two frames overlapping in time, noise
// alone. Every field of the LoRaWAN frame must come back (lorawan::parse). Synthetic tests only; not checked against a real transmitter.
// LRFHSS_SWEEP=1 prints the decode rate against Eb/N0 for every data rate (slower).
#include "dect2/lorawan/lorawan_plan.h"
#include "dect2/lrfhss/lrfhss_gen.h"
#include "dect2/lrfhss/lrfhss_rx.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;

static int fails = 0;
static void fail(const char* fmt, ...) {
    std::string b(600, '\0');
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(&b[0], b.size(), fmt, ap);
    va_end(ap);
    printf("FAIL: %s\n", b.c_str());
    fails++;
}

static std::vector<uint8_t> uplink(uint32_t addr, uint16_t fcnt, int port, size_t frmLen, uint32_t mic) {
    lorawan::Build b;
    b.mtype = 2; b.devAddr = addr; b.fcnt = fcnt; b.fport = port; b.adr = true; b.mic = mic;
    for (size_t i = 0; i < frmLen; i++) b.frm.push_back((uint8_t)(i * 29 + addr));
    return lorawan::build(b);
}

static LrfhssGenFrame frameFor(int plan, int dr, double mhz, int seq, int dev, const std::vector<uint8_t>& pl) {
    LrfhssGenFrame f;
    for (const auto& r : lorawan::planLrfhssRates(plan))
        if (r.dr == dr) { f.p.cr = r.cr; f.p.bw = r.bw; f.p.grid = r.grid; f.p.headers = r.headers; }
    f.mhz = mhz; f.hopSeqId = seq; f.deviceOffset = dev; f.payload = pl;
    return f;
}

static std::vector<lrfhss::RxFrame> run(const LrfhssGenConfig& c, double secs, lrfhss::RxStats* st = nullptr) {
    LrfhssGenerator g(c);
    lrfhss::Receiver r;
    r.configure(c.rate);
    std::vector<cf32> buf(16384);
    std::vector<lrfhss::RxFrame> out;
    const size_t total = (size_t)((secs + 0.5) * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size(), out); }
    if (st) *st = r.stats();
    return out;
}

static const lrfhss::RxFrame* find(const std::vector<lrfhss::RxFrame>& v, const std::vector<uint8_t>& pl) {
    for (const auto& f : v) if (f.crcOk && f.payload == pl) return &f;
    return nullptr;
}

// every LoRaWAN field of a decoded uplink against what was sent
static bool sameFrame(const lrfhss::RxFrame& f, uint32_t addr, uint16_t fcnt, int port, size_t frmLen, uint32_t mic) {
    const lorawan::Frame m = lorawan::parse(f.payload.data(), f.payload.size());
    return m.ok && m.mtype == 2 && m.uplink && m.devAddr == addr && m.fcnt == fcnt && m.fport == port && m.frmLen == frmLen && m.mic == mic && m.adr;
}

int main() {
    // ---- every data rate, no noise
    struct Case { int plan, dr; double rate, mhz; int seq, dev; };
    const Case cases[] = {
        {lorawan::EU868, 8, 1e6, 868.1, 17, 0},  {lorawan::EU868, 9, 1e6, 868.5, 300, -3}, {lorawan::EU868, 10, 1e6, 868.3, 5, 2},
        {lorawan::EU868, 11, 4e6, 868.3, 511, -4}, {lorawan::US915, 5, 4e6, 904.6, 100, 7}, {lorawan::US915, 6, 4e6, 903.0, 383, -20},
    };
    for (const Case& k : cases) {
        LrfhssGenConfig c;
        c.rate = k.rate; c.dialMhz = k.plan == lorawan::EU868 ? 868.3 : 904.2; c.ebN0Db = 100; c.seed = (uint32_t)k.seq;
        const uint32_t addr = 0x26010000u + (uint32_t)k.dr;
        const auto pl = uplink(addr, (uint16_t)(1000 + k.dr), k.dr, 12, 0xC0DE0000u + (uint32_t)k.dr);
        c.frames.push_back(frameFor(k.plan, k.dr, k.mhz, k.seq, k.dev, pl));
        lrfhss::RxStats st;
        const auto out = run(c, lrfhssSceneSeconds(c.frames), &st);
        const lrfhss::RxFrame* f = find(out, pl);
        printf("  %s DR%d: %zu frames, %llu candidates, %llu syncs, %llu headers%s\n", lorawan::planName(k.plan), k.dr, out.size(), (unsigned long long)st.candidates,
               (unsigned long long)st.syncs, (unsigned long long)st.headers, f ? "" : " (not decoded)");
        if (!f) { fail("%s DR%d without noise: not decoded", lorawan::planName(k.plan), k.dr); continue; }
        if (!sameFrame(*f, addr, (uint16_t)(1000 + k.dr), k.dr, 12, 0xC0DE0000u + (uint32_t)k.dr)) fail("DR%d: LoRaWAN fields differ", k.dr);
        const auto& want = c.frames[0];
        if (f->hdr.hopSeqId != k.seq || f->hdr.cr != want.p.cr || f->hdr.bw != want.p.bw || f->hdr.grid != want.p.grid || f->hdr.len != (int)pl.size())
            fail("DR%d: header fields seq %d cr %d bw %d grid %d len %d", k.dr, f->hdr.hopSeqId, f->hdr.cr, f->hdr.bw, f->hdr.grid, f->hdr.len);
        if (lorawan::lrfhssDataRate(k.plan, f->hdr.cr, f->hdr.bw, f->hdr.grid) != k.dr) fail("DR%d: data rate lookup", k.dr);
        // the OCW centre within the device offset (+-4 / +-26 steps of 488 Hz)
        const double centre = (k.mhz - c.dialMhz) * 1e6;
        const double tol = (want.p.grid == lrfhss::Grid3906 ? 5 : 27) * lrfhss::kStepHz;
        if (std::fabs(f->centreHz - centre) > tol) fail("DR%d: OCW centre %.0f Hz, want %.0f", k.dr, f->centreHz, centre);
        if (f->hopsHeard != f->hops || f->bitErrors != 0) fail("DR%d: %d of %d hops heard, %d bit errors without noise", k.dr, f->hopsHeard, f->hops, f->bitErrors);
        if (std::fabs(f->sec - c.leadSec) > 0.01) fail("DR%d: start %.3f s, want %.3f", k.dr, f->sec, c.leadSec);
    }
    // ---- noise, carrier offset, clock error, two frames overlapping in time (different hop sequences, the same OCW)
    {
        LrfhssGenConfig c;
        c.rate = 1e6; c.dialMhz = 868.3; c.ebN0Db = 12; c.cfoHz = 9000; c.sroPpm = 20; c.seed = 5;
        const auto a = uplink(0x260B0001, 7, 1, 20, 0x11111111), b = uplink(0x260B0002, 8, 2, 10, 0x22222222);
        LrfhssGenFrame fa = frameFor(lorawan::EU868, 8, 868.1, 40, 1, a), fb = frameFor(lorawan::EU868, 9, 868.1, 200, -2, b);
        fa.startSec = 0.05; fb.startSec = 0.3; fb.snrOffDb = -3; fb.cfoHz = -2500;
        c.frames = {fa, fb};
        const auto out = run(c, lrfhssSceneSeconds(c.frames));
        const lrfhss::RxFrame* ra = find(out, a);
        const lrfhss::RxFrame* rb = find(out, b);
        if (!ra || !sameFrame(*ra, 0x260B0001, 7, 1, 20, 0x11111111)) fail("overlapping frames: the DR8 frame (Eb/N0 12 dB, +9 kHz, 20 ppm) not decoded");
        if (!rb || !sameFrame(*rb, 0x260B0002, 8, 2, 10, 0x22222222)) fail("overlapping frames: the DR9 frame (Eb/N0 9 dB, +6.5 kHz) not decoded");
        if (ra) printf("  overlap: DR8 Eb/N0 estimate %.1f dB (12 sent), %d of %d hops heard, %d bit errors\n", ra->ebN0Db, ra->hopsHeard, ra->hops, ra->bitErrors);
        if (ra && std::fabs(ra->ebN0Db - 12) > 3) fail("Eb/N0 estimate %.1f dB for 12 dB", ra->ebN0Db);
    }
    // ---- sensitivity: frames decoded out of 8 at a low Eb/N0 (CR 1/3 and CR 2/3)
    {
        const bool sweep = std::getenv("LRFHSS_SWEEP") != nullptr;
        struct Sens { int dr; double ebn0; int need; };
        std::vector<Sens> pts = {{8, 3, 7}, {9, 5, 7}};
        if (sweep) pts = {{8, 0, 0}, {8, 1, 0}, {8, 2, 0}, {8, 3, 0}, {8, 4, 0}, {9, 3, 0}, {9, 4, 0}, {9, 5, 0}, {9, 6, 0}, {9, 7, 0}};
        for (const Sens& s : pts) {
            LrfhssGenConfig c;
            c.rate = 250e3; c.dialMhz = 868.3; c.ebN0Db = s.ebn0; c.cfoHz = -3000; c.seed = (uint32_t)(s.dr * 100 + (int)s.ebn0);
            std::vector<std::vector<uint8_t>> sent;
            for (int i = 0; i < 8; i++) {
                sent.push_back(uplink(0x26020000u + (uint32_t)i, (uint16_t)i, 1, 16, 0xABCD0000u + (uint32_t)i));
                LrfhssGenFrame f = frameFor(lorawan::EU868, s.dr, 868.3 + 0.03 * (i % 3 - 1), 37 * i + 3, i % 8 - 4, sent.back());
                f.gapSec = 0.1;
                c.frames.push_back(f);
            }
            lrfhss::RxStats st;
            const auto out = run(c, lrfhssSceneSeconds(c.frames), &st);
            int got = 0;
            for (const auto& p : sent) got += find(out, p) != nullptr;
            printf("  DR%d at Eb/N0 %.0f dB: %d of 8 (%llu candidates, %llu syncs, %llu headers, %llu CRC errors)\n", s.dr, s.ebn0, got, (unsigned long long)st.candidates,
                   (unsigned long long)st.syncs, (unsigned long long)st.headers, (unsigned long long)st.crcBad);
            if (got < s.need) fail("DR%d at Eb/N0 %.0f dB: %d of 8 frames (want %d)", s.dr, s.ebn0, got, s.need);
        }
    }
    // ---- noise alone: no frame
    {
        LrfhssGenConfig c;
        c.rate = 1e6; c.ebN0Db = 10;
        c.frames.clear();
        lrfhss::RxStats st;
        const auto out = run(c, 3.0, &st);
        size_t good = 0;
        for (const auto& f : out) good += f.crcOk;
        if (good) fail("noise alone: %zu frames", good);
        printf("  noise alone: %llu candidates, %llu syncs, %llu headers, %zu frames\n", (unsigned long long)st.candidates, (unsigned long long)st.syncs,
               (unsigned long long)st.headers, out.size());
    }
    if (fails) printf("lrfhss rx: %d FAILED\n", fails); else printf("%s\n", "lrfhss rx: all passed");
    return fails ? 1 : 0;
}
