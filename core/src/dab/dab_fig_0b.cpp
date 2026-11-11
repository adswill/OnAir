// DAB FIG 0 decoders, part 2 (conditional access, announcements, other ensembles, ...). Ported from etisnoop (GPL-3.0-or-later).
#include "dab_fig_internal.h"

namespace dect2 {

namespace dab_fig_detail {

void fig0_16(Fig& g) {   // 8.1.4 and 8.1.10.3
    const uint8_t* f = g.f;
    for (int i = 1; i < g.len - 4;) {
        const int rfa = f[i + 4] >> 6, rfu = (f[i + 4] >> 2) & 15, cont = (f[i + 4] >> 1) & 1, upd = f[i + 4] & 1;
        g.add(0, pf("Programme number of SId 0x%X", g.u16(i)));
        g.add(1, pf("SId=0x%X", g.u16(i)));
        g.add(1, pf("PNum=0x%X ", g.u16(i + 2)) + pnum(g.u16(i + 2)));
        if (rfa) g.err(pf("Rfa=%d invalid value", rfa));
        if (rfu) g.err(pf("Rfu=0x%X invalid value", rfu));
        g.add(1, pf("Continuation flag=%d, the programme will %s", cont, cont ? "be interrupted but continued later" : "not be subject to a planned interruption"));
        g.add(1, pf("Update flag=%d %sre-direction", upd, upd ? "" : "no "));
        i += 5;
        if (upd) {
            if (i < g.len - 1) {
                g.add(1, pf("New SId=0x%X", g.u16(i)));
                if (i < g.len - 3) g.add(1, pf("New PNum=0x%X ", g.u16(i + 2)) + pnum(g.u16(i + 2)));
                else g.err("missing New PNum !");
            } else g.err("missing New SId and New PNum !");
            i += 4;
        }
    }
}

void fig0_17(Fig& g) {   // 8.1.5
    const uint8_t* f = g.f;
    for (int i = 1; i < g.len - 3;) {
        const int sd = f[i + 2] >> 7, ps = (f[i + 2] >> 6) & 1, l = (f[i + 2] >> 5) & 1, cc = (f[i + 2] >> 4) & 1;
        g.add(0, pf("Programme type of SId 0x%X", g.u16(i)));
        g.add(1, pf("SId=0x%X", g.u16(i)));
        g.add(1, pf("S/D=%d Programme Type codes and language (when present), %srepresent the current programme contents", sd, sd ? "" : "may not "));
        g.add(1, pf("P/S=%d %s service component", ps, ps ? "secondary" : "primary"));
        g.add(1, pf("L flag=%d language field %s", l, l ? "present" : "absent"));
        g.add(1, pf("CC flag=%d complementary code and preceding Rfa and Rfu fields %s", cc, cc ? "present" : "absent"));
        if (f[i + 2] & 15) g.err(pf("Rfa=0x%X invalid value", f[i + 2] & 15));
        i += 3;
        if (l) {
            if (i < g.len) g.add(1, pf("Language=0x%X %s", f[i], language(f[i])));
            else g.err("Language= invalid FIG length");
            i++;
        }
        if (i < g.len) {
            if (f[i] >> 6) g.err(pf("Rfa=0x%X invalid value", f[i] >> 6));
            if ((f[i] >> 5) & 1) g.err("Rfu=1 invalid value");
            g.add(1, pf("Int code=0x%X %s", f[i] & 0x1F, ptyName(g.ctx->intlTable, f[i] & 0x1F)));
            i++;
        } else g.err("Int code: invalid FIG length");
        if (cc) {
            if (i < g.len) {
                if (f[i] >> 6) g.err(pf("Rfa=0x%X invalid value", f[i] >> 6));
                if ((f[i] >> 5) & 1) g.err("Rfu=1 invalid value");
                g.add(1, pf("Comp code=0x%X %s", f[i] & 0x1F, ptyName(g.ctx->intlTable, f[i] & 0x1F)));
                i++;
            } else g.err("Comp code= invalid FIG length");
        }
    }
}

void fig0_18(Fig& g) {   // 8.1.6.1
    const uint8_t* f = g.f;
    for (int i = 1; i < g.len - 4;) {
        const unsigned asu = g.u16(i + 2);
        const int ncl = f[i + 4] & 0x1F;
        g.add(0, pf("Announcement support of SId 0x%X", g.u16(i)));
        g.add(1, pf("SId=0x%X", g.u16(i)));
        g.add(1, pf("Asu flags=0x%04x", asu));
        if (f[i + 4] >> 5) g.err(pf("Rfa=%d invalid value", f[i + 4] >> 5));
        g.add(1, pf("Number of clusters=%d", ncl));
        g.add(1, pf("database key=0x%05x", (g.oe << 17) | (g.pd << 16) | g.u16(i)));
        if (!ncl && !asu) g.add(1, "CEI=true");
        i += 5;
        std::string cl;
        int j = 0;
        for (; j < ncl && i < g.len; j++, i++) cl += pf(j ? ", 0x%X" : "0x%X", f[i]);
        g.add(1, "Cluster Ids: [" + cl + "]");
        if (j < ncl) g.err("missing Cluster Id, fig length too short !");
        g.add(1, "Announcements:");
        g.announce(2, asu);
    }
}

void fig0_19(Fig& g) {   // 8.1.6.2
    const uint8_t* f = g.f;
    for (int i = 1; i < g.len - 3;) {
        const unsigned asw = g.u16(i + 1);
        const int nw = f[i + 3] >> 7, rg = (f[i + 3] >> 6) & 1;
        g.add(0, pf("Announcement switching, cluster 0x%02x", f[i]));
        g.add(1, pf("Cluster Id=0x%02x", f[i]));
        g.add(1, pf("Asw flags=0x%04x", asw));
        g.add(1, pf("New flag=%d %s", nw, nw ? "new" : "repeat"));
        g.add(1, pf("Region flag=%d last byte %s", rg, rg ? "present" : "absent"));
        g.add(1, pf("SubChId=%d", f[i + 3] & 0x3F));
        if (rg) {
            if (i < g.len - 4) {
                if (f[i + 4] >> 6) g.err(pf("Rfa=%d invalid value", f[i + 4] >> 6));
                g.add(1, pf("Region Lower Part=0x%02x", f[i + 4] & 0x3F));
            } else g.err("missing Region Lower Part, fig length too short !");
        }
        g.add(1, "Announcement switching:");
        g.announce(2, asw);
        i += 4 + rg;
    }
}

void fig0_21(Fig& g) {   // 8.1.8
    const uint8_t* f = g.f;
    int i = 1;
    while (i < g.len) {
        const int region = (f[i] << 3) | (f[i + 1] >> 5), flen = f[i + 1] & 0x1F;
        g.add(0, pf("Frequency information, region 0x%03x", region));
        g.add(1, pf("RegionId=0x%03x", region));
        g.add(1, pf("Len=%d Bytes", flen));
        i += 2;
        const int start = i;
        g.add(1, "FIs:");
        for (int fi = 0; i < start + flen; fi++) {
            if (i + 3 > g.len) { g.err("FIG0/21 too small!"); break; }
            const unsigned id = g.u16(i);
            const int rm = f[i + 2] >> 4, cont = (f[i + 2] >> 3) & 1, nfl = f[i + 2] & 7;
            g.add(2, pf("Frequency list %d", fi));
            g.add(3, pf("Length Freq list=%d", nfl));
            i += 3;
            const char* idf = "invalid";
            switch (rm) {
                case 0: case 1: idf = "EId"; break;
                case 6: idf = "DRM Service Id"; break;
                case 8: idf = "RDS PI"; break;
                case 9: case 10: case 12: idf = "Dummy"; break;
                case 14: idf = "AMSS Service Id"; break;
                default: g.err("R&M invalid"); break;
            }
            g.add(3, pf("ID field=0x%X %s", id, idf));
            const char* rms = "Rfu";
            switch (rm) {
                case 0: rms = "DAB ensemble, no local windows"; break;
                case 6: rms = "DRM"; break;
                case 8: rms = "FM with RDS"; break;
                case 9: rms = "FM without RDS"; break;
                case 10: rms = "AM (MW in 9 kHz steps & LW)"; break;
                case 12: rms = "AM (MW in 5 kHz steps & SW)"; break;
                case 14: rms = "AMSS"; break;
                default: g.err("R&M is Rfu"); break;
            }
            g.add(3, pf("R&M=0x%1x %s", rm, rms));
            std::string cs;
            if (g.oe == 0 || (rm != 6 && (rm < 8 || rm > 10) && rm != 12 && rm != 14)) {
                if (rm == 0 || rm == 1) cs = cont ? ", continuous output possible" : ", continuous output not expected";
                else if (rm == 6) cs = cont ? ", compensating time delay on DRM audio signal" : ", no compensating time delay on DRM audio signal";
                else if (rm == 8 || rm == 9) cs = cont ? ", compensating time delay on FM audio signal" : ", no compensating time delay on FM audio signal";
                else if (rm == 10 || rm == 12 || rm == 14) cs = cont ? ", compensating time delay on AM audio signal" : ", no compensating time delay on AM audio signal";
                else cs = ", Rfu";
            } else { cs = ", reserved for future addition"; g.err("Rfu"); }
            g.add(3, pf("Continuity flag=%d", cont) + cs);
            g.add(3, pf("database key=0x%09llX", ((unsigned long long)g.oe << 32) | ((unsigned long long)g.pd << 31) | ((unsigned long long)region << 20) | ((unsigned long long)id << 4) | (unsigned)rm));
            if (!nfl) g.add(3, "CEI=true");
            g.add(3, "Frequency Information:");
            switch (rm) {
                case 0: case 1: {
                    if (nfl % 3) g.err("Length of freq list incorrect size");
                    for (int k = 0; k < nfl / 3; k++) {
                        g.add(4, "-");
                        if (i + 3 > g.len) { g.err(pf("FIG 0/21 too small for FI %d, freq %d", fi, k)); break; }
                        const int ctl = f[i] >> 3;
                        const unsigned freq = 16u * (((unsigned)(f[i] & 7) << 16) | ((unsigned)f[i + 1] << 8) | f[i + 2]);
                        i += 3;
                        if (!freq) { g.err(pf("Frequency not to be used (0) in FI %d, freq %d", fi, k)); continue; }
                        const int tm = (ctl >> 1) & 7;
                        if (!(ctl & 0x10)) {
                            g.add(5, pf("%u kHz", freq));
                            g.add(5, (ctl & 1) ? "no geographically adjacent area" : "geographically adjacent area");
                            if (tm == 0) g.add(5, "no transmission mode signalled");
                            else if (tm <= 4) g.add(5, pf("transmission mode %d", tm));
                            else g.add(5, pf("invalid transmission mode 0x%x", tm));
                        } else g.add(5, pf("%u kHz, invalid Control field b23 0x%x", freq, ctl));
                    }
                    break;
                }
                case 8: case 9: case 10:
                    for (int k = 0; k < nfl; k++, i++) {
                        g.add(4, "-");
                        if (i + 1 > g.len) { g.err(pf("FIG 0/21 too small for FI %d, freq %d", fi, k)); break; }
                        const unsigned fr = f[i];
                        if (!fr) { g.err("Frequency not to be used (0)"); continue; }
                        if (rm == 10) g.add(5, pf("%u kHz", fr < 16 ? 144 + fr * 9 : 387 + fr * 9));
                        else g.add(5, pf("%.1f MHz", 87.5 + fr * 0.1));
                    }
                    break;
                case 12:
                    if (nfl % 2) g.err("Length of freq list incorrect size");
                    for (int k = 0; k < nfl / 2; k++, i += 2) {
                        g.add(4, "-");
                        if (i + 2 > g.len) { g.err(pf("FIG 0/21 too small for FI %d, freq %d", fi, k)); break; }
                        const unsigned fr = 5 * g.u16(i);
                        if (fr) g.add(5, pf("%u kHz", fr)); else g.err("Frequency not to be used (0)");
                    }
                    break;
                case 6: case 14: {
                    if (i + 1 > g.len) { g.err(pf("FIG 0/21 too small for control of FI %d", fi)); break; }
                    const int nf = std::max(0, nfl - 1) / 2;
                    if (nfl < 1 || (nfl - 1) % 2) g.err("Length of freq list incorrect size");
                    const uint32_t id2 = f[i++];
                    for (int k = 0; k < nf; k++, i += 2) {
                        g.add(4, "-");
                        if (i + 2 > g.len) { g.err(pf("FIG 0/21 too small for FI %d, freq %d", fi, k)); break; }
                        const unsigned fr = ((f[i] & 0x7F) << 8) | f[i + 1];
                        if (fr) g.add(5, pf("%u kHz", fr)); else g.err("Frequency not to be used (0)");
                        g.add(5, pf("%s Service Id 0x%X", rm == 6 ? "DRM" : "AMSS", (id2 << 16) | id));
                    }
                    break;
                }
                default:
                    g.err("Invalid R&M");
                    i += nfl;   // skip the list: its layout is unknown
                    break;
            }
        }
    }
}

void fig0_22(Fig& g) {   // 8.1.9
    const uint8_t* f = g.f;
    for (int i = 1; i < g.len;) {
        const int ms = f[i] >> 7, main = f[i] & 0x7F;
        const int key = (g.oe << 8) | (g.pd << 7) | main;
        g.add(0, pf("%s identifier 0x%X", ms ? "Sub" : "Main", main));
        g.add(1, pf("M/S=%d %sidentifier", ms, ms ? "Sub-" : "Main "));
        g.add(1, pf("MainId=0x%X", main));
        mainIdCheck(g, main);
        g.add(1, pf("database key=0x%X", key));
        i++;
        if (!ms) {
            if (i < g.len - 4) {
                const int16_t lac = (int16_t)g.u16(i), loc = (int16_t)g.u16(i + 2);
                const int laf = f[i + 4] >> 4, lof = f[i + 4] & 15;
                const double lat = (double)(((int32_t)lac << 4) | laf) * 90 / 524288, lng = (double)(((int32_t)loc << 4) | lof) * 180 / 524288;
                g.add(1, pf("Lat Lng coarse=0x%X 0x%X, Lat Lng fine=0x%X 0x%X => Lat Lng=%f, %f", (unsigned)(uint16_t)lac, (unsigned)(uint16_t)loc, laf, lof, lat, lng));
                i += 5;
            } else { g.err("invalid length of Latitude Longitude coarse fine"); break; }
        } else {
            if (i >= g.len) { g.err("invalid fig length or Number of SubId fields length"); break; }
            if (f[i] >> 3) g.err(pf("Rfu=%d invalid value", f[i] >> 3));
            const int n = f[i] & 7;
            g.add(1, pf("Number of SubId fields=%d%s", n, n ? "" : ", CEI"));
            i++;
            g.add(1, "SubId Fields:");
            const auto pos = g.ctx->mainPos.find(key);
            for (int j = i; j < i + n * 6 && j < g.len - 5; j += 6) {
                const int sub = f[j] >> 3, td = ((f[j] & 7) << 8) | f[j + 1];   // TD: 11 bits (EN 300 401 V1.4.1 figure 53)
                const int16_t dla = (int16_t)g.u16(j + 2), dlo = (int16_t)g.u16(j + 4);
                g.add(2, pf("Transmitter SubId 0x%X", sub));
                g.add(3, pf("SubId=0x%X", sub));
                if (sub == 0 || sub > 23) g.err("invalid value");
                g.add(3, pf("TD=%d us", td));
                g.add(3, pf("Lat Lng offset=0x%X 0x%X", (unsigned)(uint16_t)dla, (unsigned)(uint16_t)dlo));
                double lat = 90.0 * dla / 524288, lng = 180.0 * dlo / 524288;
                if (pos != g.ctx->mainPos.end()) {
                    g.add(3, pf("Lat Lng=%f, %f", lat + pos->second.first, lng + pos->second.second));
                } else {
                    g.add(3, pf("Lat Lng=%f, %f wrong value because Main identifier latitude/longitude not available", lat, lng));
                }
            }
            i += n * 6;
        }
    }
}

void fig0_24(Fig& g) {   // 8.1.10.2
    const uint8_t* f = g.f;
    for (int i = 1; i < g.len - (g.pd + 1) * 2;) {
        uint32_t sid;
        if (!g.pd) { sid = g.u16(i); i += 2; } else { sid = g.u32(i); i += 4; }
        const int n = f[i] & 15;
        g.add(0, pf("Other ensembles of service 0x%X", sid));
        g.add(1, pf("PD=%d", g.pd));
        g.add(1, pf("SId=0x%X", sid));
        g.add(1, pf("CAId=%d", (f[i] >> 4) & 7));
        g.add(1, pf("Number of EId=%d", n));
        g.add(1, pf("database key=%09llu", ((unsigned long long)g.oe << 33) | ((unsigned long long)g.pd << 32) | sid));
        if (f[i] >> 7) g.err("Rfa=1 invalid value");
        if (!n) g.add(1, "CEI=true");
        i++;
        std::string e;
        for (int j = i, k = 0; j < i + n * 2 && j < g.len; j += 2, k++) e += pf(k ? ", 0x%04x" : "0x%04x", g.u16(j));
        g.add(1, "EIds: [" + e + "]");
        i += n * 2;
    }
}

void fig0_25(Fig& g) {   // 8.1.10.5.1
    const uint8_t* f = g.f;
    for (int i = 1; i < g.len - 4;) {
        const unsigned asu = g.u16(i + 2);
        const int n = f[i + 4] & 15;
        g.add(0, pf("Other ensemble announcement support of SId 0x%X", g.u16(i)));
        g.add(1, pf("SId=0x%X", g.u16(i)));
        g.add(1, pf("Asu flags=0x%X", asu));
        g.add(1, pf("Number of EIds=%d", n));
        if (f[i + 4] >> 4) g.err(pf("Rfu=%d invalid value", f[i + 4] >> 4));
        g.add(1, pf("database key=0x%05x", (g.oe << 17) | (g.pd << 16) | g.u16(i)));
        if (!n) g.add(1, "CEI=true");
        i += 5;
        std::string e;
        int j = 0;
        for (; j < n && i < g.len - 1; j++, i += 2) e += pf(j ? ", 0x%04x" : "0x%04x", g.u16(i));
        g.add(1, "EIds: [" + e + "]");
        if (j < n) g.err("missing EId, fig length too short !");
        g.add(1, "OE Announcement support:");
        g.announce(2, asu);
    }
}

void fig0_26(Fig& g) {   // 8.1.10.5.2
    const uint8_t* f = g.f;
    for (int i = 1; i < g.len - 6;) {
        const unsigned asw = g.u16(i + 1);
        const int nw = f[i + 3] >> 7, rg = (f[i + 3] >> 6) & 1;
        g.add(0, pf("Other ensemble announcement switching, cluster 0x%X", f[i]));
        g.add(1, pf("Cluster Id Current Ensemble=0x%X", f[i]));
        g.add(1, pf("Asw flags=0x%X", asw));
        g.add(1, pf("New flag=%d %s announcement", nw, nw ? "newly introduced" : "repeated"));
        g.add(1, pf("Region flag=%d last byte %s", rg, rg ? "present" : "absent. The announcement concerns the whole service area"));
        g.add(1, pf("Region Id Current Ensemble=0x%X", f[i + 3] & 0x3F));
        g.add(1, pf("EId Other Ensemble=0x%X", g.u16(i + 4)));
        g.add(1, pf("Cluster Id Other Ensemble=0x%X", f[i + 6]));
        i += 7;
        if (rg) {
            if (i < g.len) {
                if (f[i] >> 6) g.err(pf("Rfa=%d invalid value", f[i] >> 6));
                g.add(1, pf("Region Id Other Ensemble=0x%X", f[i] & 0x3F));
            } else g.err("missing Region Id Other Ensemble, fig length too short !");
            i++;
        }
        g.add(1, "Announcement switching:");
        g.announce(2, asw);
    }
}

void fig0_27(Fig& g) {   // 8.1.11.2.1
    const uint8_t* f = g.f;
    for (int i = 1; i < g.len - 2;) {
        const int n = f[i + 2] & 15;
        g.add(0, pf("FM announcement support of SId 0x%X", g.u16(i)));
        g.add(1, pf("SId=0x%X", g.u16(i)));
        if (f[i + 2] >> 4) g.err(pf("Rfu=%d invalid value", f[i + 2] >> 4));
        g.add(1, pf("Number of PI codes=%d", n));
        if (n > 12) g.err(pf("Number of PI codes=%d > 12 (maximum value)", n));
        g.add(1, pf("database key=0x%02X", (g.oe << 5) | (g.pd << 4) | n));
        if (!n) g.add(1, "CEI=true");
        i += 3;
        g.add(1, "PI Codes:");
        int j = 0;
        for (; j < n && i < g.len - 1; j++, i += 2) g.add(2, pf("- 0x%X", g.u16(i)));
        if (j != n) g.err("fig length too short !");
    }
}

void fig0_28(Fig& g) {   // 8.1.11.2.2
    const uint8_t* f = g.f;
    for (int i = 1; i < g.len - 3; i += 4) {
        const int nw = f[i + 1] >> 7;
        g.add(0, pf("FM announcement switching, cluster 0x%X", f[i]));
        g.add(1, pf("Cluster Id Current Ensemble=0x%X", f[i]));
        if (!f[i]) g.err("Cluster Id Current Ensemble invalid value 0");
        g.add(1, pf("New flag=%d %s announcement", nw, nw ? "newly introduced" : "repeated"));
        if ((f[i + 1] >> 6) & 1) g.err("Rfa=1 invalid value");
        g.add(1, pf("Region Id Current Ensemble=0x%X", f[i + 1] & 0x3F));
        g.add(1, pf("PI=0x%X", g.u16(i + 2)));
    }
}

void fig0_31(Fig& g) {   // 8.1.12
    const uint8_t* f = g.f;
    if (g.len != 7) g.err(pf("invalid length %d, expecting 7", g.len));
    const uint32_t t0 = g.u32(1);
    g.add(0, pf("FIG type 0 flag field=0x%X", t0));
    g.add(0, pf("FIG type 1 flag field=0x%X", f[5]));
    g.add(0, pf("FIG type 2 flag field=0x%X", f[6]));
    for (int j = 0; j < 32; j++) {
        if (!(t0 & (1u << j))) continue;
        if (j <= 5 || j == 8 || j == 10 || j == 13 || j == 14 || j == 19 || j == 26 || j == 28)
            g.err(pf("OE %d FIG 0/%d=carried in AIC, invalid configuration, shall always be carried entirely in the FIC", g.oe, j));
        else if (j == 21 || j == 24 || g.oe == 0) g.add(1, pf("OE %d FIG 0/%d=carried in AIC, same shall be carried in FIC", g.oe, j));
        else g.add(1, pf("OE %d FIG 0/%d=carried in AIC, may be carried entirely in AIC", g.oe, j));
    }
    for (int t = 1; t <= 2; t++)
        for (int j = 0; j < 8; j++)
            if (f[4 + t] & (1u << j))
                g.add(1, pf("OE %d FIG %d/%d=carried in AIC, %s", g.oe, t, j, g.oe == 0 ? "same shall be carried in FIC" : "may be carried entirely in AIC"));
}


} // namespace dab_fig_detail

} // namespace dect2
