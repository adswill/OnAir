#include "dect2/geo/wx_overlay.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846, kDeg = kPi / 180.0, kRe = 6371.0;

double wrapPi(double d) { while (d > 180) d -= 360; while (d < -180) d += 360; return d; }

// the point `km` from (lat,lon) along `bearing` (radians clockwise from north); km may be negative
void destination(double lat, double lon, double bearing, double km, double& la2, double& lo2) {
    const double d = km / kRe, p1 = lat * kDeg, l1 = lon * kDeg;
    const double s2 = std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(bearing);
    const double p2 = std::asin(std::max(-1.0, std::min(1.0, s2)));
    const double l2 = l1 + std::atan2(std::sin(bearing) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * s2);
    la2 = p2 / kDeg; lo2 = l2 / kDeg;
}

double bearingTo(double la1, double lo1, double la2, double lo2) {
    const double p1 = la1 * kDeg, p2 = la2 * kDeg, dl = (lo2 - lo1) * kDeg;
    return std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl));
}

struct Node { double x = 0, y = 0, u = 0, v = 0; };   // map coordinate (lon deg, projection y), source pixel

void fillTriangle(const Node& a, const Node& b, const Node& c, double x0, double y0, double sx, double sy, int W, int H,
                  const uint32_t* src, int srcW, int srcH, std::vector<uint32_t>& out) {
    // target pixel space: px = (lon - x0) / sx, py = (y0 - y) / sy
    const double ax = (a.x - x0) / sx, ay = (y0 - a.y) / sy, bx = (b.x - x0) / sx, by = (y0 - b.y) / sy, cx = (c.x - x0) / sx, cy = (y0 - c.y) / sy;
    const double den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (std::fabs(den) < 1e-12) return;
    const int xa = std::max(0, (int)std::floor(std::min({ax, bx, cx}) - 0.5)), xb = std::min(W - 1, (int)std::ceil(std::max({ax, bx, cx}) + 0.5));
    const int ya = std::max(0, (int)std::floor(std::min({ay, by, cy}) - 0.5)), yb = std::min(H - 1, (int)std::ceil(std::max({ay, by, cy}) + 0.5));
    const double eps = 0.02;   // a hair outside the edge, so neighbouring triangles leave no seam
    for (int py = ya; py <= yb; py++)
        for (int px = xa; px <= xb; px++) {
            const double X = px + 0.5, Y = py + 0.5;
            const double l1 = ((by - cy) * (X - cx) + (cx - bx) * (Y - cy)) / den;
            const double l2 = ((cy - ay) * (X - cx) + (ax - cx) * (Y - cy)) / den;
            const double l3 = 1 - l1 - l2;
            if (l1 < -eps || l2 < -eps || l3 < -eps) continue;
            const double u = l1 * a.u + l2 * b.u + l3 * c.u, v = l1 * a.v + l2 * b.v + l3 * c.v;
            const int iu = std::max(0, std::min(srcW - 1, (int)std::lround(u))), iv = std::max(0, std::min(srcH - 1, (int)std::lround(v)));
            out[(size_t)py * W + px] = src[(size_t)iv * srcW + iu];
        }
}
}

double projY(double latDeg, MapProj p) {
    if (p == MapProj::PlateCarree) return latDeg;
    const double l = std::max(-85.0511, std::min(85.0511, latDeg)) * kDeg;
    return std::log(std::tan(kPi / 4 + l / 2)) / kDeg;   // in "degrees" so a Mercator raster has the same units on both axes
}
double projLat(double y, MapProj p) {
    if (p == MapProj::PlateCarree) return y;
    return (2 * std::atan(std::exp(y * kDeg)) - kPi / 2) / kDeg;
}

bool swathLatLon(const Sgp4& sat, const SwathSpec& s, double line, double x, double& latDeg, double& lonDeg) {
    if (!sat.ok() || s.linesPerSec <= 0 || s.width < 2) return false;
    const double t = s.startUnix + s.timeOffsetSec + line / s.linesPerSec;
    double la, lo, h, la1, lo1, la2, lo2;
    if (!subPoint(sat, t, la, lo, h) || !subPoint(sat, t - 0.5, la1, lo1, h) || !subPoint(sat, t + 0.5, la2, lo2, h)) return false;
    const double hdg = bearingTo(la1, lo1, la2, lo2);
    const double f = 2.0 * x / (s.width - 1) - 1.0;   // -1 .. 1
    const double d = f * s.swathKm / 2;
    // positive f is the left of the track when xToLeft: bearing heading - 90 deg
    destination(la, lo, hdg + (s.xToLeft ? -kPi / 2 : kPi / 2), d, latDeg, lonDeg);
    lonDeg = wrapPi(lonDeg);
    return true;
}

