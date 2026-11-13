// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tv/ts.h"
#include "dect2/tv/dvb_text.h"
#include "dect2/tv/epg.h"
#include "dect2/tv/psip.h"
#include "dect2/tv/mux_change.h"
#include "dect2/tv/ts_proto.h"
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

uint32_t mpegCrc32(const uint8_t* p, int n) { return tsp::crc32(p, n); }   // the section CRC-32 lives in ts_proto (Rust)

std::string dvbText(const uint8_t* p, int n, bool gbDefault) { return dvbsi::dvbText(p, n, gbDefault); }   // the text fields: dvb_text.cpp (Rust)

const char* TsService::typeName() const {
    switch (type) {
    case 0x01: case 0x11: case 0x16: case 0x17: case 0x19: case 0x1A: case 0x1F: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: return "TV";
    case 0x02: case 0x0A: return "radio";
    case 0x0C: return "data";
    case 0x03: return "teletext";
    default: return type ? "other" : "?";
    }
}

static void buildPatPacket(int tsid, int sid, int pmtPid, int& cc, uint8_t* out) { tsp::patPacket(tsid, sid, pmtPid, cc, out); cc++; }

bool ServiceFilter::process(const uint8_t* in, const TsSnapshot* snap, uint8_t* out) {
    if (sid_ < 0) { memcpy(out, in, 188); return true; }
    if (snap && (patSid_ != sid_ || pass_.empty())) {
        for (auto& sv : snap->services) if (sv.id == sid_ && sv.havePmt) {
            pass_ = {sv.pmtPid, sv.pcrPid};
            if (!strict_) { pass_.push_back(0x11); pass_.push_back(0x12); pass_.push_back(0x14); }
            for (auto& st : sv.streams) pass_.push_back(st.pid);
            patSid_ = sv.id; patPmt_ = sv.pmtPid; patTsid_ = snap->tsid;
        }
    }
    if (pass_.empty()) return false;
    const int pid = ((in[1] & 0x1F) << 8) | in[2];
    if (pid == 0) {
        if (!(in[1] & 0x40)) return false;
        buildPatPacket(patTsid_, patSid_, patPmt_, patCc_, out);
        return true;
    }
    for (int p : pass_) if (p == pid) { memcpy(out, in, 188); return true; }
    return false;
}

void TsDemux::reset() { const bool gb = gbText_; *this = TsDemux(); gbText_ = gb; }

// A new transport stream id (or network id) on the same input means another multiplex: the services of the old one are dropped.
void TsDemux::noteIds(int tsid, int onid) {
    MuxId prev, cur;
    prev.tsid = tsid_; prev.onid = onid_;
    cur.tsid = tsid; cur.onid = onid;
    if (isDifferentMux(prev, cur)) {   // only the service state: the section being parsed lives in sec_ and must stay
        psip_ = psip::State(); services_.clear(); pmtToService_.clear(); epg_.clear(); epgExt_.clear(); network_.clear();
    }
    tsid_ = tsid;
    if (onid > 0) onid_ = onid;
}

void TsDemux::markLoss() { for (auto& kv : pids_) kv.second.resync = true; for (auto& kv : sec_) kv.second.active = false; }

