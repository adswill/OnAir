// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Several single-programme transport streams into one multiplex (see ts_remux.h, issue #492).
#include "dect2/tv/ts_remux.h"
#include "dect2/tv/ts_proto.h"
#include <algorithm>

namespace dect2 {

namespace {

constexpr int kPmtMaxLen = 183;   // a rewritten PMT must fit one packet (pointer field + section)
constexpr size_t kNameMax = 60;

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
            x.pmtIn = -1; x.pmtSeen = false; x.map.clear(); x.credit = 0;
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
        in_[(size_t)best].credit -= 1.0;
        if (fromInput(in_[(size_t)best], best, pk)) return;
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
    if (pid == 0) {   // its PAT: where its PMT is
        if (sectionIn(pk, at, len) && pk[at] == 0x00)
            for (size_t p = at + 8; p + 4 <= at + len - 4; p += 4) {
                const int prog = (pk[p] << 8) | pk[p + 1];
                if (prog != 0) { x.pmtIn = ((pk[p + 2] & 0x1F) << 8) | pk[p + 3]; break; }
            }
        return false;
    }
    if (pid == 0x11) {   // its SDT: its own names
        if (sectionIn(pk, at, len)) readSdt(x, pk + at, (int)len);
        return false;
    }
    if (pid < 0x20) return false;   // other SI of its own (NIT, EIT, TDT ...): the multiplex does not carry them
    if (pid == x.pmtIn) {
        if (!sectionIn(pk, at, len) || pk[at] != 0x02) return false;
        const std::vector<uint8_t> sec(pk + at, pk + at + len);
        return rewritePmt(x, k, sec.data(), (int)len, pk, pk[3] & 0x0F);
    }
    for (const auto& m : x.map)
        if (m.first == pid) {
            pk[1] = (uint8_t)((pk[1] & 0xE0) | ((m.second >> 8) & 0x1F));
            pk[2] = (uint8_t)(m.second & 0xFF);
            return true;
        }
    return false;   // a PID its PMT does not name (or no PMT yet)
}

bool TsRemux::rewritePmt(In& x, int k, const uint8_t* sec, int len, uint8_t* pk, int cc) {
    if (len < 16 || len > kPmtMaxLen) return false;
    std::vector<uint8_t> o(sec, sec + len - 4);   // without its CRC
    const int base = esPidBase(k);
    std::vector<std::pair<int, int>> map;
    const int pil = ((o[10] & 0x0F) << 8) | o[11];
    size_t p = 12 + (size_t)pil;
    while (p + 5 <= o.size()) {
        const int pidIn = ((o[p + 1] & 0x1F) << 8) | o[p + 2];
        const size_t eil = (((size_t)o[p + 3] & 0x0F) << 8) | o[p + 4];
        const int pidOut = base + (int)map.size();
        if (map.size() >= 0xF0) return false;
        map.emplace_back(pidIn, pidOut);
        o[p + 1] = (uint8_t)((o[p + 1] & 0xE0) | (pidOut >> 8));
        o[p + 2] = (uint8_t)(pidOut & 0xFF);
        p += 5 + eil;
    }
    if (p != o.size() || map.empty()) return false;   // a malformed PMT: keep the last good one
    const int pcrIn = ((o[8] & 0x1F) << 8) | o[9];
    int pcrOut = 0x1FFF;
    if (pcrIn != 0x1FFF) {
        for (const auto& m : map) if (m.first == pcrIn) pcrOut = m.second;
        if (pcrOut == 0x1FFF) { pcrOut = base + 0xFF; map.emplace_back(pcrIn, pcrOut); }   // PCR on a PID of its own
    }
    o[8] = (uint8_t)((o[8] & 0xE0) | (pcrOut >> 8));
    o[9] = (uint8_t)(pcrOut & 0xFF);
    const int sid = (cfg_.firstServiceId + k) & 0xFFFF;
    o[3] = (uint8_t)(sid >> 8);
    o[4] = (uint8_t)(sid & 0xFF);
    bumpIfChanged(o, 5, x.lastPmt, x.pmtVersion);
    o[5] = (uint8_t)(0xC1 | (x.pmtVersion << 1));
    putCrc(o);
    x.map = std::move(map);
    x.pmtSeen = true;
    const int pid = pmtPid(k);
    pk[0] = 0x47;
    pk[1] = (uint8_t)(0x40 | (pid >> 8));
    pk[2] = (uint8_t)(pid & 0xFF);
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
    if (p + 5 > end) return;
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
            x.serviceType = q[0];
            x.sdtProvider.assign((const char*)q + 2, (size_t)pl);
            x.sdtName.assign((const char*)q + 3 + pl, (size_t)nl);
            return;
        }
        d += 2 + n;
    }
}

void TsRemux::queueTables() {
    if (count_ >= nextPat_) {
        nextPat_ = count_ + patEvery_;
        std::vector<uint8_t> s = {0x00, 0xB0, 0, (uint8_t)(cfg_.tsid >> 8), (uint8_t)cfg_.tsid, 0, 0, 0};
        for (size_t k = 0; k < in_.size(); k++) {
            if (!in_[k].active || !in_[k].pmtSeen) continue;   // a programme is in the PAT once its PMT is
            const int sid = (cfg_.firstServiceId + (int)k) & 0xFFFF, pid = pmtPid((int)k);
            s.push_back((uint8_t)(sid >> 8)); s.push_back((uint8_t)sid);
            s.push_back((uint8_t)(0xE0 | (pid >> 8))); s.push_back((uint8_t)pid);
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
            if (!x.active || !x.pmtSeen) continue;
            const std::string name = !x.src.name.empty() ? dvbName(x.src.name) : !x.sdtName.empty() ? x.sdtName.substr(0, kNameMax) : "Generator " + std::to_string(k + 1);
            const std::string prov = !x.src.provider.empty() ? dvbName(x.src.provider) : x.sdtProvider.substr(0, kNameMax);
            const int sid = (cfg_.firstServiceId + (int)k) & 0xFFFF;
            const size_t dlen = 2 + 3 + prov.size() + name.size();
            s.push_back((uint8_t)(sid >> 8)); s.push_back((uint8_t)sid);
            s.push_back(0xFC);                                    // no EIT
            s.push_back((uint8_t)(0x80 | ((dlen >> 8) & 0x0F)));  // running, free to air
            s.push_back((uint8_t)dlen);
            s.push_back(0x48); s.push_back((uint8_t)(dlen - 2));
            s.push_back((uint8_t)x.serviceType);
            s.push_back((uint8_t)prov.size()); s.insert(s.end(), prov.begin(), prov.end());
            s.push_back((uint8_t)name.size()); s.insert(s.end(), name.begin(), name.end());
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
