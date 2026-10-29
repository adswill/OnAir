// LR-FHSS inside the LoRaWAN (all channels) mode: the LoRaWAN test signal with its LR-FHSS frames (lorawanLrfhssSchedule, modeOpt[1] = 0)
// through LorawanReceiver at 4 Msps. The LR-FHSS frames are in the same table as the LoRa ones, marked with their data rate and grid, with every
// LoRaWAN field; the LoRa frames still all come; switching LR-FHSS off removes them; the US915 scene too. Synthetic tests only; not checked
// against a real transmitter.
#include "dect2/lorawan/lorawan_gen.h"
#include "dect2/lorawan/lorawan_plan.h"
#include "dect2/lorawan/lorawan_rx.h"
#include "dect2/lrfhss/lrfhss.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
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

static LorawanGenConfig scene(int plan, double snr, bool lr) {
    LorawanGenConfig c;
    c.rate = 4e6;
    c.dialMhz = lorawanPlanCentreMhz(plan);
    c.dialOffsetHz = -lorawanTuning().tuneOffsetHz;
    c.snrDb = snr; c.loop = false;
    c.frames = lorawanTestSchedule(plan);
    if (lr) c.lrfhss = lorawanLrfhssSchedule(plan);
    return c;
}

static LorawanTelemetry run(const LorawanGenConfig& c, const LorawanSettings& s, double secs) {
    LorawanGenerator g(c);
    LorawanReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(c.dialOffsetHz);
    r.setSettings(s);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)((secs + 0.6) * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    LorawanTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static double sceneEnd(const LorawanGenConfig& c) {
    double e = lorawanSceneSeconds(c.frames);
    for (const auto& f : c.lrfhss) e = std::max(e, f.startSec + lrfhssFrameSeconds(f) + 0.2);
    return e;
}

int main() {
    for (int plan : {(int)lorawan::EU868, (int)lorawan::US915}) {
        LorawanGenConfig c = scene(plan, 20, true);
        LorawanSettings s;
        s.plan = plan;
        const LorawanTelemetry t = run(c, s, sceneEnd(c));
        printf("  %s: %s; LR-FHSS %llu frames, %llu headers, %llu CRC errors, %llu candidates; note: %s\n", lorawan::planName(plan), lorawanSummary(t).c_str(),
               (unsigned long long)t.lrFrames, (unsigned long long)t.lrHeaders, (unsigned long long)t.lrCrcBad, (unsigned long long)t.lrCandidates, t.note.c_str());
        if (!t.lrfhssOn) fail("%s: the LR-FHSS receiver is off", lorawan::planName(plan));
        // every LR-FHSS frame of the scene, with its LoRaWAN fields and its data rate
        for (const LrfhssGenFrame& g : c.lrfhss) {
            const lorawan::Frame want = lorawan::parse(g.payload.data(), g.payload.size());
            const LwFrame* got = nullptr;
            for (const auto& f : t.log) if (f.lrfhss && f.hasDevAddr && f.devAddr == want.devAddr) got = &f;
            int dr = -1;
            for (const auto& r : lorawan::planLrfhssRates(plan)) if (r.cr == g.p.cr && r.bw == g.p.bw && r.grid == g.p.grid && r.headers == g.p.headers) dr = r.dr;
            if (!got) { fail("%s: LR-FHSS DR%d frame %08X missing", lorawan::planName(plan), dr, want.devAddr); continue; }
            if (got->dr != dr || got->lrCr != g.p.cr || got->hopSeq != g.hopSeqId || got->sf != 0 || !got->crcOk || !got->lorawan)
                fail("%s: DR%d frame: dr %d cr %d seq %d", lorawan::planName(plan), dr, got->dr, got->lrCr, got->hopSeq);
            if (got->mtype != want.mtype || got->fcnt != want.fcnt || got->fport != want.fport || got->frmLen != (int)want.frmLen || got->mic != want.mic || got->flags != want.flags)
                fail("%s: DR%d frame %08X: LoRaWAN fields differ (type %d fcnt %u port %d len %d mic %08X flags %s)", lorawan::planName(plan), dr, want.devAddr, got->mtype,
                     got->fcnt, got->fport, got->frmLen, got->mic, got->flags.c_str());
            std::string wantMac;
            for (size_t i = 0; i < want.mac.size(); i++) wantMac += (i ? ", " : "") + want.mac[i].name + (want.mac[i].detail.empty() ? "" : " (" + want.mac[i].detail + ")");
            if (got->mac != wantMac) fail("DR%d MAC commands '%s', want '%s'", dr, got->mac.c_str(), wantMac.c_str());
            const double tol = (g.p.grid == lrfhss::Grid3906 ? 5 : 27) * lrfhss::kStepHz + 3000;   // device offset and the carrier offset of the frame
            if (std::fabs(got->freqHz - g.mhz * 1e6) > tol) fail("DR%d: OCW centre %.4f MHz, want %.4f", dr, got->freqHz / 1e6, g.mhz);
            if (std::fabs(got->ocwKhz - lrfhss::occupiedHz(g.p.bw) / 1e3) > 0.5 || std::fabs(got->gridKhz - lrfhss::gridHz(g.p.grid) / 1e3) > 0.01) fail("DR%d: OCW / grid", dr);
            if (got->snrDb < 3) fail("DR%d: Eb/N0 estimate %.1f dB (28 sent, less where a LoRa frame overlaps)", dr, got->snrDb);
        }
        // the LoRa frames are all still there
        size_t lora = 0;
        for (const auto& f : t.log) lora += !f.lrfhss && f.crcOk;
        size_t want = 0;
        for (const auto& f : c.frames) want += !f.inverted;
        if (lora < want) fail("%s: %zu LoRa uplinks of %zu", lorawan::planName(plan), lora, want);
        if (t.lrCrcBad != 0) fail("%s: %llu LR-FHSS CRC errors", lorawan::planName(plan), (unsigned long long)t.lrCrcBad);
        if (t.blocksBad != 0) fail("%s: %llu bad frames", lorawan::planName(plan), (unsigned long long)t.blocksBad);
    }
    {   // LR-FHSS switched off: no LR-FHSS frame, the LoRa ones stay
        LorawanGenConfig c = scene(lorawan::EU868, 20, true);
        LorawanSettings s;
        s.lrfhss = false;
        const LorawanTelemetry t = run(c, s, sceneEnd(c));
        size_t lr = 0, lora = 0;
        for (const auto& f : t.log) { lr += f.lrfhss; lora += !f.lrfhss && f.crcOk; }
        if (lr != 0 || t.lrfhssOn) fail("LR-FHSS off: %zu LR-FHSS frames", lr);
        if (lora < 7) fail("LR-FHSS off: %zu LoRa frames", lora);
    }
    {   // the LoRa-only scene gives no LR-FHSS frame (the LoRa chirps make no hops)
        LorawanGenConfig c = scene(lorawan::EU868, 30, false);
        LorawanSettings s;
        const LorawanTelemetry t = run(c, s, sceneEnd(c));
        size_t lr = 0;
        for (const auto& f : t.log) lr += f.lrfhss;
        printf("  LoRa only: %llu LR-FHSS candidates, %llu headers\n", (unsigned long long)t.lrCandidates, (unsigned long long)t.lrHeaders);
        if (lr != 0 || t.lrHeaders != 0) fail("LoRa only: %zu LR-FHSS frames, %llu headers", lr, (unsigned long long)t.lrHeaders);
    }
    if (fails) printf("lrfhss lorawan: %d FAILED\n", fails); else printf("%s\n", "lrfhss lorawan: all passed");
    return fails ? 1 : 0;
}