void TsDemux::feed(const uint8_t* p) {
    tsp::PacketHeader hd;
    if (!tsp::packetHeader(p, 188, hd)) return;
    const int pid = hd.pid;
    const bool tei = hd.tei, pusi = hd.pusi;
    const int scr = hd.scr, afc = hd.afc, cc = hd.cc;
    total_++; totalWin_++;
    PidState& st = pids_[pid];
    st.packets++; st.bytesWin += 188;
    if (pid == 0x1FFF) { nulls_++; nullWin_++; return; }
    if (tei) { tei_++; st.tei++; return; }
    // (only packets with a payload: the adaptation field is never scrambled, and a PCR-only packet of a scrambled stream may say 00)
    if (afc & 1) {
        if (scr) st.scrWin++; else st.clrWin++;
        if (st.scrWin + st.clrWin >= 512) { st.scrWin /= 2; st.clrWin /= 2; }   // a sliding window: old packets fade
        st.scrambled = st.scrWin > st.clrWin;
    }
    if (afc & 1) {
        if (!st.resync && st.lastCc >= 0 && cc != ((st.lastCc + 1) & 15) && !(cc == st.lastCc && afc == 2)) { st.ccErrors++; ccErr_++; }
        st.lastCc = cc;
        st.resync = false;
    } else if (afc == 2) st.resync = false;
    rcs_.packet(p, streamClock_);
    if (afc & 1) { dsmcc_.packet(p); data_.packet(p); }   // carousels, AIT, SCTE-35, EBU data: clear payloads only (scrambled ones are skipped inside)
    if (!(afc & 1) || scr) return;
    const int off = hd.payloadOff;
    if (off < 0) return;
    // only PSI/SI PIDs and PMT PIDs carry sections we care about
    const bool psi = pid == 0 || pid == 1 || pid == 0x10 || pid == 0x11 || pid == 0x12 || pid == 0x14 || pid == 0x1FFB || (pid >= 0x1FC8 && pid <= 0x1FCF) || pmtToService_.count(pid) || psip_.eitPids.count(pid) || psip_.ettPids.count(pid);
    if (!psi) return;
    SectionBuf& sb = sec_[pid];
    const uint8_t* d = p + off;
    int n = 188 - off;
    if (pusi) {
        int ptr = d[0];
        // a pointer past the end of the packet: a corrupt packet that error correction missed (a Reed-Solomon miscorrection). Forget the
        // partial section and wait for the next one, rather than building a section from a backwards range.
        if (1 + ptr > n) { sb.data.clear(); sb.active = false; return; }
        if (sb.active && ptr > 0 && ptr < n) { sb.data.insert(sb.data.end(), d + 1, d + 1 + ptr); }
        // the previous section may be complete now
        auto tryEmit = [&]() {
            while (sb.data.size() >= 3) {
                int sl = ((sb.data[1] & 0x0F) << 8) | sb.data[2];
                if ((int)sb.data.size() < sl + 3) break;
                section(pid, sb.data.data(), sl + 3);
                sb.data.erase(sb.data.begin(), sb.data.begin() + sl + 3);
                if (!sb.data.empty() && sb.data[0] == 0xFF) { sb.data.clear(); break; }
            }
        };
        if (sb.active) tryEmit();
        sb.data.assign(d + 1 + ptr, d + n);
        sb.active = true;
        tryEmit();
    } else if (sb.active) {
        sb.data.insert(sb.data.end(), d, d + n);
        while (sb.data.size() >= 3) {
            int sl = ((sb.data[1] & 0x0F) << 8) | sb.data[2];
            if ((int)sb.data.size() < sl + 3) break;
            section(pid, sb.data.data(), sl + 3);
            sb.data.erase(sb.data.begin(), sb.data.begin() + sl + 3);
            if (!sb.data.empty() && sb.data[0] == 0xFF) { sb.data.clear(); break; }
        }
    }
}

