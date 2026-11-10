// FIS-B TWGO products (uat_twgo.h).
#include "dect2/uat/uat_twgo.h"
#include <cstdio>

namespace dect2 {

std::string uatDlacToText(const uint8_t* d, size_t n) {
    static const char* const kAlpha = "\x01" "ABCDEFGHIJKLMNOPQRSTUVWXYZ\x01\t\x01\n| !\"#$%&'()*+,-./0123456789:;<=>?";
    std::string out;
    int have = 0, acc = 0;
    bool tab = false;
    auto sym = [&](int c) {
        if (tab) { tab = false; out.append((size_t)(c & 63), ' '); return; }
        const char ch = kAlpha[c & 63];
        if (ch == '\t') tab = true;
        else if (ch != '\x01') out.push_back(ch);
    };
    for (size_t i = 0; i < n; i++) {
        acc = (acc << 8) | d[i]; have += 8;
        while (have >= 6) { sym((acc >> (have - 6)) & 63); have -= 6; }
        acc &= (1 << have) - 1;
    }
    return out;
}

static double wrap(double v, double half) { return v > half ? v - 2 * half : v; }

bool uatTwgoDecode(uint16_t product, const uint8_t* d, size_t n, std::string& text, std::vector<UatOverlay>& ov) {
    if (n < 6) return false;
    const int fmt = d[0] >> 4, count = d[1] >> 4;
    if (fmt != 2 && fmt != 8) return false;
    const std::string loc = uatDlacToText(d + 2, 3);
    size_t p = 6;
    std::string t;
    char b[160];
    for (int r = 0; r < count && p + 5 <= n; r++) {
        if (fmt == 2) {
            const size_t len = (size_t)d[p] << 8 | d[p + 1];
            if (len < 5 || p + len > n) break;
            const int number = (d[p + 2] << 6) | (d[p + 3] >> 2), status = (d[p + 4] >> 2) & 1;
            std::string body = uatDlacToText(d + p + 5, len - 5);
            snprintf(b, sizeof b, "[%s #%d%s] ", loc.c_str(), number, status ? "" : " cancelled");
            t += b + body + "\n";
            p += len;
            continue;
        }
        const size_t len = ((size_t)d[p] << 2) | (d[p + 1] >> 6);
        if (len < 7 || p + len > n) break;
        UatOverlay o;
        o.reportNumber = ((d[p + 1] & 0x3F) << 8) | d[p + 2];
        o.reportYear = d[p + 3] >> 1;
        o.recordId = ((d[p + 4] >> 1) & 15) + 1;
        const int labelFlag = d[p + 4] & 1;
        size_t q = p + 5;
        if (labelFlag) { if (q + 9 > p + len) break; o.label = uatDlacToText(d + q, 9); q += 9; } else q += 2;
        if (q + 2 > p + len) break;
        const int a = d[q], bb = d[q + 1];
        q += 2;
        const bool qual = (a >> 6) & 1, param = (a >> 5) & 1;
        o.objectType = bb >> 4; o.objectStatus = bb & 15;
        if (product == 14 && qual) q += 3;
        if (param) q += 2;
        if (q + 2 > p + len) break;
        const int appl = d[q] >> 6, dtf = (d[q] >> 4) & 3;
        o.geometry = d[q] & 15;
        q++;
        const int nVert = (d[q] & 0x3F) + 1;
        q++;
        const size_t dtSize = dtf == 1 ? 4 : dtf == 2 ? 3 : dtf == 3 ? 2 : 0;
        if (appl == 1 || appl == 2) q += dtSize; else if (appl == 3) q += 2 * dtSize;
        if (o.geometry == 3 || o.geometry == 4 || o.geometry == 9 || o.geometry == 10 || o.geometry == 11 || o.geometry == 12) {
            if (q + 6 * (size_t)nVert > p + len) break;
            for (int v = 0; v < nVert; v++) {
                const uint8_t* g = d + q + 6 * (size_t)v;
                const uint32_t lonR = (uint32_t)g[0] << 11 | (uint32_t)g[1] << 3 | g[2] >> 5;
                const uint32_t latR = (uint32_t)(g[2] & 0x1F) << 14 | (uint32_t)g[3] << 6 | g[4] >> 2;
                const int alt = ((g[4] & 3) << 8) | g[5];
                UatOverlayPoint pt;
                pt.lon = wrap(lonR * 360.0 / 524288.0, 180.0);
                pt.lat = wrap(latR * 360.0 / 524288.0, 90.0);
                pt.altFt = alt * 100;
                o.pts.push_back(pt);
            }
        } else if (o.geometry == 7 || o.geometry == 8) {
            if (q + 14 * (size_t)nVert > p + len) break;
            for (int v = 0; v < nVert; v++) {
                const uint8_t* g = d + q + 14 * (size_t)v;
                const uint32_t lonB = (uint32_t)g[0] << 10 | (uint32_t)g[1] << 2 | g[2] >> 6;
                const uint32_t latB = (uint32_t)(g[2] & 0x3F) << 12 | (uint32_t)g[3] << 4 | g[4] >> 4;
                const int zb = g[9] >> 1;
                UatOverlayPoint pt;
                pt.lon = wrap(lonB * 360.0 / 262144.0, 180.0);
                pt.lat = wrap(latB * 360.0 / 262144.0, 90.0);
                pt.altFt = zb * 500;
                o.pts.push_back(pt);
                o.radiusNm = (((g[10] & 3) << 7) | (g[11] >> 1)) * 0.2;
            }
        } else if (o.geometry != 0) { p += len; continue; }
        snprintf(b, sizeof b, "[%s #%d] overlay %d, %zu points", loc.c_str(), o.reportNumber, o.recordId, o.pts.size());
        t += b;
        if (!o.pts.empty()) { snprintf(b, sizeof b, " from %.4f %.4f", o.pts[0].lat, o.pts[0].lon); t += b; if (o.radiusNm > 0) { snprintf(b, sizeof b, ", radius %.1f NM", o.radiusNm); t += b; } }
        t += "\n";
        ov.push_back(o);
        p += len;
    }
    if (t.empty()) return false;
    while (!t.empty() && t.back() == '\n') t.pop_back();
    text = t;
    return true;
}

} // namespace dect2