bool projectSwath(const Sgp4& sat, const SwathSpec& s, const uint32_t* src, int srcH, int maxDim, MapProj proj, GeoRaster& out) {
    out = GeoRaster();
    if (!src || srcH < 2 || s.width < 2 || maxDim < 16 || !sat.ok()) return false;
    const int lineStep = std::max(1, std::min(8, srcH / 4));
    const int nCols = 33;
    std::vector<int> lines;
    for (int l = 0; l < srcH; l += lineStep) lines.push_back(l);
    if (lines.back() != srcH - 1) lines.push_back(srcH - 1);
    const int R = (int)lines.size();
    std::vector<Node> g((size_t)R * nCols);
    double prevLon = 0; bool havePrev = false;
    double xmin = 1e9, xmax = -1e9, ymin = 1e9, ymax = -1e9;
    for (int r = 0; r < R; r++) {
        for (int c = 0; c < nCols; c++) {
            const double px = (double)c / (nCols - 1) * (s.width - 1);
            double la, lo;
            if (!swathLatLon(sat, s, lines[r], px, la, lo)) return false;
            if (havePrev) lo = prevLon + wrapPi(lo - prevLon);   // unwrapped against the previous node, so the date line is no jump
            prevLon = lo; havePrev = true;
            Node& n = g[(size_t)r * nCols + c];
            n.x = lo; n.y = projY(la, proj); n.u = px; n.v = lines[r];
            xmin = std::min(xmin, n.x); xmax = std::max(xmax, n.x); ymin = std::min(ymin, n.y); ymax = std::max(ymax, n.y);
        }
        prevLon = g[(size_t)r * nCols].x;   // walk along the track from the first column of the next line
    }
    // the unwrap above follows node by node across a row, so the row is continuous; rows are chained through their first node
    if (xmax - xmin > 120 || xmax - xmin <= 0 || ymax - ymin <= 0) return false;   // a swath is never this wide: a wrap went wrong
    const double span = std::max(xmax - xmin, ymax - ymin);
    const double pxSize = span / maxDim;
    const int W = std::max(2, (int)std::ceil((xmax - xmin) / pxSize)), H = std::max(2, (int)std::ceil((ymax - ymin) / pxSize));
    out.proj = proj; out.w = W; out.h = H;
    out.px.assign((size_t)W * H, 0u);
    for (int r = 0; r + 1 < R; r++)
        for (int c = 0; c + 1 < nCols; c++) {
            const Node& a = g[(size_t)r * nCols + c]; const Node& b = g[(size_t)r * nCols + c + 1];
            const Node& d = g[(size_t)(r + 1) * nCols + c]; const Node& e = g[(size_t)(r + 1) * nCols + c + 1];
            fillTriangle(a, b, d, xmin, ymax, pxSize, pxSize, W, H, src, s.width, srcH, out.px);
            fillTriangle(b, e, d, xmin, ymax, pxSize, pxSize, W, H, src, s.width, srcH, out.px);
        }
    out.west = xmin; out.east = xmin + W * pxSize;
    out.north = projLat(ymax, proj); out.south = projLat(ymax - H * pxSize, proj);
    return true;
}

bool projectGeosDisk(const uint32_t* src, int srcW, int srcH, double satLon, double diskPx, int maxDim, MapProj proj, GeoRaster& out) {
    out = GeoRaster();
    if (!src || srcW < 8 || srcH < 8 || diskPx < 8 || maxDim < 16) return false;
    constexpr double H = 42164.16, a = 6378.137, b = 6356.7523, e2 = 1.0 - (b * b) / (a * a);
    const double alpha0 = std::asin(a / H);                  // the equatorial limb half angle
    const double scale = (diskPx / 2) / alpha0;              // pixels per radian
    const double cx = (srcW - 1) / 2.0, cy = (srcH - 1) / 2.0;
    const double lim = 81.0;
    const double y0 = projY(lim, proj), y1 = projY(-lim, proj);
    const double x0 = satLon - lim, x1 = satLon + lim;
    const double span = std::max(x1 - x0, y0 - y1), pxSize = span / maxDim;
    const int W = (int)std::ceil((x1 - x0) / pxSize), Hh = (int)std::ceil((y0 - y1) / pxSize);
    out.proj = proj; out.w = W; out.h = Hh; out.px.assign((size_t)W * Hh, 0u);
    for (int py = 0; py < Hh; py++) {
        const double lat = projLat(y0 - (py + 0.5) * pxSize, proj) * kDeg;
        const double lc = std::atan((b * b) / (a * a) * std::tan(lat));
        const double rl = b / std::sqrt(1.0 - e2 * std::cos(lc) * std::cos(lc));
        for (int px = 0; px < W; px++) {
            const double dl = (x0 + (px + 0.5) * pxSize - satLon) * kDeg;
            const double X = rl * std::cos(lc) * std::cos(dl), Y = rl * std::cos(lc) * std::sin(dl), Z = rl * std::sin(lc);
            if (std::cos(lat) * std::cos(dl) * (H - X) - std::cos(lat) * std::sin(dl) * Y - std::sin(lat) * Z <= 0) continue;   // beyond the limb
            const double xa = std::atan2(Y, H - X), ya = std::atan2(Z, std::sqrt((H - X) * (H - X) + Y * Y));
            const int u = (int)std::lround(cx + xa * scale), v = (int)std::lround(cy - ya * scale);
            if (u < 0 || v < 0 || u >= srcW || v >= srcH) continue;
            out.px[(size_t)py * W + px] = src[(size_t)v * srcW + u];
        }
    }
    out.west = x0; out.east = x0 + W * pxSize; out.north = projLat(y0, proj); out.south = projLat(y0 - Hh * pxSize, proj);
    return true;
}

