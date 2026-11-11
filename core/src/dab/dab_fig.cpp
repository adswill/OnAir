// FIG decoders. Ported from the FIG analyser of etisnoop (https://github.com/Opendigitalradio/etisnoop), GPL-3.0-or-later:
//   Copyright (C) 2014 CSP Innovazione nelle ICT s.c.a r.l. (http://www.csp.it/)
//   Copyright (C) 2018 Matthias P. Braendli (http://www.opendigitalradio.org)
//   Copyright (C) 2015 Data Path
// Each decoder works on one FIG data field copied into a zero padded buffer, so a truncated FIG reads zeros and ends its loops instead
// of reading outside it; the global databases of etisnoop (international table, main identifier positions, label segments) come in
// through DabFigContext.
#include "dab_fig_internal.h"

namespace dect2 {


// ------------------------------------------------------------------ public
const char* dabFigName(int type, int ext) {
    if (type == 0) {
        switch (ext) {
            case 0: return "Ensemble information";
            case 1: return "Basic sub-channel organization";
            case 2: return "Basic service and service component definition";
            case 3: return "Service component in packet mode";
            case 4: return "Service component with conditional access";
            case 5: return "Service component language";
            case 6: return "Service linking information";
            case 7: return "Configuration information";
            case 8: return "Service component global definition";
            case 9: return "Country, LTO and international table";
            case 10: return "Date and time";
            case 11: return "Region definition";
            case 13: return "User application information";
            case 14: return "FEC sub-channel organization";
            case 16: return "Programme number";
            case 17: return "Programme type";
            case 18: return "Announcement support";
            case 19: return "Announcement switching";
            case 20: return "Service component information";
            case 21: return "Frequency information";
            case 22: return "Transmitter identification information (TII) database";
            case 24: return "Other ensembles services";
            case 25: return "Other ensembles announcement support";
            case 26: return "Other ensembles announcement switching";
            case 27: return "FM announcement support";
            case 28: return "FM announcement switching";
            case 31: return "FIC re-direction";
            default: return "Reserved";
        }
    }
    if (type == 1 || type == 2) {
        switch (ext) {
            case 0: return "Ensemble label";
            case 1: return "Programme service label";
            case 4: return "Service component label";
            case 5: return "Data service label";
            case 6: return "X-PAD user application label";
            default: return "Reserved";
        }
    }
    if (type == 5) return "Fast information data channel (FIDC)";
    if (type == 6) return "Conditional access";
    return "Reserved";
}

std::string dabFigHeader(const DabFigRecord& r) {
    if (r.type == 0) return pf("0/%d  C/N %d  OE %d  P/D %d", r.ext, r.cn, r.oe, r.pd);
    if (r.type == 1 || r.type == 2) return pf("%d/%d  OE %d", r.type, r.ext, r.oe);
    return pf("%d", r.type);
}

DabFigContext dabFigContext(const DabFicAnalysis& a) {
    DabFigContext c;
    c.all = &a.figs;
    for (const DabFigRecord& r : a.figs) {
        if (r.type != 0 || r.data.size() < 4) continue;
        if (r.ext == 9 && r.data.size() >= 4) c.intlTable = r.data[3];
        else if (r.ext == 22) {   // main identifiers: latitude and longitude, for the offsets of the sub identifiers
            const uint8_t* f = r.data.data();
            const int n = (int)r.data.size();
            for (int i = 1; i < n;) {
                const int ms = f[i] >> 7, main = f[i] & 0x7F;
                i++;
                if (!ms) {
                    if (i + 5 > n) break;
                    const int16_t lac = (int16_t)((f[i] << 8) | f[i + 1]), loc = (int16_t)((f[i + 2] << 8) | f[i + 3]);
                    c.mainPos[(r.oe << 8) | (r.pd << 7) | main] = {(double)(((int32_t)lac << 4) | (f[i + 4] >> 4)) * 90 / 524288,
                                                                    (double)(((int32_t)loc << 4) | (f[i + 4] & 15)) * 180 / 524288};
                    i += 5;
                } else {
                    if (i >= n) break;
                    i += 1 + (f[i] & 7) * 6;
                }
            }
        }
    }
    return c;
}

DabFigDecoded dabFigDecode(const DabFigRecord& r, const DabFigContext& ctx) {
    DabFigDecoded out;
    uint8_t buf[96] = {};
    const int len = (int)std::min<size_t>(r.data.size(), 32);
    std::copy(r.data.begin(), r.data.begin() + len, buf);
    if (len < 1) { out.errors.push_back("empty FIG"); return out; }
    Fig g{buf, len, r.cn, r.oe, r.pd, &ctx, &out};
    // the shortest data field each decoder can read without running past the padding
    auto need = [&](int n) { if (len >= n) return true; g.err(pf("FIG too short (%d bytes, %d expected)", len, n)); return false; };
    bool done = true;
    if (r.type == 0) {
        switch (r.ext) {
            case 0: if (need(5)) fig0_0(g); break;
            case 1: fig0_1(g); break;
            case 2: if (need(3)) fig0_2(g); break;
            case 3: fig0_3(g); break;
            case 5: fig0_5(g); break;
            case 6: fig0_6(g); break;
            case 7: fig0_7(g); break;
            case 8: fig0_8(g); break;
            case 9: fig0_9(g); break;
            case 10: if (need(5)) fig0_10(g); break;
            case 11: fig0_11(g); break;
            case 13: if (need(g.pd ? 6 : 4)) fig0_13(g); break;
            case 14: fig0_14(g); break;
            case 16: fig0_16(g); break;
            case 17: fig0_17(g); break;
            case 18: fig0_18(g); break;
            case 19: fig0_19(g); break;
            case 21: fig0_21(g); break;
            case 22: fig0_22(g); break;
            case 24: fig0_24(g); break;
            case 25: fig0_25(g); break;
            case 26: fig0_26(g); break;
            case 27: fig0_27(g); break;
            case 28: fig0_28(g); break;
            case 31: fig0_31(g); break;
            default: done = false; out.errors.push_back(pf("FIG 0/%d: no decoder", r.ext)); break;
        }
    } else if (r.type == 1) {
        fig1(g, buf[0] >> 4, r.ext);
    } else if (r.type == 2) {
        fig2(g, buf[0] >> 7, (buf[0] >> 4) & 7, (buf[0] >> 3) & 1, r.ext);
    } else {
        done = false;
    }
    if (!done || (out.lines.empty() && r.type != 1 && r.type != 2)) out.lines.push_back({0, "data: " + hexOf(r.data.data(), r.data.size())});
    return out;
}

const char* dabLanguageName(int code) { return language((unsigned)code); }
const char* dabPtyName(int table, int pty) { return ptyName(table, (unsigned)pty); }
const char* dabAnnouncementName(int bit) { return bit >= 0 && bit < 16 ? kAnnounce[bit] : "?"; }
std::string dabUserAppName(int type) { return userApp(type); }

DabFicInfo dabFicInfo(const DabFicAnalysis& a) {
    DabFicInfo c;
    auto sid16 = [](const uint8_t* p) { return (uint32_t)(p[0] << 8) | p[1]; };
    // 0/9 first: the international table names the programme types
    for (const DabFigRecord& r : a.figs) if (r.type == 0 && r.ext == 9 && r.data.size() >= 4) c.intlTable = r.data[3];
    for (const DabFigRecord& r : a.figs) {
        if (r.type != 0) continue;
        uint8_t f[96] = {};
        const int len = (int)std::min<size_t>(r.data.size(), 32);
        std::copy(r.data.begin(), r.data.begin() + len, f);
        const bool pd = r.pd;
        auto u32 = [&](int i) { return ((uint32_t)f[i] << 24) | ((uint32_t)f[i + 1] << 16) | ((uint32_t)f[i + 2] << 8) | f[i + 3]; };
        switch (r.ext) {
            case 0:
                if (len >= 5) { c.country = f[1] >> 4; c.eref = (f[1] & 15) * 256 + f[2]; c.alarm = (f[3] >> 5) & 1; c.cifHigh = f[3] & 0x1F; c.cifLow = f[4]; }
                break;
            case 5:
                for (int i = 1; i < len - 1;) {
                    if (!(f[i] >> 7)) { if (!((f[i] >> 6) & 1)) c.subLanguage[f[i] & 0x3F] = f[i + 1]; i += 2; } else i += 3;
                }
                break;
            case 6:
                for (int i = 1; i < len - 1;) {
                    DabFicInfo::Link l;
                    const int idl = f[i] >> 7;
                    l.active = (f[i] >> 6) & 1; l.hard = (f[i] >> 5) & 1; l.intl = (f[i] >> 4) & 1; l.lsn = ((f[i] & 15) << 8) | f[i + 1];
                    i += 2;
                    if (idl && i < len) {
                        const int n = f[i] & 15;
                        const int w = pd ? 4 : l.intl ? 3 : 2;
                        for (int j = 0; j < n && i + 1 + w * (j + 1) <= len; j++) {
                            const int o = i + 1 + w * j;
                            l.ids.push_back(w == 2 ? sid16(f + o) : w == 4 ? u32(o) : ((uint32_t)f[o] << 16) | sid16(f + o + 1));
                        }
                        i += n * w + 1;
                    }
                    c.links.push_back(l);
                }
                break;
            case 7: if (len == 3) { c.services = (f[1] << 8 | f[2]) >> 10; c.reconfCount = (f[1] << 8 | f[2]) & 0x3FF; } break;
            case 9:
                if (len >= 4) { c.hasLto = true; c.ltoHalfHours = f[1] & 0x3F; if (c.ltoHalfHours & 0x20) c.ltoHalfHours -= 64; c.hasEcc = true; c.ecc = f[2]; }
                break;
            case 10:
                if (len >= 5) {
                    c.hasTime = true;
                    c.mjd = ((f[1] & 0x7F) << 10) | (f[2] << 2) | (f[3] >> 6);
                    c.hour = ((f[3] & 7) << 2) | (f[4] >> 6); c.minute = f[4] & 0x3F;
                    c.second = (f[3] & 8) && len >= 6 ? f[5] >> 2 : -1;
                }
                break;
            case 13: {
                int k = 1;
                if (len < (pd ? 6 : 4)) break;
                uint32_t sid = pd ? u32(k) : sid16(f + k);
                k += pd ? 4 : 2;
                const int no = f[k] & 15;
                k++;
                for (int n = 0; n < no && k + 2 <= len; n++) {
                    c.userApps[sid].push_back((f[k] << 3) | (f[k + 1] >> 5));
                    k += 2 + (f[k + 1] & 0x1F);
                }
                break;
            }
            case 14: for (int i = 1; i < len; i++) c.subFec[f[i] >> 2] = f[i] & 3; break;
            case 17:
                for (int i = 1; i < len - 3;) {
                    const uint32_t sid = sid16(f + i);
                    const int l = (f[i + 2] >> 5) & 1, cc = (f[i + 2] >> 4) & 1;
                    i += 3;
                    if (l) { c.language[sid] = f[i]; i++; }
                    if (i < len) { c.pty[sid] = f[i] & 0x1F; i++; }
                    if (cc) i++;
                }
                break;
            case 18: for (int i = 1; i < len - 4; i += 5 + (f[i + 4] & 0x1F)) c.announce[sid16(f + i)] = sid16(f + i + 2); break;
            case 21:
                for (int i = 1; i < len;) {
                    const int flen = f[i + 1] & 0x1F;
                    i += 2;
                    const int end = std::min(len, i + flen);
                    while (i + 3 <= end) {
                        DabFicInfo::Freq q;
                        q.id = sid16(f + i); q.rm = f[i + 2] >> 4;
                        const int nfl = f[i + 2] & 7;
                        i += 3;
                        const int fe = std::min(end, i + nfl);
                        if (q.rm == 0 || q.rm == 1) for (; i + 3 <= fe; i += 3) { const unsigned v = 16u * (((unsigned)(f[i] & 7) << 16) | (f[i + 1] << 8) | f[i + 2]); if (v) q.kHz.push_back(v); }
                        else if (q.rm == 8 || q.rm == 9) for (; i < fe; i++) { if (f[i]) q.kHz.push_back(87500.0 + f[i] * 100.0); }
                        else if (q.rm == 10) for (; i < fe; i++) { if (f[i]) q.kHz.push_back(f[i] < 16 ? 144.0 + f[i] * 9 : 387.0 + f[i] * 9); }
                        else if (q.rm == 12) for (; i + 2 <= fe; i += 2) { const unsigned v = 5 * sid16(f + i); if (v) q.kHz.push_back(v); }
                        i = fe;
                        c.freqs.push_back(q);
                    }
                    i = end;
                }
                break;
            case 22:
                if (r.oe) break;   // the transmitters of another ensemble are not this one's
                for (int i = 1; i < len;) {
                    const int ms = f[i] >> 7, main = f[i] & 0x7F;
                    i++;
                    if (!ms) {
                        if (i + 5 > len) break;
                        const int16_t lac = (int16_t)(f[i] << 8 | f[i + 1]), loc = (int16_t)(f[i + 2] << 8 | f[i + 3]);
                        c.transmitters.push_back({main, (double)(((int32_t)lac << 4) | (f[i + 4] >> 4)) * 90 / 524288, (double)(((int32_t)loc << 4) | (f[i + 4] & 15)) * 180 / 524288});
                        i += 5;
                    } else {
                        if (i >= len) break;
                        const int n = f[i] & 7;
                        i++;
                        for (int j = 0; j < n && i + 6 <= len; j++, i += 6) {
                            DabFicInfo::SubTx t;
                            t.mainId = main; t.subId = f[i] >> 3; t.delayUs = ((f[i] & 7) << 8) | f[i + 1];   // TD: 11 bits (figure 53)
                            t.lat = (int16_t)(f[i + 2] << 8 | f[i + 3]) * 90.0 / 524288; t.lng = (int16_t)(f[i + 4] << 8 | f[i + 5]) * 180.0 / 524288;
                            c.subTransmitters.push_back(t);
                        }
                    }
                }
                break;
            case 11:   // region definitions (8.1.16.1): GATy 0 a TII list, GATy 1 a rectangle; a region may come in both kinds
                if (r.oe) break;
                for (int i = 1; i + 2 <= len;) {
                    const int gaty = f[i] >> 4, global = (f[i] >> 3) & 1, id = ((f[i] & 7) << 8) | f[i + 1];
                    i += 2;
                    DabFicInfo::Region* g = nullptr;
                    for (auto& q : c.regions) if (q.id == id) g = &q;
                    if (!g) { c.regions.emplace_back(); g = &c.regions.back(); g->id = id; }
                    g->global = global;
                    if (gaty == 0) {
                        if (i >= len) break;
                        const int end = std::min(len, i + 1 + (f[i] & 0x1F));   // length of the TII list in bytes, after this byte
                        i++;
                        while (i + 2 <= end) {
                            const int main = f[i] & 0x7F, nb = f[i + 1] & 0x1F;   // length of the SubId list in bytes: 5-bit SubIds, zero padding
                            i += 2;
                            const int se = std::min(end, i + nb);
                            for (int bit = 0; (i * 8 + bit + 5) <= se * 8; bit += 5) {
                                const int at = i * 8 + bit;
                                const int v = (((f[at / 8] << 8) | f[at / 8 + 1]) >> (11 - at % 8)) & 0x1F;
                                if (v >= 1 && v <= 23) g->tii.push_back({main, v});
                            }
                            i = se;
                        }
                        i = end;
                    } else if (gaty == 1) {
                        if (i + 7 > len) break;
                        const int16_t la = (int16_t)(f[i] << 8 | f[i + 1]), lo = (int16_t)(f[i + 2] << 8 | f[i + 3]);
                        const int el = (f[i + 4] << 4) | (f[i + 5] >> 4), eo = ((f[i + 5] & 15) << 8) | f[i + 6];
                        g->hasBox = true; g->lat = la * 90.0 / 32768; g->lng = lo * 180.0 / 32768; g->extLat = el * 90.0 / 32768; g->extLng = eo * 180.0 / 32768;
                        i += 7;
                    } else {   // reserved kinds carry their length like GATy 0
                        if (i >= len) break;
                        i += 1 + (f[i] & 0x1F);
                    }
                }
                break;
            case 24:
                for (int i = 1; i < len - (pd + 1) * 2;) {
                    const uint32_t sid = pd ? u32(i) : sid16(f + i);
                    i += pd ? 4 : 2;
                    const int n = f[i] & 15;
                    i++;
                    auto& v = c.otherEnsembles[sid];
                    for (int j = 0; j < n && i + 2 <= len; j++, i += 2) v.push_back((uint16_t)sid16(f + i));
                }
                break;
            default: break;
        }
    }
    for (const DabFigRecord& r : a.figs)   // FIG 1/3: region labels, by the lower six bits of the region id (8.1.16.2)
        if (r.type == 1 && r.ext == 3 && !r.oe && r.data.size() >= 20) {
            const std::string l = dab_fig_detail::labelUtf8(r.data[0] >> 4, r.data.data() + r.data.size() - 18, 16);
            for (auto& g : c.regions) if ((g.id & 0x3F) == (r.data[1] & 0x3F)) g.label = l;
        }
    for (auto& t : c.subTransmitters)   // the offset is from the position of the main identifier
        for (const auto& m : c.transmitters) if (m.mainId == t.mainId) { t.lat += m.lat; t.lng += m.lng; t.placed = true; break; }
    return c;
}

std::string dabFicText(const DabFicAnalysis& a) {
    std::string s = pf("FIC: %llu frames, %llu FIBs ok, %llu bad\n\n", (unsigned long long)a.frames, (unsigned long long)a.fibOk, (unsigned long long)a.fibBad);
    const DabFigContext ctx = dabFigContext(a);
    int lt = -1, le = -1;
    for (const DabFigRecord& r : a.figs) {
        if (r.type != lt || r.ext != le) {
            s += pf("FIG %d/%d  %s\n", r.type, r.ext, dabFigName(r.type, r.ext));
            lt = r.type; le = r.ext;
        }
        s += pf("  %s  x%llu  frames %llu..%llu  FIB %d\n", dabFigHeader(r).c_str(), (unsigned long long)r.count, (unsigned long long)r.firstFrame,
                (unsigned long long)r.lastFrame, r.lastFib);
        const DabFigDecoded d = dabFigDecode(r, ctx);
        for (const DabFigLine& l : d.lines) s += std::string((size_t)(4 + 2 * l.level), ' ') + l.text + "\n";
        for (const std::string& e : d.errors) s += "    ERROR: " + e + "\n";
    }
    return s;
}

} // namespace dect2