void TsDemux::section(int pid, const uint8_t* s, int len) {
    if (len < 8) return;
    const int tid = s[0];
    // Every table parsed here carries a CRC_32 except the TDT (the TOT has one although its syntax indicator is 0). Decide by the
    // table id, not by the syntax bit: a corrupted section whose syntax bit reads 0 would otherwise skip the check and fill the
    // service list with phantom programs from a damaged PAT.
    if (tid != 0x70 && mpegCrc32(s, len) != 0) return;
    si_.section(pid, s, len);   // transponder lists, bouquets, other transport streams, time offsets, CAT (signalling only)
    if (tid == 0x00) parsePat(s, len);
    else if (tid == 0x02) parsePmt(pid, s, len);
    else if (tid == 0x42) parseSdt(s, len);
    else if (tid == 0x40) parseNit(s, len);
    else if (tid == 0x4E || (tid >= 0x50 && tid <= 0x5F)) parseEit(s, len);
    else if (tid == 0x70 || tid == 0x73) parseTdt(s, len);
    else if (pid == 0x1FFB && (tid == 0xC8 || tid == 0xC9)) { parseTvct(s, len); psip::parseVctSources(s, len, psip_); }
    else if (pid == 0x1FFB && tid == 0xC7) psip::parseMgt(s, len, psip_);
    else if (pid == 0x1FFB && tid == 0xCD) psip::parseStt(s, len, psip_);
    else if (psip_.eitPids.count(pid) && tid == 0xCB) psipEit(s, len);
    else if (psip_.ettPids.count(pid) && tid == 0xCC) psipEtt(s, len);
}

// ATSC A/65 virtual channel table: names and channel numbers, shown like DVB services (name, number in the provider line).
void TsDemux::parseTvct(const uint8_t* s, int len) {
    int tsid;
    std::vector<tsp::Tvct> ch;
    if (!tsp::parseTvct(s, len, tsid, ch)) return;
    noteIds(tsid, 0);
    for (const tsp::Tvct& c : ch) {
        // program 0 / 0xFFFF: an entry with no program in this stream (an analog or inactive channel some stations still list): not a service
        if (c.prog == 0 || c.prog == 0xFFFF) continue;
        TsService& sv = services_[c.prog];
        sv.id = c.prog;
        sv.name = c.name;
        sv.haveSdt = true;
        sv.lcn = c.major * 100 + c.minor;
        sv.type = c.stype == 2 ? 0x01 : c.stype == 3 ? 0x02 : 0x0C;
        char b[40];
        snprintf(b, sizeof b, "channel %d.%d", c.major, c.minor);
        sv.provider = b;
    }
}

void TsDemux::psipEit(const uint8_t* s, int len) {
    std::vector<psip::Event> ev;
    if (!psip::parseEit(s, len, psip_, ev)) return;
    for (const psip::Event& e : ev) {
        auto it = psip_.sourceToProgram.find(e.sourceId);
        if (it == psip_.sourceToProgram.end()) continue;
        auto& evs = epg_[it->second];
        EpgEvent n; n.eventId = e.eventId; n.start = e.start; n.duration = e.duration; n.running = 0; n.title = e.title;
        auto was = evs.find(e.eventId);
        if (was != evs.end()) n.extended = was->second.extended;
        const uint32_t etm = (uint32_t)e.sourceId << 16 | (uint32_t)e.eventId << 2 | 2;
        auto tx = psip_.ett.find(etm);
        if (tx != psip_.ett.end()) n.extended = tx->second;
        evs[e.eventId] = n;
    }
}

void TsDemux::psipEtt(const uint8_t* s, int len) {
    uint32_t id = 0; std::string t;
    if (!psip::parseEtt(s, len, psip_, &id, &t)) return;
    if ((id & 3) != 2) return;                                  // not an event ETM
    const int src = (int)(id >> 16), evn = (int)((id >> 2) & 0x3FFF);
    auto it = psip_.sourceToProgram.find(src);
    if (it == psip_.sourceToProgram.end()) return;
    auto& evs = epg_[it->second];
    auto e = evs.find(evn);
    if (e != evs.end()) e->second.extended = t;
}

void TsDemux::parsePat(const uint8_t* s, int len) {
    int tsid;
    std::vector<tsp::PatEntry> e;
    if (!tsp::parsePat(s, len, tsid, e)) return;
    noteIds(tsid, 0);
    for (const tsp::PatEntry& pe : e) {
        TsService& sv = services_[pe.prog];
        sv.id = pe.prog;
        sv.pmtPid = pe.pid;
        pmtToService_[pe.pid] = pe.prog;
    }
}

