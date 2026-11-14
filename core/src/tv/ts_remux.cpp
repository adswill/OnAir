// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Several transport streams into one multiplex (see ts_remux.h, issues #492 and #458).
#include "dect2/tv/ts_remux.h"
#include "dect2/tv/ts_proto.h"
#include <algorithm>

namespace dect2 {

namespace {

constexpr int kPmtMaxLen = 183;   // a rewritten PMT must fit one packet (pointer field + section)
constexpr size_t kNameMax = 60;
constexpr int kMaxAutoBlocks = 31;   // automatic blocks 0x100 * (k + 1) stay under 0x2000

void nullPacket(uint8_t* pk) {
    pk[0] = 0x47; pk[1] = 0x1F; pk[2] = 0xFF; pk[3] = 0x10;
    std::fill(pk + 4, pk + 188, uint8_t(0xFF));
}

int pidOf(const uint8_t* pk) { return ((pk[1] & 0x1F) << 8) | pk[2]; }

// The section that starts in this packet, when it is whole in it: its offset and its length (header and CRC included). False otherwise.
bool sectionIn(const uint8_t* pk, size_t& at, size_t& len) {
    if (!(pk[1] & 0x40)) return false;
    const int afc = (pk[3] >> 4) & 3;
    if (afc == 0 || afc == 2) return false;
    size_t off = 4;
    if (afc == 3) off = 5 + (size_t)pk[4];
    if (off >= 188) return false;
    const size_t s = off + 1 + (size_t)pk[off];
    if (s + 3 > 188) return false;
    const size_t n = 3 + ((((size_t)pk[s + 1] & 0x0F) << 8) | pk[s + 2]);
    if (n < 12 || s + n > 188) return false;
    at = s; len = n;
    return true;
}

void putCrc(std::vector<uint8_t>& s) {
    const uint32_t c = tsp::crc32(s.data(), (int)s.size());
    s.push_back((uint8_t)(c >> 24)); s.push_back((uint8_t)(c >> 16)); s.push_back((uint8_t)(c >> 8)); s.push_back((uint8_t)c);
}

void setSectionLength(std::vector<uint8_t>& s) {   // before the CRC is added: the length counts it
    const size_t n = s.size() + 4 - 3;
    s[1] = (uint8_t)((s[1] & 0xF0) | ((n >> 8) & 0x0F));
    s[2] = (uint8_t)(n & 0xFF);
}

// A version that moves on when the table changes (the version byte and CRC are left out of the comparison)
void bumpIfChanged(const std::vector<uint8_t>& s, size_t verAt, std::vector<uint8_t>& last, int& version) {
    std::vector<uint8_t> k(s);
    k[verAt] = 0;
    if (!last.empty() && k != last) version = (version + 1) & 31;
    last = std::move(k);
}

std::string dvbName(const std::string& utf8) {   // a UTF-8 name gets the DVB prefix 0x15 when it is not plain ASCII
    std::string s = utf8.substr(0, kNameMax);
    for (unsigned char c : s) if (c >= 0x80) return std::string(1, '\x15') + s;
    return s;
}

bool usablePid(int p) { return p >= 0x20 && p < 0x1FFF; }

} // namespace

TsRemux::TsRemux(std::vector<TsRemuxInput> in, const TsRemuxConfig& c) : cfg_(c) {
    for (auto& s : in) { In x; x.src = std::move(s); in_.push_back(std::move(x)); }
    const double pps = std::max(1.0, cfg_.outBitrate / (188.0 * 8.0));
    patEvery_ = std::max<uint64_t>(1, (uint64_t)(pps * 0.1));   // PAT 10 times a second, SDT twice (as the generator itself)
    sdtEvery_ = std::max<uint64_t>(1, (uint64_t)(pps * 0.5));
}

void TsRemux::next(uint8_t* pk) {
    count_++;
    const double out = std::max(1.0, cfg_.outBitrate);
    std::vector<double> share(in_.size(), 0.0);
    double total = 0;
    for (size_t k = 0; k < in_.size(); k++) {
        In& x = in_[k];
        const double r = x.src.rate ? x.src.rate() : 0.0;
        const bool act = r > 0;
        if (act != x.active) {   // started or stopped: learn its tables again; a fresh start drops what queued before the transmitter took it
            x.active = act;
            x.progs.clear(); x.sdt.clear(); x.credit = 0;
            if (act && x.src.flush) x.src.flush();
        }
        if (!act) continue;
        double s = r * 1.005 / out;   // a little faster than the input, so its queue stays short
        if (x.src.queued && (double)x.src.queued() * 1504.0 > r * 0.3) s *= 1.1;   // over 0.3 s waiting: catch up
        share[k] = s;
        total += s;
    }
    const double scale = total > 0.98 ? 0.98 / total : 1.0;   // never all of it: the PAT and SDT need free slots
    int best = -1;
    for (size_t k = 0; k < in_.size(); k++) {
        In& x = in_[k];
        if (!x.active) continue;
        x.credit = std::min(x.credit + share[k] * scale, 2.0);
        if (x.credit >= 1.0 && (best < 0 || x.credit > in_[(size_t)best].credit)) best = (int)k;
    }
    if (count_ >= nextPat_ || count_ >= nextSdt_) queueTables();
    if (best >= 0) {
        In& x = in_[(size_t)best];
        x.credit -= 1.0;
        if (fromInput(x, best, pk)) { x.out++; return; }
    }
    if (!psi_.empty()) {
        std::copy(psi_.front().begin(), psi_.front().end(), pk);
        psi_.pop_front();
        return;
    }
    nullPacket(pk);
}

bool TsRemux::fromInput(In& x, int k, uint8_t* pk) {
    if (!x.src.pop || !x.src.pop(pk)) return false;
    if (pk[0] != 0x47) return false;
    const int pid = pidOf(pk);
    size_t at = 0, len = 0;
    if (pid == 0x1FFF) return false;
    if (pid == 0) {   // its PAT: where its PMTs are
        if (sectionIn(pk, at, len) && pk[at] == 0x00) readPat(x, pk + at, (int)len);
        return false;
    }
    if (pid == 0x11) {   // its SDT: its own names
        if (sectionIn(pk, at, len)) readSdt(x, pk + at, (int)len);
        return false;
    }
    if (pid < 0x20) return false;   // other SI of its own (NIT, EIT, TDT ...): the multiplex does not carry them
    for (Prog& pg : x.progs)
        if (pid == pg.pmtIn) {
            if (!sectionIn(pk, at, len) || pk[at] != 0x02) return false;
            const std::vector<uint8_t> sec(pk + at, pk + at + len);
            return rewritePmt(x, k, pg, sec.data(), (int)len, pk, pk[3] & 0x0F);
        }
    for (const Prog& pg : x.progs)
        for (const auto& m : pg.map)
            if (m.first == pid) {
                pk[1] = (uint8_t)((pk[1] & 0xE0) | ((m.second >> 8) & 0x1F));
                pk[2] = (uint8_t)(m.second & 0xFF);
                return true;
            }
    return false;   // a PID no PMT names (or no PMT yet)
}

void TsRemux::readPat(In& x, const uint8_t* s, int len) {
    std::vector<std::pair<int, int>> list;   // programme number, PMT PID
    for (int p = 8; p + 4 <= len - 4; p += 4) {
        const int prog = (s[p] << 8) | s[p + 1];
        if (prog == 0) continue;   // the NIT
        list.emplace_back(prog, ((s[p + 2] & 0x1F) << 8) | s[p + 3]);
        if (!x.src.keepIds) break;   // one programme per input unless it is carried whole
    }
    std::vector<Prog> next;
    for (const auto& [prog, pmt] : list) {
        auto it = std::find_if(x.progs.begin(), x.progs.end(), [&](const Prog& g) { return g.progIn == prog && g.pmtIn == pmt; });
        if (it != x.progs.end()) next.push_back(std::move(*it));
        else { Prog g; g.progIn = prog; g.pmtIn = pmt; next.push_back(std::move(g)); }
    }
    x.progs = std::move(next);
}

bool TsRemux::pidTaken(int pid, const Prog* self, int selfInput) const {
    for (size_t k = 0; k < in_.size(); k++)
        for (const Prog& g : in_[k].progs) {
            if (&g == self) continue;
            if (!g.pmtSeen) {
                if ((int)k < selfInput && in_[k].src.keepIds && g.pmtIn == pid) return true;
                continue;
            }
            if (g.pmtOut == pid) return true;
            for (const auto& m : g.map) if (m.second == pid) return true;
        }
    return false;
}

bool TsRemux::sidTaken(int sid, const Prog* self, int selfInput) const {
    for (size_t k = 0; k < in_.size(); k++)
        for (const Prog& g : in_[k].progs) {
            if (&g == self) continue;
            if (g.pmtSeen ? g.sid == sid : ((int)k < selfInput && in_[k].src.keepIds && g.progIn == sid)) return true;
        }
    return false;
}

bool TsRemux::rewritePmt(In& x, int k, Prog& pg, const uint8_t* sec, int len, uint8_t* pk, int cc) {
    if (len < 16 || len > kPmtMaxLen) return false;
    std::vector<uint8_t> o(sec, sec + len - 4);   // without its CRC
    // the input's streams, in PMT order
    std::vector<int> pidsIn;
    std::vector<size_t> pidAt;
    const int pil = ((o[10] & 0x0F) << 8) | o[11];
    size_t p = 12 + (size_t)pil;
    while (p + 5 <= o.size()) {
        pidsIn.push_back(((o[p + 1] & 0x1F) << 8) | o[p + 2]);
        pidAt.push_back(p);
        const size_t eil = (((size_t)o[p + 3] & 0x0F) << 8) | o[p + 4];
        p += 5 + eil;
        if (pidsIn.size() >= 0xF0) return false;
    }
    if (p != o.size() || pidsIn.empty()) return false;   // a malformed PMT: keep the last good one
    const int pcrIn = ((o[8] & 0x1F) << 8) | o[9];

    // where it goes
    int block = k;          // automatic block
    bool automatic = !x.src.keepIds;
    if (x.src.keepIds && pg.autoIdx < 0) {   // its own PIDs, unless an earlier input uses one of them
        bool clash = !usablePid(pg.pmtIn) || pidTaken(pg.pmtIn, &pg, k);
        for (int q : pidsIn) clash = clash || !usablePid(q) || q == pg.pmtIn || pidTaken(q, &pg, k);
        if (pcrIn != 0x1FFF && std::find(pidsIn.begin(), pidsIn.end(), pcrIn) == pidsIn.end()) clash = clash || !usablePid(pcrIn) || pidTaken(pcrIn, &pg, k);
        if (clash) pg.autoIdx = std::min(kMaxAutoBlocks - 1, (int)in_.size() + moved_++);
    }
    if (x.src.keepIds && pg.autoIdx >= 0) { automatic = true; block = pg.autoIdx; }
    const int base = esPidBase(block);
    std::vector<std::pair<int, int>> map;
    for (size_t j = 0; j < pidsIn.size(); j++) {
        int out = pidsIn[j];
        if (automatic) {
            out = base + (int)j;
            if (!x.src.keepIds && j < x.src.esPids.size() && usablePid(x.src.esPids[j])) out = x.src.esPids[j];
        }
        map.emplace_back(pidsIn[j], out);
        o[pidAt[j] + 1] = (uint8_t)((o[pidAt[j] + 1] & 0xE0) | (out >> 8));
        o[pidAt[j] + 2] = (uint8_t)(out & 0xFF);
    }
    int pcrOut = 0x1FFF;
    if (pcrIn != 0x1FFF) {
        for (const auto& m : map) if (m.first == pcrIn) pcrOut = m.second;
        if (pcrOut == 0x1FFF) { pcrOut = automatic ? base + 0xFF : pcrIn; map.emplace_back(pcrIn, pcrOut); }   // PCR on a PID of its own
    }
    o[8] = (uint8_t)((o[8] & 0xE0) | (pcrOut >> 8));
    o[9] = (uint8_t)(pcrOut & 0xFF);
    int sid = 0, pmtOut = 0;
    if (x.src.keepIds) {
        sid = pg.progIn;
        while (sidTaken(sid, &pg, k)) sid = sid % 0xFFFF + 1;   // a number an earlier input has: the next free one
        pmtOut = automatic ? pmtPid(block) : pg.pmtIn;
    } else {
        sid = x.src.serviceId > 0 ? x.src.serviceId : cfg_.firstServiceId + k;
        pmtOut = usablePid(x.src.pmtPid) ? x.src.pmtPid : pmtPid(k);
    }
    sid &= 0xFFFF;
    o[3] = (uint8_t)(sid >> 8);
    o[4] = (uint8_t)(sid & 0xFF);
    bumpIfChanged(o, 5, pg.lastPmt, pg.pmtVersion);
    o[5] = (uint8_t)(0xC1 | (pg.pmtVersion << 1));
    putCrc(o);
    pg.map = std::move(map);
    pg.sid = sid;
    pg.pmtOut = pmtOut;
    pg.pmtSeen = true;
    pk[0] = 0x47;
    pk[1] = (uint8_t)(0x40 | (pmtOut >> 8));
    pk[2] = (uint8_t)(pmtOut & 0xFF);
    pk[3] = (uint8_t)(0x10 | (cc & 0x0F));
    pk[4] = 0;
    std::copy(o.begin(), o.end(), pk + 5);
    std::fill(pk + 5 + o.size(), pk + 188, uint8_t(0xFF));
    return true;
}

void TsRemux::readSdt(In& x, const uint8_t* s, int len) {
    if (s[0] != 0x42 || len < 15) return;
    const int end = len - 4;
    int p = 11;
    while (p + 5 <= end) {   // every service of the input
        const int sid = (s[p] << 8) | s[p + 1];
        const int dl = ((s[p + 3] & 0x0F) << 8) | s[p + 4];
        int d = p + 5;
        const int dend = std::min(end, d + dl);
        while (d + 2 <= dend) {
            const int tag = s[d], n = s[d + 1];
            if (d + 2 + n > dend) break;
            if (tag == 0x48 && n >= 3) {
                const uint8_t* q = s + d + 2;
                const int pl = q[1];
                if (2 + pl + 1 > n) break;
                const int nl = q[2 + pl];
                if (3 + pl + nl > n) break;
                SdtName& sn = x.sdt[sid];
                sn.type = q[0];
                sn.provider.assign((const char*)q + 2, (size_t)pl);
                sn.name.assign((const char*)q + 3 + pl, (size_t)nl);
                break;
            }
            d += 2 + n;
        }
        p += 5 + dl;
    }
}

std::vector<TsRemuxProgramme> TsRemux::programmes() const {
    std::vector<TsRemuxProgramme> v;
    for (size_t k = 0; k < in_.size(); k++) {
        const In& x = in_[k];
        if (!x.active) continue;
        for (const Prog& g : x.progs) {
            if (!g.pmtSeen) continue;
            TsRemuxProgramme r;
            r.input = (int)k; r.serviceId = g.sid; r.pmtPid = g.pmtOut;
            for (const auto& m : g.map) r.esPids.push_back(m.second);
            auto it = x.sdt.find(g.progIn);
            r.name = !x.src.keepIds && !x.src.name.empty() ? x.src.name : it != x.sdt.end() ? it->second.name : std::string();
            v.push_back(std::move(r));
        }
    }
    return v;
}

void TsRemux::queueTables() {
    if (count_ >= nextPat_) {
        nextPat_ = count_ + patEvery_;
        std::vector<uint8_t> s = {0x00, 0xB0, 0, (uint8_t)(cfg_.tsid >> 8), (uint8_t)cfg_.tsid, 0, 0, 0};
        for (const In& x : in_) {
            if (!x.active) continue;
            for (const Prog& g : x.progs) {
                if (!g.pmtSeen) continue;   // a programme is in the PAT once its PMT is
                s.push_back((uint8_t)(g.sid >> 8)); s.push_back((uint8_t)g.sid);
                s.push_back((uint8_t)(0xE0 | (g.pmtOut >> 8))); s.push_back((uint8_t)g.pmtOut);
            }
        }
        setSectionLength(s);
        bumpIfChanged(s, 5, lastPat_, patVersion_);
        s[5] = (uint8_t)(0xC1 | (patVersion_ << 1));
        putCrc(s);
        queueSection(0, s, patCc_);
    }
    if (count_ >= nextSdt_) {
        nextSdt_ = count_ + sdtEvery_;
        std::vector<uint8_t> s = {0x42, 0xF0, 0, (uint8_t)(cfg_.tsid >> 8), (uint8_t)cfg_.tsid, 0, 0, 0, (uint8_t)(cfg_.onid >> 8), (uint8_t)cfg_.onid, 0xFF};
        for (size_t k = 0; k < in_.size(); k++) {
            const In& x = in_[k];
            if (!x.active) continue;
            for (const Prog& g : x.progs) {
                if (!g.pmtSeen) continue;
                auto it = x.sdt.find(g.progIn);
                const SdtName* own = it != x.sdt.end() ? &it->second : nullptr;
                std::string name, prov;
                if (!x.src.keepIds && !x.src.name.empty()) name = dvbName(x.src.name);
                else if (own && !own->name.empty()) name = own->name.substr(0, kNameMax);
                else name = x.src.keepIds ? "Programme " + std::to_string(g.sid) : "Generator " + std::to_string(k + 1);
                if (!x.src.provider.empty()) prov = dvbName(x.src.provider);
                else if (own) prov = own->provider.substr(0, kNameMax);
                const size_t dlen = 2 + 3 + prov.size() + name.size();
                if (s.size() + 5 + dlen + 4 > 1024) break;   // one section
                s.push_back((uint8_t)(g.sid >> 8)); s.push_back((uint8_t)g.sid);
                s.push_back(0xFC);                                    // no EIT
                s.push_back((uint8_t)(0x80 | ((dlen >> 8) & 0x0F)));  // running, free to air
                s.push_back((uint8_t)dlen);
                s.push_back(0x48); s.push_back((uint8_t)(dlen - 2));
                s.push_back((uint8_t)(own ? own->type : 1));
                s.push_back((uint8_t)prov.size()); s.insert(s.end(), prov.begin(), prov.end());
                s.push_back((uint8_t)name.size()); s.insert(s.end(), name.begin(), name.end());
            }
        }
        setSectionLength(s);
        bumpIfChanged(s, 5, lastSdt_, sdtVersion_);
        s[5] = (uint8_t)(0xC1 | (sdtVersion_ << 1));
        putCrc(s);
        queueSection(0x11, s, sdtCc_);
    }
}

void TsRemux::queueSection(int pid, const std::vector<uint8_t>& sec, int& cc) {
    if (psi_.size() > 64) return;   // no free slots for a long while: do not pile up old tables
    size_t at = 0;
    bool first = true;
    while (at < sec.size()) {
        std::array<uint8_t, 188> pk{};
        pk.fill(0xFF);
        pk[0] = 0x47;
        pk[1] = (uint8_t)((first ? 0x40 : 0) | ((pid >> 8) & 0x1F));
        pk[2] = (uint8_t)(pid & 0xFF);
        pk[3] = (uint8_t)(0x10 | (cc & 0x0F));
        cc = (cc + 1) & 0x0F;
        size_t o = 4;
        if (first) pk[o++] = 0;   // pointer field
        const size_t n = std::min(sec.size() - at, (size_t)188 - o);
        std::copy_n(sec.begin() + (std::ptrdiff_t)at, n, pk.begin() + (std::ptrdiff_t)o);
        at += n;
        first = false;
        psi_.push_back(pk);
    }
}

} // namespace dect2
