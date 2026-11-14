// Other payloads of a transport stream (see ts_data.h).
#include "dect2/tv/ts_data.h"
#include "dect2/dvbs/dvbs_ip.h"
#include "dect2/tv/ts.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dect2 {

const char* aitAppTypeName(int t) {
    switch (t) { case 0x0001: return "DVB-J (MHP)"; case 0x0002: return "DVB-HTML"; case 0x0010: return "HbbTV"; case 0x0011: return "OIPF DAE"; case 0x0004: return "ACAP-J"; case 0x0005: return "ACAP-X"; default: return "application"; }
}
const char* aitControlName(int c) {
    switch (c) { case 1: return "autostart"; case 2: return "present"; case 3: return "destroy"; case 4: return "kill"; case 5: return "prefetch"; case 6: return "remote"; case 7: return "unbound"; case 8: return "storage"; default: return "reserved"; }
}

static const char* scteCommand(int c) {
    switch (c) { case 0x00: return "splice null"; case 0x04: return "splice schedule"; case 0x05: return "splice insert"; case 0x06: return "time signal"; case 0x07: return "bandwidth reservation"; case 0xFF: return "private command"; default: return "reserved"; }
}

void TsDataScan::packet(const uint8_t* p) {
    const int pid = (p[1] & 0x1F) << 8 | p[2];
    if ((p[1] & 0x80) || pid < 0x20 || pid == 0x1FFF) return;
    const int afc = (p[3] >> 4) & 3;
    if (!(afc & 1)) return;
    const size_t at = 4 + ((afc & 2) ? 1 + (size_t)p[4] : 0);
    if (at >= 188) return;
    const bool pusi = (p[1] & 0x40) != 0;
    const bool scr = (p[3] >> 6) != 0;
    if (pids_.size() > 256 && !pids_.count(pid)) return;
    Pid& st = pids_[pid];
    st.packets++;
    if (scr) { if (st.kind.empty()) st.kind = "scrambled (not examined)"; return; }   // scrambled payloads are never looked into
    const uint8_t* d = p + at;
    const size_t n = 188 - at;
    if (pusi && n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 1) {   // a PES packet: EBU data and DVB subtitles live in private stream 1
        st.pes = true;
        if (d[3] == 0xBD && n >= 10) {
            const size_t hl = 9 + d[8];
            if (hl < n) {
                const int di = d[hl];
                if (di >= 0x10 && di <= 0x1F && hl + 3 < n) {
                    const int du = d[hl + 1];
                    st.kind = du == 0x02 ? "EBU teletext" : du == 0x03 ? "EBU teletext subtitles" : du == 0xC0 ? "EBU teletext (inverted)" : du == 0xC3 ? "EBU VPS" : du == 0xC4 ? "EBU WSS" : du == 0xC5 ? "EBU closed captions" : "EBU data";
                } else if (di == 0x20) st.kind = "DVB subtitles";
                else if (st.kind.empty()) st.kind = "PES (private stream 1)";
            }
        } else if (st.kind.empty()) {
            const int sid = d[3];
            st.kind = sid >= 0xE0 && sid <= 0xEF ? "PES video" : sid >= 0xC0 && sid <= 0xDF ? "PES audio" : "PES";
        }
        return;
    }
    if (st.pes) return;
    // sections of private PIDs
    auto emit = [&](const uint8_t* s, size_t len) { if (len >= 8 || (len >= 3 && s[0] == 0xFC)) section(pid, s, (int)len); };
    auto secLen = [](const uint8_t* s) { return 3 + (size_t)(((s[1] & 0x0F) << 8) | s[2]); };
    if (pusi) {
        const size_t ptr = d[0];
        if (1 + ptr > n) { st.partial.clear(); st.active = false; return; }
        if (st.active && ptr > 0) {
            st.partial.insert(st.partial.end(), d + 1, d + 1 + ptr);
            if (st.partial.size() >= 3 && st.partial.size() >= secLen(st.partial.data())) emit(st.partial.data(), secLen(st.partial.data()));
        }
        st.partial.clear(); st.active = false;
        const uint8_t* rest = d + 1 + ptr;
        size_t rn = n - 1 - ptr;
        while (rn >= 3 && rest[0] != 0xFF) {
            const size_t len = secLen(rest);
            if (rn >= len) { emit(rest, len); rest += len; rn -= len; }
            else { st.partial.assign(rest, rest + rn); st.active = true; break; }
        }
    } else if (st.active) {
        st.partial.insert(st.partial.end(), d, d + n);
        if (st.partial.size() >= 3 && st.partial.size() >= secLen(st.partial.data())) { emit(st.partial.data(), secLen(st.partial.data())); st.partial.clear(); st.active = false; }
        else if (st.partial.size() > 4200) { st.partial.clear(); st.active = false; }
    }
}