void TsDemux::parsePmt(int pid, const uint8_t* s, int len) {
    auto it = pmtToService_.find(pid);
    tsp::Pmt pmt;
    if (it == pmtToService_.end()) {
        // ISDB-T partial reception (one-seg): its PMT sits at a fixed PID 0x1FC8-0x1FCF (ARIB STD-B10). When the PAT has not arrived
        // (it can be carried in a layer that is not received) the service is built from the PMT itself, so the one-seg still plays.
        if (pid < 0x1FC8 || pid > 0x1FCF) return;
        if (!tsp::parsePmt(s, len, pmt)) return;
        TsService& nsv = services_[pmt.prog];
        nsv.id = pmt.prog;
        if (!nsv.pmtPid) nsv.pmtPid = pid;
        pmtToService_[pid] = pmt.prog;
        it = pmtToService_.find(pid);
    }
    TsService& sv = services_[it->second];
    if (pmt.prog == 0 && !tsp::parsePmt(s, len, pmt)) return;
    if (pmt.prog != sv.id) return;
    sv.pcrPid = pmt.pcrPid;
    std::vector<TsStream> streams;
    for (const tsp::PmtStream& ps : pmt.streams) {
        TsStream st;
        st.pid = ps.pid; st.streamType = ps.streamType; st.kind = ps.kind; st.codec = ps.codec; st.lang = ps.lang;
        for (const tsp::PmtSub& e : ps.subs) {
            TsSubEntry x; x.teletext = e.teletext; x.lang = e.lang; x.type = e.type; x.page = e.page; x.ancPage = e.ancPage;
            st.subs.push_back(x);
        }
        streams.push_back(st);
    }
    sv.streams = streams;
    sv.havePmt = true;
    sv.caFlag = sv.caFlag || pmt.ca;
}

void TsDemux::parseSdt(const uint8_t* s, int len) {
    int tsid, onid;
    std::vector<tsp::SdtService> v;
    if (!tsp::parseSdt(s, len, tsid, onid, v)) return;
    noteIds(tsid, onid);
    for (const tsp::SdtService& e : v) {
        TsService& sv = services_[e.sid];
        sv.id = e.sid;
        sv.haveSdt = true;
        sv.caFlag = sv.caFlag || e.freeCa;
        if (e.hasType) sv.type = e.type;
        if (e.hasProvider) sv.provider = dvbText(s + e.provider.off, (int)e.provider.len, gbText_);
        if (e.hasName) sv.name = dvbText(s + e.name.off, (int)e.name.len, gbText_);
    }
    // linkage descriptors (0x4A) of the service loop; type 0x05 names the service that replaces this one (the HD simulcast), read here from the raw section
    const int end = len - 4;
    for (int i = 11; i + 5 <= end;) {
        const int sid = s[i] << 8 | s[i + 1], dl = ((s[i + 3] & 0x0F) << 8) | s[i + 4];
        i += 5;
        if (i + dl > end) break;
        for (int j = i; j + 2 <= i + dl;) {
            const int tag = s[j], n = s[j + 1];
            if (j + 2 + n > i + dl) break;
            if (tag == 0x4A && n >= 7 && s[j + 8] == 0x05) {
                TsService& sv = services_[sid];
                sv.id = sid;
                sv.linkTsid = s[j + 2] << 8 | s[j + 3]; sv.linkOnid = s[j + 4] << 8 | s[j + 5]; sv.linkSid = s[j + 6] << 8 | s[j + 7];
            }
            j += 2 + n;
        }
        i += dl;
    }
}

void TsDemux::parseNit(const uint8_t* s, int len) {
    tsp::Nit nit;
    if (!tsp::parseNit(s, len, nit)) return;
    if (nit.hasName) network_ = dvbText(s + nit.name.off, (int)nit.name.len, gbText_);
    for (const tsp::NitLcn& e : nit.lcn) {   // logical channel numbers (descriptor 0x83 in private data, e.g. EACEM / UK)
        services_[e.sid].id = e.sid;
        services_[e.sid].lcn = e.lcn;
    }
}

