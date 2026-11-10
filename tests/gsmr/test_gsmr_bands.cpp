// GSM (shared with GSM-R): the GSM 850 and PCS 1900 rasters, the SI2bis neighbour list, the CCCH message kinds (counted, never parsed),
// and the receiver counting the CCCH load of the test signal at two offsets and two noise levels. Simulated signals only.
#include "dect2/gsmr/gsmr_rx.h"
#include "dect2/gsmr/gsmr_si.h"
#include "dect2/gsmr/gsmr_tel.h"
#include "dect2/gsmr/gsmr_gen.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/source/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>
using namespace dect2;
using namespace dect2::gsmr;
#include "../common/test_util.h"

int main() {
    // rasters (3GPP TS 45.005 2)
    CHECK(gsmrHzToArfcn(869.2e6) == 128 && gsmrHzToArfcn(893.8e6) == 251 && std::fabs(gsmrArfcnToHz(190) - 881.6e6) < 1, "GSM 850");
    CHECK(gsmrHzToArfcn(1930.2e6) == 512 && gsmrHzToArfcn(1989.8e6) == 810 && std::fabs(gsmrRasterHz(1950.21e6) - 1950.2e6) < 1, "PCS 1900");
    CHECK(std::fabs(gsmrRasterHz(1822.83e6) - 1822.8e6) < 1 && std::fabs(gsmrRasterHz(922.19e6) - 922.2e6) < 1 && gsmrRasterHz(1.9e9) == 0, "raster in the band");
    CHECK(std::string(gsmrBandName(881e6)) == "GSM 850" && std::string(gsmrBandName(922e6)) == "GSM-R" && std::string(gsmrBandName(1950e6)) == "PCS 1900" && std::string(gsmrBandName(1840e6)) == "DCS 1800", "band names");
    // CCCH kinds
    CHECK(ccchKind(encodePagingKind(0x21).data()) == CcchKind::Paging1 && ccchKind(encodePagingKind(0x22).data()) == CcchKind::Paging2 && ccchKind(encodePagingKind(0x24).data()) == CcchKind::Paging3, "paging kinds");
    CHECK(ccchKind(encodePagingKind(0x3F).data()) == CcchKind::ImmAssign && ccchKind(encodePagingKind(0x3A).data()) == CcchKind::ImmAssignReject && ccchKind(idleFrame().data()) == CcchKind::Idle, "assignment / idle");
    CHECK(ccchKind(encodeSi3(Si3{}).data()) == CcchKind::Other, "SI3 is not a CCCH kind");
    { CcchCount c; c.add(CcchKind::Paging2); c.add(CcchKind::ImmAssignExt); c.add(CcchKind::Idle); CHECK(c.paging == 1 && c.assign == 1 && c.idle == 1 && c.blocks() == 3, "count"); }
    // SI2bis: a neighbour list in the SI2 format
    { Frame23 f = encodeSi2({20, 33, 70}, 0xFF, Si3{}); f[2] = kSi2bis; Bcch b; CHECK(parseBcch(f.data(), b) && b.type == kSi2bis && b.hasList && b.arfcns.size() == 3 && b.arfcns[1] == 33, "SI2bis list"); }
    // the receiver on the test signal: the CCCH load of each cell
    for (int k = 0; k < 2; k++) {
        SynthConfig sc; sc.snrDb = k ? 12 : 25; sc.cfoHz = k ? -1800 : 400; sc.mode = 41;
        auto syn = makeModeSynth(41, sc, 5e6);
        CHECK(syn != nullptr, "synth"); if (!syn) break;
        GsmrReceiver rx; rx.configure(syn->sampleRate()); rx.setCenterHz(kGsmrTestCentreHz);
        std::vector<cf32> buf(50000);
        for (int i = 0; i < 3 * 100; i++) {   // 3 s
            syn->generate(buf.data(), buf.size());
            rx.feed(buf.data(), buf.size());
            while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (int w = 0; w < 100 && rx.busy(); w++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        GsmrTelemetry t; rx.telemetry(t, 0);
        int withLoad = 0;
        for (auto& c : t.cells) {
            printf("  SNR %.0f dB, %+.0f Hz: ARFCN %d (%s) CCCH %llu blocks, paging %llu, assign %llu, idle %llu, other %llu, failed %llu\n", sc.snrDb, sc.cfoHz, c.arfcn, gsmrBandName(c.freqHz),
                   (unsigned long long)c.ccch.blocks(), (unsigned long long)c.ccch.paging, (unsigned long long)c.ccch.assign, (unsigned long long)c.ccch.idle, (unsigned long long)c.ccch.other, (unsigned long long)c.ccch.bad);
            if (c.ccch.blocks() < 20) continue;
            // the test pattern: per multiframe 9 blocks, 1 paging, 0.5 assignment
            const double pr = (double)c.ccch.paging / (double)c.ccch.blocks(), ar = (double)c.ccch.assign / (double)c.ccch.blocks();
            CHECK(std::fabs(pr - 1.0 / 9) < 0.04 && std::fabs(ar - 0.5 / 9) < 0.04 && c.ccch.other == 0, "CCCH shares %.3f %.3f", pr, ar);
            withLoad++;
        }
        CHECK(withLoad >= 1, "no cell with a CCCH count (%zu cells)", t.cells.size());
    }
    return testSummary("gsmr bands");
}