void TsDataScan::section(int pid, const uint8_t* s, int len) {
    Pid& st = pids_[pid];
    const int tid = s[0];
    if (tid == 0x74) { if (len >= 18 && dvbsip::crc32Mpeg2(s, (size_t)len) == 0) { st.kind = "AIT (application signalling)"; ait(pid, s, len); } }
    else if (tid == 0xFC) { st.kind = "SCTE-35 (splice information)"; scte(pid, s, len); }
    else if (tid == 0x3B || tid == 0x3C || tid == 0x3D) st.kind = "DSM-CC carousel";
    else if (tid == 0x3E) st.kind = "MPE (IP data)";
    else if (tid == 0x7E) st.kind = "discontinuity information (DIT)";
    else if (tid == 0x7F) st.kind = "selection information (SIT)";
    else if (tid == 0x71) st.kind = "running status (RST)";
    else if (tid == 0x72) st.kind = "stuffing";
    else if (tid >= 0xC0 && tid <= 0xCF) { if (st.kind.empty()) st.kind = "ATSC PSIP"; }
    else if (st.kind.empty()) { char b[40]; snprintf(b, sizeof b, "private sections (table 0x%02X)", tid); st.kind = b; }
}

void TsDataScan::ait(int pid, const uint8_t* s, int len) {
    const int type = (s[3] & 0x7F) << 8 | s[4];
    AitTable& t = ait_[{pid, type}];
    t.pid = pid; t.appType = type; t.typeName = aitAppTypeName(type); t.version = (s[5] >> 1) & 0x1F;
    const int cl = ((s[8] & 0x0F) << 8) | s[9];
    int i = 10 + cl;
    if (i + 2 > len - 4) return;
    const int al = ((s[i] & 0x0F) << 8) | s[i + 1];
    i += 2;
    const int end = std::min(len - 4, i + al);
    std::vector<AitApplication> apps;
    while (i + 9 <= end) {
        AitApplication a;
        a.orgId = (uint32_t)s[i] << 24 | (uint32_t)s[i + 1] << 16 | (uint32_t)s[i + 2] << 8 | s[i + 3];
        a.appId = s[i + 4] << 8 | s[i + 5];
        a.controlCode = s[i + 6]; a.controlName = aitControlName(a.controlCode);
        const int dl = ((s[i + 7] & 0x0F) << 8) | s[i + 8];
        i += 9;
        if (i + dl > end) break;
        int j = i;
        while (j + 2 <= i + dl) {
            const int tag = s[j], n = s[j + 1];
            if (j + 2 + n > i + dl) break;
            const uint8_t* b = s + j + 2;
            if (tag == 0x00 && n >= 1) {   // application descriptor: profiles, service bound, visibility, priority
                const int pl = b[0];
                for (int k = 0; k + 5 <= pl && 1 + k + 5 <= n; k += 5) a.profiles.push_back(b[1 + k] << 8 | b[2 + k]);
                if (1 + pl + 2 <= n) { a.visibility = (b[1 + pl] >> 5) & 3; a.priority = b[2 + pl]; }
            } else if (tag == 0x01 && n >= 4) {   // application name
                a.lang.assign((const char*)b, 3);
                const int nl = b[3];
                if (4 + nl <= n) a.name = dvbText(b + 4, nl, textContext_.withLanguage(a.lang));
            } else if (tag == 0x02 && n >= 3) {   // transport protocol
                const int proto = b[0] << 8 | b[1];
                if (proto == 1 && n >= 5) {   // object carousel: remote flag, (component tag), ...
                    const bool remote = (b[3] & 0x80) != 0;
                    char x[64];
                    if (!remote && n >= 5) snprintf(x, sizeof x, "object carousel (component tag 0x%02X)", b[4]);
                    else snprintf(x, sizeof x, "object carousel (remote)");
                    a.transports.push_back(x);
                } else if (proto == 3 && n >= 4) {   // HTTP: URL base
                    const int bl = b[3];
                    if (4 + bl <= n) { a.baseUrl.assign((const char*)b + 4, (size_t)bl); a.transports.push_back("HTTP " + a.baseUrl); }
                } else if (proto == 2) a.transports.push_back("IP multicast");
                else a.transports.push_back("transport protocol " + std::to_string(proto));
            } else if (tag == 0x15 && n >= 1) {   // simple application location: the initial path
                a.initialPath.assign((const char*)b, (size_t)n);
            }
            j += 2 + n;
        }
        i += dl;
        apps.push_back(std::move(a));
    }
    if (!apps.empty()) t.apps = std::move(apps);
}