std::vector<uint8_t> rasterBytes(const GeoRaster& r) {
    std::vector<uint8_t> o((size_t)r.w * r.h * 4);
    for (size_t i = 0; i < r.px.size() && i * 4 + 3 < o.size(); i++) {
        const uint32_t p = r.px[i];
        o[i * 4] = p & 255; o[i * 4 + 1] = (p >> 8) & 255; o[i * 4 + 2] = (p >> 16) & 255; o[i * 4 + 3] = p >> 24;
    }
    return o;
}

namespace {
void le16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 255); v.push_back((x >> 8) & 255); }
void le32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 255); }
void f64(std::vector<uint8_t>& v, double d) { uint8_t b[8]; std::memcpy(b, &d, 8); v.insert(v.end(), b, b + 8); }   // little-endian hosts
void entry(std::vector<uint8_t>& v, uint16_t tag, uint16_t type, uint32_t count, uint32_t val) { le16(v, tag); le16(v, type); le32(v, count); le32(v, val); }
}

bool geoTiffRgba(const GeoRaster& r, std::vector<uint8_t>& out) {
    out.clear();
    if (r.empty() || r.proj != MapProj::PlateCarree || r.east <= r.west || r.north <= r.south) return false;
    const std::vector<uint8_t> data = rasterBytes(r);
    // layout: header (8) | pixel data | bps (4 x u16) | extra samples u16 | pixel scale 3 doubles | tiepoint 6 doubles | geokeys 4 x 4 u16 | IFD
    std::vector<uint8_t> f = {'I', 'I', 42, 0, 0, 0, 0, 0};
    f.insert(f.end(), data.begin(), data.end());
    const uint32_t offBps = (uint32_t)f.size(); for (int i = 0; i < 4; i++) le16(f, 8);
    const uint32_t offScale = (uint32_t)f.size(); f64(f, (r.east - r.west) / r.w); f64(f, (r.north - r.south) / r.h); f64(f, 0);
    const uint32_t offTie = (uint32_t)f.size(); f64(f, 0); f64(f, 0); f64(f, 0); f64(f, r.west); f64(f, r.north); f64(f, 0);
    const uint32_t offKeys = (uint32_t)f.size();
    const uint16_t keys[16] = {1, 1, 0, 3,  1024, 0, 1, 2,  1025, 0, 1, 1,  2048, 0, 1, 4326};   // GTModelType geographic, RasterPixelIsArea, EPSG 4326
    for (uint16_t k : keys) le16(f, k);
    if (f.size() & 1) f.push_back(0);
    const uint32_t ifd = (uint32_t)f.size();
    le16(f, 13);
    entry(f, 256, 4, 1, (uint32_t)r.w); entry(f, 257, 4, 1, (uint32_t)r.h);
    entry(f, 258, 3, 4, offBps); entry(f, 259, 3, 1, 1); entry(f, 262, 3, 1, 2);
    entry(f, 273, 4, 1, 8); entry(f, 277, 3, 1, 4); entry(f, 278, 4, 1, (uint32_t)r.h);
    entry(f, 279, 4, 1, (uint32_t)data.size());
    entry(f, 338, 3, 1, 2);   // ExtraSamples: unassociated alpha
    entry(f, 33550, 12, 3, offScale); entry(f, 33922, 12, 6, offTie); entry(f, 34735, 3, 16, offKeys);
    le32(f, 0);
    f[4] = ifd & 255; f[5] = (ifd >> 8) & 255; f[6] = (ifd >> 16) & 255; f[7] = (ifd >> 24) & 255;
    out = std::move(f);
    return true;
}

std::string kmlGroundOverlay(const std::string& name, const std::string& href, const GeoRaster& r, double opacity) {
    auto esc = [](const std::string& s) { std::string o; for (char c : s) { if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else o += c; } return o; };
    const int a = (int)std::lround(std::max(0.0, std::min(1.0, opacity)) * 255);
    char b[1024];
    snprintf(b, sizeof b,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<GroundOverlay>\n  <name>%s</name>\n  <color>%02xffffff</color>\n"
             "  <Icon><href>%s</href></Icon>\n  <LatLonBox><north>%.6f</north><south>%.6f</south><east>%.6f</east><west>%.6f</west><rotation>0</rotation></LatLonBox>\n</GroundOverlay>\n</kml>\n",
             esc(name).c_str(), a, esc(href).c_str(), r.north, r.south, r.east, r.west);
    return b;
}

}
