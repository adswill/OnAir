// Hytera XPT inside the DMR receiver: the block parser, and the DMR receiver reading the blocks from the generator's air signal. Field positions come from DSD-FME; no real XPT system was heard.
#include "dect2/dmr/dmr_gen.h"
#include "dect2/dmr/dmr_rx.h"
#include "dect2/dmr/dmr_tel.h"
#include "dect2/dmr/dmr_xpt.h"
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    {
        const int st[6] = {3, 0, 2, 0, 1, 3};
        const int tg[6] = {21, 0, 77, 0, 9, 35};
        Csbk c = xptSiteStatusCsbk(1, 6, st, tg);
        XptState x;
        CHECK(xptParseCsbk(c, 2.0, x) && x.seen && x.freeLcn == 6, "site status: free %d", x.freeLcn);
        for (int i = 0; i < 6; i++) CHECK(x.lsn[6 + i].status == st[i] && x.lsn[6 + i].tg == tg[i], "LSN %d: %d %d", i, x.lsn[6 + i].status, x.lsn[6 + i].tg);
        CHECK(x.lsn[0].status == -1, "sequence 1 fills LSN 7..12");
        // adjacent
        Csbk a; a.opcode = kXptAdjacentOp; a.fid = kXptFid; a.data[0] = 0x18; a.data[1] = 0x70; a.data[2] = 0x28; a.data[3] = 0x20;
        XptState y;
        CHECK(xptParseCsbk(a, 1.0, y) && y.adjacent.size() == 2, "adjacent: %zu", y.adjacent.size());
        CHECK(y.adjacent.size() == 2 && y.adjacent[0].site == 3 && y.adjacent[0].freeLcn == 7 && y.adjacent[1].site == 5 && y.adjacent[1].freeLcn == 2, "adjacent values");
        Csbk other; other.opcode = 0x3D; other.fid = kXptFid;
        CHECK(!xptParseCsbk(other, 0, y), "other opcodes are not XPT");
        Csbk std; std.opcode = kXptSiteStatusOp; std.fid = 0;
        CHECK(!xptParseCsbk(std, 0, y), "another manufacturer");
        FullLc f = xptCallLc(3, 5, 2, 0x1234, 0x005678);
        XptState z;
        CHECK(xptParseCall(f, 3.0, z) && z.calls.size() == 1 && z.calls[0].lcn == 3 && z.calls[0].freeLcn == 5 && z.calls[0].handshake == 2 && z.calls[0].target == 0x1234 && z.calls[0].source == 0x5678, "call set-up");
    }
    for (double snr : {35.0, 18.0, 10.0}) {
        DmrGenConfig c;
        c.rate = 2.4e6; c.xptSite = true; c.snrDb = snr; c.cfoHz = 150;
        DmrSignal sig(c);
        std::vector<cf32> buf(1u << 14);
        DmrReceiver rx;
        rx.setSilent(true);
        rx.configure(c.rate);
        for (int i = 0; i < 700; i++) { sig.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
        DmrTelemetry t;
        rx.telemetry(t, 0);
        printf("SNR %.0f dB: XPT seen %d, free LCN %d, %zu call set-ups, %llu blocks ok\n", snr, (int)t.xpt.seen, t.xpt.freeLcn, t.xpt.calls.size(), (unsigned long long)t.blocksOk);
        CHECK(t.xpt.seen && t.xpt.freeLcn == 5, "SNR %.0f: site status not read", snr);
        CHECK(!t.xpt.calls.empty() && t.xpt.calls[0].source == 0x1234 && t.xpt.calls[0].target == 0x15, "SNR %.0f: call set-up", snr);
        bool lsn = false;
        for (const auto& l : t.xpt.lsn) lsn |= l.status == 3 && l.tg == 21;
        CHECK(lsn, "SNR %.0f: LSN table", snr);
    }
    return testSummary("dmr_xpt");
}