void TsDataScan::scte(int pid, const uint8_t* s, int len) {
    if (len < 15) return;
    if (s[4] & 0x80) { return; }   // encrypted: not examined
    const int cmd = s[13];
    Scte35Cue& c = scte_[{pid, cmd}];
    c.pid = pid; c.command = cmd; c.commandName = scteCommand(cmd); c.count++;
    const uint8_t* b = s + 14;
    int n = len - 14 - 4;
    auto pts = [&](const uint8_t* q) { return (double)(((uint64_t)(q[0] & 1) << 32) | (uint64_t)q[1] << 24 | (uint64_t)q[2] << 16 | (uint64_t)q[3] << 8 | q[4]) / 90000.0; };
    if (cmd == 0x06 && n >= 1) {   // time signal: splice_time
        if (b[0] & 0x80 && n >= 5) { c.hasPts = true; c.ptsSec = pts(b); }
    } else if (cmd == 0x05 && n >= 5) {   // splice insert
        c.eventId = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
        if (!(b[4] & 0x80) && n >= 6) {
            const uint8_t f = b[5];
            c.outOfNetwork = (f & 0x80) != 0;
            const bool program = (f & 0x40) != 0, dur = (f & 0x20) != 0, imm = (f & 0x10) != 0;
            int k = 6;
            if (program && !imm && k + 1 <= n) { if (b[k] & 0x80) { if (k + 5 <= n) { c.hasPts = true; c.ptsSec = pts(b + k); } k += 5; } else k += 1; }
            if (!program && k + 1 <= n) { const int comps = b[k]; k += 1 + comps * (imm ? 1 : 6); }
            if (dur && k + 5 <= n) c.durationSec = pts(b + k);
        }
    }
}

TsDataSnapshot TsDataScan::snapshot() const {
    TsDataSnapshot o;
    for (const auto& kv : pids_) {
        if (kv.second.kind.empty()) continue;
        TsDataPid p; p.pid = kv.first; p.kind = kv.second.kind; p.packets = kv.second.packets;
        o.pids.push_back(p);
    }
    for (const auto& kv : ait_) o.ait.push_back(kv.second);
    for (const auto& kv : scte_) o.scte.push_back(kv.second);
    return o;
}

} // namespace dect2
