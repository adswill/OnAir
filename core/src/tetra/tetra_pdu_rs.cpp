// TETRA network information parsing on top of the Rust crate rust/onair_tetra: the parse side of tetra_pdu.h;
// tetra_pdu.cpp keeps the encoders and the generator PDUs. Only broadcasts are read (see tetra_pdu.h).
#include "dect2/tetra/tetra_pdu.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/tetra.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2::tetra {

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[5];
        onair_tetra_struct_sizes(s);
        dect2::rsAbiCheck("onair_tetra", s, {sizeof(OnairTetraSync), sizeof(OnairTetraSysinfo), sizeof(OnairTetraNeighbour), sizeof(OnairTetraNwrk), sizeof(OnairTetraMacBlock)});
    }
} abiCheck;


void fromC(const OnairTetraSysinfo& o, Sysinfo& s) {
    s.mainCarrier = o.main_carrier; s.band = o.band; s.offset = o.offset; s.duplex = o.duplex; s.reverse = o.reverse != 0;
    s.numScch = o.num_scch; s.msTxPwrMax = o.ms_tx_pwr_max; s.rxLevAccessMin = o.rx_lev_access_min; s.accessParam = o.access_param; s.dlTimeout = o.dl_timeout;
    s.cckFlag = o.cck_flag != 0; s.hyperframeOrCck = o.hyperframe_or_cck; s.optFlag = o.opt_flag; s.optValue = o.opt_value;
    s.la = o.la; s.subscriberClass = o.subscriber_class; s.bsService = o.bs_service;
}
OnairTetraSysinfo toC(const Sysinfo& s) {
    OnairTetraSysinfo o;
    memset(&o, 0, sizeof o);
    o.main_carrier = s.mainCarrier; o.band = s.band; o.offset = s.offset; o.duplex = s.duplex; o.reverse = s.reverse;
    o.num_scch = s.numScch; o.ms_tx_pwr_max = s.msTxPwrMax; o.rx_lev_access_min = s.rxLevAccessMin; o.access_param = s.accessParam; o.dl_timeout = s.dlTimeout;
    o.cck_flag = s.cckFlag; o.hyperframe_or_cck = s.hyperframeOrCck; o.opt_flag = s.optFlag; o.opt_value = s.optValue;
    o.la = s.la; o.subscriber_class = s.subscriberClass; o.bs_service = s.bsService;
    return o;
}
// negative or absurd lengths are clamped to 0 before they reach Rust
inline size_t clampLen(long n) { return n < 0 ? 0 : (size_t)n; }
} // namespace

bool parseSync(const Bits& b, Sync& s) {
    OnairTetraSync o;
    memset(&o, 0, sizeof o);
    const int r = onair_tetra_parse_sync(b.data(), clampLen((long)b.size()), &o);
    s.systemCode = o.system_code; s.colourCode = o.colour_code; s.tn = o.tn; s.fn = o.fn_; s.mn = o.mn; s.sharingMode = o.sharing_mode;
    s.tsReservedFrames = o.ts_reserved_frames; s.uplaneDtx = o.uplane_dtx != 0; s.frame18Ext = o.frame18_ext != 0; s.mcc = o.mcc; s.mnc = o.mnc;
    s.nbrBroadcast = o.nbr_broadcast; s.cellLoad = o.cell_load; s.lateEntry = o.late_entry != 0;
    return r == 2;
}

bool parseSysinfo(const Bits& b, Sysinfo& s) {
    OnairTetraSysinfo o;
    memset(&o, 0, sizeof o);
    const int r = onair_tetra_parse_sysinfo(b.data(), clampLen((long)b.size()), &o);
    if (r == 0) return false;   // not a SYSINFO: s untouched
    fromC(o, s);
    return r == 2;
}

std::string serviceText(int v) {
    uint8_t buf[256];
    const size_t n = onair_tetra_service_text(v, buf, sizeof buf);
    return std::string(reinterpret_cast<const char*>(buf), n);
}

Security parseSecurity(const Sysinfo& s) {
    Security o;
    const OnairTetraSysinfo c = toC(s);
    const int f = onair_tetra_security(&c);
    if (!(f & 1)) return o;
    o.valid = true; o.authRequired = (f >> 1) & 1; o.class1 = (f >> 2) & 1; o.class2 = (f >> 3) & 1; o.class3 = (f >> 4) & 1;
    return o;
}

double dlCarrierHz(int band, int carrier, int offset) { return onair_tetra_dl_carrier_hz(band, carrier, offset); }
double duplexSpacingHz(int band, int duplex) { return onair_tetra_duplex_spacing_hz(band, duplex); }
double ulCarrierHz(int band, int carrier, int offset, int duplex, bool reverse) { return onair_tetra_ul_carrier_hz(band, carrier, offset, duplex, reverse); }

std::string networkTimeText(uint64_t t) {
    uint8_t buf[96];
    const size_t n = onair_tetra_network_time_text(t, buf, sizeof buf);
    return std::string(reinterpret_cast<const char*>(buf), n);
}

void parseMacBlock(const Bits& b, MacBlock& out) {
    const size_t cap = b.size() / 8 + 2;
    std::vector<OnairTetraNwrk> nw(cap);
    std::vector<OnairTetraNeighbour> nb(cap * 7);
    OnairTetraMacBlock k;
    memset(&k, 0, sizeof k);
    onair_tetra_parse_block(b.data(), clampLen((long)b.size()), &k, nw.data(), nw.size(), nb.data(), nb.size());
    if (k.sysinfo) { out.sysinfo = true; fromC(k.si, out.si); }
    if (k.access_define) out.accessDefine = true;
    out.resources += k.resources; out.encrypted += k.encrypted; out.fragments += k.fragments;
    for (int i = 0; i < 8; i++) out.byPd[i] += k.by_pd[i];
    for (int i = 0; i < k.n_call && i < 4; i++) {
        CallEvent e;
        e.kind = k.call[i].kind; e.callId = k.call[i].call_id; e.enc = k.call[i].enc; e.slots = k.call[i].slots; e.carrier = k.call[i].carrier; e.gssi = k.call[i].gssi;
        out.calls.push_back(e);
    }
    for (uint32_t i = 0; i < k.n_nwrk && i < nw.size(); i++) {
        NwrkBroadcast n;
        n.reselect = nw[i].reselect; n.cellLoad = nw[i].cell_load; n.hasTime = nw[i].has_time != 0; n.time48 = nw[i].time48;
        for (uint32_t j = 0; j < nw[i].nbr_count; j++) {
            const OnairTetraNeighbour& c = nb[nw[i].nbr_first + j];
            Neighbour x;
            x.cellId = c.cell_id; x.reselTypes = c.resel_types; x.cellLoad = c.cell_load; x.carrier = c.carrier; x.synced = c.synced != 0;
            x.hasExt = c.has_ext != 0; x.band = c.band; x.offset = c.offset; x.duplex = c.duplex; x.reverse = c.reverse != 0;
            x.mcc = c.mcc; x.mnc = c.mnc; x.la = c.la; x.maxTx = c.max_tx; x.minRx = c.min_rx; x.subscriberClass = c.subscriber_class;
            x.bsService = c.bs_service; x.timeshare = c.timeshare; x.frameOffset = c.frame_offset;
            n.nbrs.push_back(x);
        }
        out.nwrk.push_back(n);
    }
}

int aachDlUsage(uint16_t aa, bool frame18) { return onair_tetra_aach_dl_usage(aa, frame18); }
const char* dlUsageText(int u) { return reinterpret_cast<const char*>(onair_tetra_dl_usage_text(u)); }

} // namespace dect2::tetra
