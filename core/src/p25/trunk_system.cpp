// P25 trunking system model: what the control channel's TSBKs say (TIA-102.AABC), turned into a site, a channel identifier table and Grants.
// Receive only. Clean-room from the message layouts in the spec; the existing receiver's decoding (p25_rx.cpp) uses the same field positions.
#include "dect2/p25/trunk.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace dect2 {
namespace trunk {

namespace {

std::string fmtf(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmtf(const char* f, ...) {
    char b[240];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

// slots per carrier of a TDMA channel type (TIA-102.BBAC channel types): only the 12.5 kHz two-slot type is known to this implementation
int slotsOfType(int type) { return type <= 2 ? 1 : type == 3 ? 2 : 0; }   // 0..2: one voice channel per carrier (FDMA)

} // namespace

void SystemModel::clear() {
    site_ = SiteInfo();
    adj_.clear();
    for (auto& i : idens_) i = IdenInfo();
    secondary_.clear();
    nTsbk_ = nGrant_ = 0;
}

// The control channel is another site's (or another system's) now: what was learned from the old one must not place this one's channels. The
// identifier rows announced in the last two seconds stay (they may belong to the new site, which sends them in the same cycle as its site id);
// the neighbours and secondary control channels are the old site's.
void SystemModel::leaveSite(double now) {
    adj_.clear();
    secondary_.clear();
    for (auto& i : idens_) if (i.valid && now - i.lastSec > 2.0) i = IdenInfo();
}

double SystemModel::channelFreq(int channel, int* slot, bool* tdma) const {
    const IdenInfo& i = idens_[(channel >> 12) & 15];
    if (slot) *slot = -1;
    if (tdma) *tdma = false;
    if (!i.valid) return 0;
    int ch = channel & 0xFFF;
    if (i.tdma) {
        if (i.slots <= 0) return 0;   // a channel type this build cannot place
        if (slot) *slot = ch % i.slots;
        if (tdma) *tdma = true;
        ch /= i.slots;
    }
    return i.baseHz + ch * i.spacingHz;
}

std::vector<double> SystemModel::controlCandidates() const {
    std::vector<double> v;
    auto add = [&](double hz) {
        if (hz <= 0) return;
        for (double o : v) if (std::fabs(o - hz) < 1) return;
        v.push_back(hz);
    };
    add(site_.ccHz);
    for (double h : secondary_) add(h);
    for (const SiteInfo& a : adj_) add(a.ccHz);
    return v;
}

std::vector<double> SystemModel::ownControlChannels() const {
    std::vector<double> v;
    auto add = [&](double hz) {
        if (hz <= 0) return;
        for (double o : v) if (std::fabs(o - hz) < 1) return;
        v.push_back(hz);
    };
    add(site_.ccHz);
    for (double h : secondary_) add(h);
    return v;
}

void SystemModel::setSitePosition(int sysId, int rfss, int site, double lat, double lon) {
    for (auto& p : positions_)
        if (p.first.first == sysId && p.first.second.first == rfss && p.first.second.second == site) { p.second = {lat, lon}; return; }
    positions_.push_back({{sysId, {rfss, site}}, {lat, lon}});
}

std::string SystemModel::onTsbk(const p25::Tsbk& t, int nac, double now, std::vector<Grant>* grants) {
    nTsbk_++;
    if (t.mfid != 0 && t.mfid != 1) return std::string();   // a manufacturer's own message: not interpreted
    const uint64_t a = t.args;
    auto place = [&](int ch, Grant& g) {
        g.channel = ch;
        g.freqHz = channelFreq(ch, &g.slot, &g.tdma);
        const IdenInfo& ii = idens_[(ch >> 12) & 15];
        if (g.freqHz <= 0 && ii.valid && ii.tdma && ii.slots <= 0) g.unplacedType = ii.chanType;
    };
    auto emit = [&](const Grant& g) { nGrant_++; if (grants) grants->push_back(g); };
    switch (t.opcode) {
    case p25::kGrpVGrant: {   // service options 8, channel 16, group 16, source 24
        Grant g;
        const int opts = (int)(a >> 56);
        g.group = true; g.tg = (int)((a >> 24) & 0xFFFF); g.src = (int)(a & 0xFFFFFF);
        g.emergency = (opts & 0x80) != 0; g.encrypted = (opts & 0x40) != 0; g.priority = opts & 7 ? opts & 7 : 4;
        place((int)((a >> 40) & 0xFFFF), g);
        emit(g);
        return fmtf("GRP_V_CH_GRANT TG %d from %d on %s", g.tg, g.src, p25::channelText(g.channel).c_str());
    }
    case p25::kGrpVGrantUpd: {   // two (channel, group) pairs: repeats of calls in progress
        for (int k = 0; k < 2; k++) {
            Grant g;
            const int ch = (int)((a >> (48 - 32 * k)) & 0xFFFF), tg = (int)((a >> (32 - 32 * k)) & 0xFFFF);
            if (k == 1 && tg == (int)((a >> 32) & 0xFFFF) && ch == (int)((a >> 48) & 0xFFFF)) break;
            if (tg == 0) continue;
            g.group = true; g.tg = tg; g.explicitUpdate = true;
            place(ch, g);
            emit(g);
        }
        return std::string();
    }
    case kOpGrpVGrantUpdExp: {   // service options 8, reserved 8, channel (T) 16, channel (R) 16, group 16
        Grant g;
        const int opts = (int)(a >> 56);
        g.group = true; g.tg = (int)(a & 0xFFFF); g.explicitUpdate = true;
        g.emergency = (opts & 0x80) != 0; g.encrypted = (opts & 0x40) != 0; g.priority = opts & 7 ? opts & 7 : 4;
        // Channel (T), bits 32..47, is the voice channel's downlink (what a listener tunes to); channel (R) at bits 16..31 is the subscriber's uplink and is
        // not followed. Same choice as OP25's tk_p25.py (opcode 0x03 standard branch: the first channel field gives the frequency, the second is debug only).
        place((int)((a >> 32) & 0xFFFF), g);
        emit(g);
        return std::string();
    }
    case p25::kUuVGrant: {   // channel 16 (bits 48..63), target 24, source 24
        Grant g;
        g.group = false; g.tg = (int)((a >> 24) & 0xFFFFFF); g.src = (int)(a & 0xFFFFFF);
        place((int)((a >> 48) & 0xFFFF), g);
        emit(g);
        return fmtf("UU_V_CH_GRANT %d -> %d on %s", g.src, g.tg, p25::channelText(g.channel).c_str());
    }
    case p25::kIdenUp: case p25::kIdenUpVu: case kOpIdenUpTdma: {
        const int id = (int)(a >> 60) & 15;
        IdenInfo r;
        r.valid = true;
        r.baseHz = (double)(a & 0xFFFFFFFFULL) * 5;
        r.spacingHz = (double)((a >> 32) & 0x3FF) * 125;
        if (t.opcode == p25::kIdenUp) {
            const int bw = (int)(a >> 51) & 0x1FF, toff = (int)((a >> 42) & 0x1FF);
            r.bwHz = bw * 125.0;
            r.txOffsetHz = ((toff >> 8) & 1 ? 1 : -1) * (double)(toff & 0xFF) * 250000;
        } else {
            const int toff = (int)((a >> 42) & 0x3FFF);
            r.txOffsetHz = ((toff >> 13) & 1 ? 1 : -1) * (double)(toff & 0x1FFF) * r.spacingHz;
            const int field = (int)(a >> 56) & 15;
            if (t.opcode == p25::kIdenUpVu) { r.vu = true; r.bwHz = field == 4 ? 6250 : field == 5 ? 12500 : 0; }
            else { r.chanType = field; r.slots = slotsOfType(field); r.tdma = field >= 3; r.bwHz = 12500; }
        }
        r.lastSec = now;
        const bool news = !idens_[id].valid || idens_[id].baseHz != r.baseHz || idens_[id].spacingHz != r.spacingHz || idens_[id].tdma != r.tdma;
        idens_[id] = r;
        if (!news) return std::string();
        return fmtf("%s %d: base %.5f MHz, spacing %.3f kHz%s", p25::opcodeName(t.opcode) && *p25::opcodeName(t.opcode) ? p25::opcodeName(t.opcode) : "IDEN_UP", id, r.baseHz / 1e6,
                    r.spacingHz / 1e3, r.tdma ? (r.slots ? ", TDMA 2 slots" : ", TDMA (channel type not placed)") : "");
    }
    case p25::kNetSts: {   // WACN 20, system 12, control channel 16, service class 8
        const int wacn = (int)((a >> 36) & 0xFFFFF), sys = (int)((a >> 24) & 0xFFF), ch = (int)((a >> 8) & 0xFFFF);
        const bool news = site_.wacn != wacn || site_.sysId != sys;
        if (site_.valid && site_.wacn >= 0 && news) leaveSite(now);
        site_.valid = true; site_.wacn = wacn; site_.sysId = sys; site_.nac = nac; site_.ccChannel = ch; site_.ccHz = channelFreq(ch); site_.ssc = (int)(a & 0xFF); site_.lastSec = now;
        return news ? fmtf("NET_STS_BCST WACN %05X system %03X NAC %03X", wacn, sys, nac) : std::string();
    }
    case p25::kRfssSts: {   // (LRA 8) system 12, RFSS 8, site 8, control channel 16, service class 8
        const int sys = (int)((a >> 40) & 0xFFF), rf = (int)((a >> 32) & 0xFF), st = (int)((a >> 24) & 0xFF), ch = (int)((a >> 8) & 0xFFFF);
        const bool news = site_.rfss != rf || site_.site != st;
        if (site_.valid && site_.rfss >= 0 && (news || (site_.sysId >= 0 && site_.sysId != sys))) leaveSite(now);
        site_.valid = true; site_.sysId = sys; site_.rfss = rf; site_.site = st; site_.nac = nac; site_.ccChannel = ch; site_.ccHz = channelFreq(ch); site_.ssc = (int)(a & 0xFF); site_.lastSec = now;
        for (const auto& p : positions_)
            if (p.first.first == sys && p.first.second.first == rf && p.first.second.second == st) { site_.lat = p.second.first; site_.lon = p.second.second; site_.hasPosition = true; }
        return news ? fmtf("RFSS_STS_BCST system %03X RFSS %d site %d", sys, rf, st) : std::string();
    }
    case p25::kAdjSts: {
        SiteInfo s;
        s.valid = true; s.adjacent = true;
        s.sysId = (int)((a >> 40) & 0xFFF); s.rfss = (int)((a >> 32) & 0xFF); s.site = (int)((a >> 24) & 0xFF);
        s.ccChannel = (int)((a >> 8) & 0xFFFF); s.ccHz = channelFreq(s.ccChannel); s.ssc = (int)(a & 0xFF); s.wacn = site_.wacn; s.lastSec = now;
        for (const auto& p : positions_)
            if (p.first.first == s.sysId && p.first.second.first == s.rfss && p.first.second.second == s.site) { s.lat = p.second.first; s.lon = p.second.second; s.hasPosition = true; }
        auto it = std::find_if(adj_.begin(), adj_.end(), [&](const SiteInfo& o) { return o.sysId == s.sysId && o.rfss == s.rfss && o.site == s.site; });
        if (it != adj_.end()) { *it = s; return std::string(); }
        adj_.push_back(s);
        return fmtf("ADJ_STS_BCST system %03X RFSS %d site %d on %s", s.sysId, s.rfss, s.site, p25::channelText(s.ccChannel).c_str());
    }
    case kOpSccb: {   // RFSS 8, site 8, channel 16, service class 8, channel 16, service class 8
        const int rf = (int)((a >> 56) & 0xFF), st = (int)((a >> 48) & 0xFF);
        if (site_.valid && (rf != site_.rfss || st != site_.site)) return std::string();   // another site's: the adjacent list carries those
        std::string out;
        for (int k = 0; k < 2; k++) {
            const int ch = (int)((a >> (32 - 24 * k)) & 0xFFFF);
            const double hz = channelFreq(ch);
            if (hz <= 0) continue;
            if (std::none_of(secondary_.begin(), secondary_.end(), [&](double h) { return std::fabs(h - hz) < 1; })) {
                secondary_.push_back(hz);
                out = fmtf("SCCB secondary control channel %s", p25::channelText(ch).c_str());
            }
        }
        return out;
    }
    default: return std::string();
    }
}

namespace gen {

namespace {
p25::Tsbk mk(int op, uint64_t args) { p25::Tsbk t; t.opcode = op; t.args = args; return t; }
}

p25::Tsbk idenUp(int id, double baseHz, double spacingHz, double txOffsetHz, double bwHz) {
    const uint64_t toff = (uint64_t)(txOffsetHz >= 0 ? 0x100 : 0) | (uint64_t)(std::llround(std::fabs(txOffsetHz) / 250000) & 0xFF);
    return mk(p25::kIdenUp, ((uint64_t)(id & 15) << 60) | ((uint64_t)std::llround(bwHz / 125) << 51) | (toff << 42) | ((uint64_t)std::llround(spacingHz / 125) << 32) | (uint64_t)std::llround(baseHz / 5));
}
static p25::Tsbk idenVuLike(int op, int id, int field, double baseHz, double spacingHz, double txOffsetHz) {
    const uint64_t spac = (uint64_t)std::llround(spacingHz / 125);
    const uint64_t n = spac ? (uint64_t)std::llround(std::fabs(txOffsetHz) / (double)(spac * 125)) : 0;
    const uint64_t toff = (txOffsetHz >= 0 ? 0x2000ULL : 0) | (n & 0x1FFF);
    return mk(op, ((uint64_t)(id & 15) << 60) | ((uint64_t)(field & 15) << 56) | (toff << 42) | (spac << 32) | (uint64_t)std::llround(baseHz / 5));
}
p25::Tsbk idenUpVu(int id, double baseHz, double spacingHz, double txOffsetHz, int bwCode) { return idenVuLike(p25::kIdenUpVu, id, bwCode, baseHz, spacingHz, txOffsetHz); }
p25::Tsbk idenUpTdma(int id, int chanType, double baseHz, double spacingHz, double txOffsetHz) { return idenVuLike(kOpIdenUpTdma, id, chanType, baseHz, spacingHz, txOffsetHz); }
p25::Tsbk netSts(int wacn, int sysId, int cc, int ssc) { return mk(p25::kNetSts, ((uint64_t)wacn << 36) | ((uint64_t)sysId << 24) | ((uint64_t)cc << 8) | (uint64_t)ssc); }
p25::Tsbk rfssSts(int sysId, int rfss, int site, int cc, int ssc) {
    return mk(p25::kRfssSts, ((uint64_t)sysId << 40) | ((uint64_t)rfss << 32) | ((uint64_t)site << 24) | ((uint64_t)cc << 8) | (uint64_t)ssc);
}
p25::Tsbk adjSts(int sysId, int rfss, int site, int cc, int ssc) {
    return mk(p25::kAdjSts, ((uint64_t)sysId << 40) | ((uint64_t)rfss << 32) | ((uint64_t)site << 24) | ((uint64_t)cc << 8) | (uint64_t)ssc);
}
p25::Tsbk sccb(int rfss, int site, int ch1, int ch2) { return mk(kOpSccb, ((uint64_t)rfss << 56) | ((uint64_t)site << 48) | ((uint64_t)ch1 << 32) | ((uint64_t)0x70 << 24) | ((uint64_t)ch2 << 8) | 0x70); }
p25::Tsbk grpVGrant(int svc, int ch, int tg, int src) { return mk(p25::kGrpVGrant, ((uint64_t)svc << 56) | ((uint64_t)ch << 40) | ((uint64_t)tg << 24) | (uint64_t)src); }
p25::Tsbk uuVGrant(int ch, int dst, int src) { return mk(p25::kUuVGrant, ((uint64_t)ch << 48) | ((uint64_t)dst << 24) | (uint64_t)src); }
p25::Tsbk grpVGrantUpd(int ch1, int tg1, int ch2, int tg2) { return mk(p25::kGrpVGrantUpd, ((uint64_t)ch1 << 48) | ((uint64_t)tg1 << 32) | ((uint64_t)ch2 << 16) | (uint64_t)tg2); }
p25::Tsbk grpVGrantUpdExp(int svc, int chT, int chR, int tg) { return mk(kOpGrpVGrantUpdExp, ((uint64_t)svc << 56) | ((uint64_t)chT << 32) | ((uint64_t)chR << 16) | (uint64_t)tg); }

} // namespace gen

} // namespace trunk
} // namespace dect2