void TsDemux::parseEit(const uint8_t* s, int len) {
    tsp::Eit eit;
    if (!tsp::parseEit(s, len, eit)) return;
    const int sid = eit.sid;
    // the guide language the user prefers (#268): where the texts of that language sit, per event
    std::map<int, EpgLangPick> picks;
    { const std::string want = epgPreferredLanguage(); if (!want.empty()) epgPickLanguage(s, len, want, picks); }
    // legacy: present / following event names for the service list
    if (eit.legacy) {
        TsService& sv = services_[sid];
        sv.id = sid;
        std::string nm = eit.hasName ? dvbText(s + eit.nowNextName.off, (int)eit.nowNextName.len, gbText_) : std::string();
        if (!eit.events.empty()) {
            auto pk = picks.find(eit.events[0].eventId);
            if (pk != picks.end() && pk->second.hasShort) nm = dvbText(s + pk->second.titleOff, pk->second.titleLen, gbText_);
        }
        (eit.secNum == 0 ? sv.now : sv.next) = nm;
    }
    // the guide: every event in the section
    auto& evs = epg_[sid];
    for (const tsp::EitEvent& pe : eit.events) {
        EpgEvent ev;
        ev.eventId = pe.eventId; ev.start = pe.start; ev.duration = pe.duration; ev.running = pe.running; ev.genre = pe.genre; ev.lang = pe.lang;
        if (pe.hasTitle) ev.title = dvbText(s + pe.title.off, (int)pe.title.len, gbText_);
        if (pe.hasText) ev.text = dvbText(s + pe.text.off, (int)pe.text.len, gbText_);
        for (int n = 0; n < 16; n++) if (pe.extMask & (1u << n)) ev.extended += dvbText(s + pe.ext[n].off, (int)pe.ext[n].len, gbText_);   // pieces in descriptor_number order
        {
            auto pk = picks.find(ev.eventId);
            if (pk != picks.end()) {
                const EpgLangPick& k = pk->second;
                if (!k.lang.empty()) ev.lang = k.lang;
                if (k.hasShort) { ev.title = dvbText(s + k.titleOff, k.titleLen, gbText_); ev.text = dvbText(s + k.textOff, k.textLen, gbText_); }
                if (!k.ext.empty()) { ev.extended.clear(); for (auto& e : k.ext) ev.extended += dvbText(s + e.first, e.second, gbText_); }
            }
        }
        auto it = evs.find(ev.eventId);
        if (it != evs.end() && ev.extended.empty()) ev.extended = it->second.extended;
        evs[ev.eventId] = ev;
    }
    // keep the table bounded: drop the oldest events
    while (evs.size() > 400) {
        auto oldest = evs.begin();
        for (auto it = evs.begin(); it != evs.end(); ++it) if (it->second.start < oldest->second.start) oldest = it;
        evs.erase(oldest);
    }
}

std::map<int, std::vector<EpgEvent>> TsDemux::epg() const {
    std::map<int, std::vector<EpgEvent>> out;
    for (auto& kv : epg_) {
        auto& v = out[kv.first];
        for (auto& e : kv.second) v.push_back(e.second);
        std::sort(v.begin(), v.end(), [](const EpgEvent& a, const EpgEvent& b) { return a.start < b.start; });
    }
    return out;
}

void TsDemux::parseTdt(const uint8_t* s, int len) {
    tsp::Tdt d;
    if (!tsp::parseTdt(s, len, d)) return;
    utc_ = d.text;
    tdtUnix_ = d.unixSec;
    tdtAt_ = streamClock_;
}

void TsDemux::advance(double dt) {
    if (dt <= 0) return;
    streamClock_ += dt;
    const double a = 0.35;
    for (auto& kv : pids_) {
        double k = kv.second.bytesWin * 8.0 / dt / 1000.0;
        kv.second.kbps = kv.second.kbps * (1 - a) + k * a;
        kv.second.bytesWin = 0;
    }
    muxKbps_ = muxKbps_ * (1 - a) + (totalWin_ * 188 * 8.0 / dt / 1000.0) * a;
    nullKbps_ = nullKbps_ * (1 - a) + (nullWin_ * 188 * 8.0 / dt / 1000.0) * a;
    totalWin_ = nullWin_ = 0;
}

TsSnapshot TsDemux::snapshot() const {
    TsSnapshot sn;
    sn.networkName = network_; sn.utc = utc_; sn.onid = onid_; sn.tsid = tsid_;
    sn.totalPackets = total_; sn.nullPackets = nulls_; sn.teiPackets = tei_; sn.ccErrors = ccErr_;
    sn.muxKbps = muxKbps_; sn.nullKbps = nullKbps_;
    sn.utcNow = tdtUnix_ ? tdtUnix_ + (int64_t)(streamClock_ - tdtAt_) : 0;
    { auto ci = dsmcc_.info(); if (!ci.empty()) sn.carousels = std::make_shared<const std::vector<DsmccCarouselInfo>>(std::move(ci)); TsDataSnapshot ds = data_.snapshot(); if (!ds.empty()) sn.data = std::make_shared<const TsDataSnapshot>(std::move(ds)); }
    if (rcs_.active()) sn.rcs = std::make_shared<const RcsView>(rcs_.view());
    { SiSnapshot si = si_.snapshot(); if (!si.empty()) sn.si = std::make_shared<const SiSnapshot>(std::move(si)); }
    std::map<int, std::string> label;
    for (auto& kv : services_) {
        TsService sv = kv.second;
        // scrambled: what the streams' packets say, not the CA signalling (a free channel can carry CA descriptors or free_CA_mode)
        sv.scrambled = false;
        CaEvidence ev; ev.flagged = sv.caFlag;
        for (auto& st : sv.streams) {
            auto it = pids_.find(st.pid);
            if (it != pids_.end()) { st.kbps = it->second.kbps; st.scrambled = it->second.scrambled; st.scrPackets = it->second.scrWin; st.clrPackets = it->second.clrWin; }
            if (st.kind == "video" || st.kind == "audio") { CaStreamEvidence se; se.video = st.kind == "video"; se.scrambledPackets = st.scrPackets; se.clearPackets = st.clrPackets; ev.streams.push_back(se); }
            label[st.pid] = (sv.name.empty() ? "service " + std::to_string(sv.id) : sv.name) + " " + st.kind + " (" + st.codec + ")";
        }
        sv.caState = classifyCa(ev);
        sv.scrambled = sv.caState == CaState::Scrambled;
        label[sv.pmtPid] = "PMT " + (sv.name.empty() ? std::to_string(sv.id) : sv.name);
        if (sv.pmtPid || sv.haveSdt || sv.havePmt) sn.services.push_back(sv);
    }
    std::sort(sn.services.begin(), sn.services.end(), [](const TsService& a, const TsService& b) {
        if (a.lcn != b.lcn) return (a.lcn ? a.lcn : 9999) < (b.lcn ? b.lcn : 9999);
        return a.id < b.id;
    });
    label[0] = "PAT"; label[1] = "CAT"; label[0x10] = "NIT"; label[0x11] = "SDT/BAT"; label[0x12] = "EIT"; label[0x14] = "TDT/TOT"; label[0x1FFF] = "null";
    label[0x13] = "RST"; label[0x1F] = "DIT"; label[0x1E] = "ST";
    for (auto& kv : pids_) {
        TsPid p;
        p.pid = kv.first; p.packets = kv.second.packets; p.ccErrors = kv.second.ccErrors; p.tei = kv.second.tei;
        p.kbps = kv.second.kbps; p.scrambled = kv.second.scrambled;
        auto it = label.find(kv.first);
        p.label = it != label.end() ? it->second : "";
        sn.pids.push_back(p);
    }
    return sn;
}

} // namespace dect2
