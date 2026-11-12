// Elevation files and terrain diffraction for the propagation tool, see core/include/dect2/prop/prop_terrain.h and docs/propagation.md.
#include "dect2/prop/prop_terrain.h"
#include "dect2/prop/prop.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "dect2/common/u8file.h"
#include "dect2/common/inflate.h"


namespace dect2::prop {

namespace {
constexpr double kAe = 8495.0;   // km, 4/3 earth
double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string baseName(const std::string& p) {
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}
}

bool ElevTile::covers(double lat, double lon) const {
    return w >= 2 && h >= 2 && lat <= latTop + 1e-9 && lat >= latBottom() - 1e-9 && lon >= lonLeft - 1e-9 && lon <= lonRight() + 1e-9;
}

bool ElevTile::heightAt(double lat, double lon, double& m) const {
    if (!covers(lat, lon)) return false;
    const double fy = clampd((latTop - lat) / dLat, 0, h - 1.000001), fx = clampd((lon - lonLeft) / dLon, 0, w - 1.000001);
    const int y = (int)fy, x = (int)fx;
    const double ty = fy - y, tx = fx - x;
    const int16_t a = z[(size_t)y * w + x], b = z[(size_t)y * w + x + 1], c = z[(size_t)(y + 1) * w + x], d = z[(size_t)(y + 1) * w + x + 1];
    double sum = 0, wsum = 0;   // bilinear over the corners that have data
    const double wa = (1 - tx) * (1 - ty), wb = tx * (1 - ty), wc = (1 - tx) * ty, wd = tx * ty;
    if (a != kVoid) { sum += a * wa; wsum += wa; }
    if (b != kVoid) { sum += b * wb; wsum += wb; }
    if (c != kVoid) { sum += c * wc; wsum += wc; }
    if (d != kVoid) { sum += d * wd; wsum += wd; }
    if (wsum < 1e-6) {   // on a void sample itself: the mean of the corners that have data
        int n = 0;
        sum = 0;
        for (int16_t v : {a, b, c, d}) if (v != kVoid) { sum += v; n++; }
        if (n == 0) return false;
        m = sum / n;
        return true;
    }
    m = sum / wsum;
    return true;
}

bool Terrain::heightAt(double lat, double lon, double& m) const {
    for (const ElevTile& t : tiles)
        if (t.heightAt(lat, lon, m)) return true;
    return false;
}

std::string Terrain::describe() const {
    if (tiles.empty()) return "no elevation data";
    double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180;
    for (const ElevTile& t : tiles) {
        la0 = std::min(la0, t.latBottom()); la1 = std::max(la1, t.latTop);
        lo0 = std::min(lo0, t.lonLeft); lo1 = std::max(lo1, t.lonRight());
    }
    char b[160];
    snprintf(b, sizeof b, "%d file%s, %.2f..%.2f lat, %.2f..%.2f lon", (int)tiles.size(), tiles.size() == 1 ? "" : "s", la0, la1, lo0, lo1);
    return b;
}

bool parseHgt(const std::string& fileName, const std::vector<uint8_t>& bytes, ElevTile& t, std::string& err) {
    const std::string n = lower(baseName(fileName));
    int la = 0, lo = 0;
    char ns = 0, ew = 0;
    if (n.size() < 7 || std::sscanf(n.c_str(), "%c%2d%c%3d", &ns, &la, &ew, &lo) != 4 || (ns != 'n' && ns != 's') || (ew != 'e' && ew != 'w')) {
        err = "an .hgt file must keep its SRTM name, like N54E025.hgt (the name says where it is)";
        return false;
    }
    if (ns == 's') la = -la;
    if (ew == 'w') lo = -lo;
    int side = 0;
    if (bytes.size() == 1201u * 1201u * 2u) side = 1201;
    else if (bytes.size() == 3601u * 3601u * 2u) side = 3601;
    else { err = "not an SRTM .hgt size (1201 x 1201 or 3601 x 3601 samples)"; return false; }
    t = ElevTile();
    t.name = baseName(fileName);
    t.w = t.h = side;
    t.latTop = la + 1.0; t.lonLeft = lo;
    t.dLat = t.dLon = 1.0 / (side - 1);
    t.z.resize((size_t)side * side);
    for (size_t i = 0; i < t.z.size(); i++) t.z[i] = (int16_t)(uint16_t)((bytes[2 * i] << 8) | bytes[2 * i + 1]);
    return true;
}

namespace {
struct Tiff {
    const std::vector<uint8_t>& b;
    bool le = true;
    explicit Tiff(const std::vector<uint8_t>& v) : b(v) {}
    bool ok(size_t off, size_t n) const { return off + n <= b.size() && off + n >= off; }
    uint32_t u16(size_t o) const { return le ? (uint32_t)(b[o] | b[o + 1] << 8) : (uint32_t)(b[o] << 8 | b[o + 1]); }
    uint32_t u32(size_t o) const {
        return le ? (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24
                  : (uint32_t)b[o] << 24 | (uint32_t)b[o + 1] << 16 | (uint32_t)b[o + 2] << 8 | (uint32_t)b[o + 3];
    }
    double f64(size_t o) const {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= (uint64_t)b[o + i] << (le ? 8 * i : 8 * (7 - i));
        double d;
        std::memcpy(&d, &v, 8);
        return d;
    }
    float f32(size_t o) const { const uint32_t v = u32(o); float f; std::memcpy(&f, &v, 4); return f; }
};
struct Entry { uint32_t tag = 0, type = 0, count = 0; size_t valOff = 0; };
size_t typeSize(uint32_t t) { switch (t) { case 1: case 2: case 6: case 7: return 1; case 3: case 8: return 2; case 4: case 9: case 11: return 4; case 12: return 8; default: return 0; } }
}

bool parseGeoTiff(const std::vector<uint8_t>& bytes, ElevTile& t, std::string& err) {
    Tiff T(bytes);
    if (bytes.size() < 8) { err = "too short for a TIFF"; return false; }
    if (bytes[0] == 'I' && bytes[1] == 'I') T.le = true;
    else if (bytes[0] == 'M' && bytes[1] == 'M') T.le = false;
    else { err = "not a TIFF file"; return false; }
    if (T.u16(2) == 43) { err = "BigTIFF is not supported: save as a plain GeoTIFF"; return false; }
    if (T.u16(2) != 42) { err = "not a TIFF file"; return false; }
    const size_t ifd = T.u32(4);
    if (!T.ok(ifd, 2)) { err = "broken TIFF directory"; return false; }
    const uint32_t n = T.u16(ifd);
    if (!T.ok(ifd + 2, (size_t)n * 12)) { err = "broken TIFF directory"; return false; }
    std::vector<Entry> es;
    for (uint32_t i = 0; i < n; i++) {
        const size_t e = ifd + 2 + (size_t)i * 12;
        Entry x; x.tag = T.u16(e); x.type = T.u16(e + 2); x.count = T.u32(e + 4);
        const size_t sz = typeSize(x.type) * x.count;
        x.valOff = sz <= 4 ? e + 8 : T.u32(e + 8);
        if (sz == 0 || !T.ok(x.valOff, sz)) continue;
        es.push_back(x);
    }
    auto find = [&](uint32_t tag) -> const Entry* { for (const Entry& e : es) if (e.tag == tag) return &e; return nullptr; };
    auto num = [&](const Entry* e, uint32_t i) -> double {
        if (!e || i >= e->count) return 0;
        const size_t o = e->valOff + typeSize(e->type) * i;
        switch (e->type) { case 3: return T.u16(o); case 4: return T.u32(o); case 8: return (int16_t)T.u16(o); case 9: return (int32_t)T.u32(o);
                           case 11: return T.f32(o); case 12: return T.f64(o); default: return bytes[o]; }
    };
    const Entry *eW = find(256), *eH = find(257), *eBits = find(258), *eComp = find(259), *eSpp = find(277), *eFmt = find(339);
    const Entry *eScale = find(33550), *eTie = find(33922), *eKeys = find(34735), *eNoData = find(42113);
    if (!eW || !eH) { err = "no image size in the TIFF"; return false; }
    const int w = (int)num(eW, 0), h = (int)num(eH, 0);
    if (w < 2 || h < 2 || (double)w * h > 64e6) { err = "image size out of range (2 to 64 million samples)"; return false; }
    if (eComp && num(eComp, 0) != 1) { err = "compressed GeoTIFF: save it uncompressed (e.g. gdal_translate -co COMPRESS=NONE)"; return false; }
    if (eSpp && num(eSpp, 0) != 1) { err = "more than one band: keep only the elevation band"; return false; }
    const int bits = eBits ? (int)num(eBits, 0) : 16;
    const int fmt = eFmt ? (int)num(eFmt, 0) : 1;   // 1 unsigned, 2 signed, 3 float
    if (!((bits == 16 && (fmt == 1 || fmt == 2)) || (bits == 32 && (fmt == 1 || fmt == 2 || fmt == 3)))) {
        err = "sample type not supported (16-bit integer, 32-bit integer or 32-bit float)";
        return false;
    }
    if (!eScale || eScale->count < 2 || !eTie || eTie->count < 6) { err = "no georeference (pixel scale and tie point) in the TIFF"; return false; }
    bool pixelIsPoint = false;
    if (eKeys && eKeys->count >= 4) {
        const uint32_t nk = (uint32_t)num(eKeys, 3);
        for (uint32_t k = 0; k < nk && 4 + 4 * k + 3 < eKeys->count; k++) {
            const uint32_t id = (uint32_t)num(eKeys, 4 + 4 * k), loc = (uint32_t)num(eKeys, 5 + 4 * k), val = (uint32_t)num(eKeys, 7 + 4 * k);
            if (loc != 0) continue;
            if (id == 1024 && val == 1) { err = "projected coordinates (UTM ...): reproject to WGS 84 latitude / longitude (EPSG:4326)"; return false; }
            if (id == 3072) { err = "projected coordinates (UTM ...): reproject to WGS 84 latitude / longitude (EPSG:4326)"; return false; }
            if (id == 1025 && val == 2) pixelIsPoint = true;
        }
    }
    double noData = -1e30;
    bool haveNoData = false;
    if (eNoData && eNoData->type == 2) {
        std::string s((const char*)&bytes[eNoData->valOff], eNoData->count);
        noData = std::atof(s.c_str());
        haveNoData = true;
    }
    const double sx = num(eScale, 0), sy = num(eScale, 1);
    const double ti = num(eTie, 0), tj = num(eTie, 1), tx = num(eTie, 3), ty = num(eTie, 4);
    if (!(sx > 0) || !(sy > 0) || sx > 1 || sy > 1 || std::fabs(ty) > 90 || std::fabs(tx) > 360) {
        err = "the georeference does not look like latitude / longitude degrees";
        return false;
    }
    t = ElevTile();
    t.w = w; t.h = h;
    t.dLon = sx; t.dLat = sy;
    const double half = pixelIsPoint ? 0.0 : 0.5;
    t.lonLeft = tx + (0 + half - ti) * sx;
    t.latTop = ty - (0 + half - tj) * sy;
    t.z.assign((size_t)w * h, ElevTile::kVoid);
    const size_t bps = (size_t)bits / 8;
    auto sample = [&](size_t off) -> double {
        if (bits == 16) return fmt == 2 ? (double)(int16_t)T.u16(off) : (double)T.u16(off);
        if (fmt == 3) return T.f32(off);
        return fmt == 2 ? (double)(int32_t)T.u32(off) : (double)T.u32(off);
    };
    auto put = [&](int x, int y, size_t off) {
        if (x >= w || y >= h || !T.ok(off, bps)) return;
        const double v = sample(off);
        if (!std::isfinite(v) || (haveNoData && std::fabs(v - noData) < 1e-3) || v < -1000 || v > 9000) return;
        t.z[(size_t)y * w + x] = (int16_t)std::lround(v);
    };
    const Entry *eTw = find(322), *eTl = find(323), *eTo = find(324);
    if (eTw && eTl && eTo) {
        const int tw = (int)num(eTw, 0), tl = (int)num(eTl, 0);
        if (tw <= 0 || tl <= 0) { err = "broken tile size"; return false; }
        const int across = (w + tw - 1) / tw, down = (h + tl - 1) / tl;
        if ((int)eTo->count < across * down) { err = "missing tiles"; return false; }
        for (int ty2 = 0; ty2 < down; ty2++)
            for (int tx2 = 0; tx2 < across; tx2++) {
                const size_t base = (size_t)num(eTo, (uint32_t)(ty2 * across + tx2));
                for (int r = 0; r < tl; r++)
                    for (int c = 0; c < tw; c++) put(tx2 * tw + c, ty2 * tl + r, base + ((size_t)r * tw + c) * bps);
            }
    } else {
        const Entry *eSo = find(273), *eRps = find(278);
        if (!eSo) { err = "no image data offsets in the TIFF"; return false; }
        const int rps = eRps ? std::max(1, (int)std::min<double>(num(eRps, 0), h)) : h;
        for (uint32_t s = 0; s < eSo->count; s++) {
            const size_t base = (size_t)num(eSo, s);
            for (int r = 0; r < rps; r++) {
                const int y = (int)s * rps + r;
                if (y >= h) break;
                for (int x = 0; x < w; x++) put(x, y, base + ((size_t)r * w + x) * bps);
            }
        }
    }
    return true;
}

bool loadElevationFile(const std::string& path, Terrain& terr, std::string& err) {
    FILE* f = u8fopen(path, "rb");   // the file dialogs give UTF-8 paths
    if (!f) { err = "cannot open the file"; return false; }
    std::vector<uint8_t> b;
    uint8_t buf[1 << 16];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) {
        b.insert(b.end(), buf, buf + got);
        if (b.size() > (size_t)300 * 1024 * 1024) { std::fclose(f); err = "file larger than 300 MB"; return false; }
    }
    std::fclose(f);
    if (b.empty()) { err = "the file is empty"; return false; }
    ElevTile t;
    const std::string n = lower(baseName(path));
    bool ok;
    if (n.size() > 7 && n.substr(n.size() - 7) == ".hgt.gz") {
        // 4.0: a downloaded tile (demTileUrl): gzip, 1 arc-second; kept at 3 arc-seconds (90 m) to save memory (P.1812 needs no finer)
        std::vector<uint8_t> raw;
        if (!gunzip(b.data(), b.size(), raw, (size_t)64 << 20)) { err = "not a valid .gz file (an interrupted download? delete it and try again)"; return false; }
        ok = parseHgt(n.substr(0, n.size() - 3), raw, t, err);
        if (ok && t.w == 3601 && t.h == 3601) {
            ElevTile s;
            s.latTop = t.latTop; s.lonLeft = t.lonLeft; s.dLat = t.dLat * 3; s.dLon = t.dLon * 3; s.w = s.h = 1201;
            s.z.resize((size_t)1201 * 1201);
            for (int r = 0; r < 1201; r++)
                for (int c = 0; c < 1201; c++) s.z[(size_t)r * 1201 + c] = t.z[(size_t)(3 * r) * 3601 + 3 * c];
            t = std::move(s);
        }
    } else if (n.size() > 4 && n.substr(n.size() - 4) == ".hgt") ok = parseHgt(n, b, t, err);
    else if (b.size() >= 4 && ((b[0] == 'I' && b[1] == 'I') || (b[0] == 'M' && b[1] == 'M'))) ok = parseGeoTiff(b, t, err);
    else { err = "not an .hgt or GeoTIFF (.tif) file"; return false; }
    if (!ok) return false;
    t.name = baseName(path);
    for (ElevTile& e : terr.tiles)
        if (e.name == t.name) { e = std::move(t); return true; }   // the same file again: replace it
    terr.tiles.push_back(std::move(t));
    return true;
}

std::string demTileName(int la, int lo) {
    char b[16];
    std::snprintf(b, sizeof b, "%c%02d%c%03d", la >= 0 ? 'N' : 'S', std::abs(la), lo >= 0 ? 'E' : 'W', std::abs(lo));
    return b;
}

std::string demTileUrl(int la, int lo) {
    const std::string n = demTileName(la, lo);
    return "https://s3.amazonaws.com/elevation-tiles-prod/skadi/" + n.substr(0, 3) + "/" + n + ".hgt.gz";
}

std::vector<std::pair<int, int>> demTilesFor(double lat, double lon, double radiusKm, double lat2, double lon2, int maxTiles) {
    std::vector<std::pair<int, int>> v;
    auto add = [&](double la, double lo) {
        if (!std::isfinite(la) || !std::isfinite(lo) || std::fabs(la) >= 60) return;   // SRTM covers 60 S .. 60 N
        lo = std::fmod(lo + 540.0, 360.0) - 180.0;
        const std::pair<int, int> k{(int)std::floor(la), (int)std::floor(lo)};
        if ((int)v.size() < maxTiles && std::find(v.begin(), v.end(), k) == v.end()) v.push_back(k);
    };
    add(lat, lon);
    const double dla = radiusKm / 111.0, dlo = radiusKm / (111.0 * std::max(std::cos(lat * 3.14159265358979 / 180), 0.05));
    for (int i = -1; i <= 1; i++)
        for (int j = -1; j <= 1; j++) add(lat + i * dla, lon + j * dlo);
    if (std::isfinite(lat2) && std::isfinite(lon2)) {
        const double D = greatCircleKm(lat, lon, lat2, lon2), b = bearingDeg(lat, lon, lat2, lon2);
        for (double x = 0; x <= D; x += 20) { double la, lo; destination(lat, lon, b, x, la, lo); add(la, lo); }
        add(lat2, lon2);
    }
    return v;
}

Profile pathProfile(const Terrain& t, double lat1, double lon1, double lat2, double lon2, int samples) {
    Profile p;
    samples = std::max(samples, 3);
    const double d = greatCircleKm(lat1, lon1, lat2, lon2), brg = bearingDeg(lat1, lon1, lat2, lon2);
    p.dKm.resize(samples);
    p.hM.resize(samples);
    double last = 0;
    bool have = false, first = false, end = false;
    for (int i = 0; i < samples; i++) {
        const double di = d * i / (samples - 1);
        double la = lat1, lo = lon1, m = 0;
        if (i > 0) destination(lat1, lon1, brg, di, la, lo);
        const bool ok = t.heightAt(la, lo, m);
        if (ok) { last = m; have = true; }
        if (i == 0) first = ok;
        if (i == samples - 1) end = ok;
        p.dKm[i] = di;
        p.hM[i] = ok ? m : (have ? last : 0.0);   // a gap: hold the last height
    }
    p.complete = first && end;
    return p;
}

double bullingtonDb(const std::vector<double>& dKm, const std::vector<double>& hM, double hts, double hrs, double fMHz) {
    const size_t n = dKm.size();
    if (n < 3) return 0;
    const double d = dKm.back();
    if (d <= 0) return 0;
    const double lambda = 299.792458 / fMHz, Ce = 1.0 / kAe;
    double Stim = -1e30;
    for (size_t i = 1; i + 1 < n; i++) Stim = std::max(Stim, (hM[i] + 500 * Ce * dKm[i] * (d - dKm[i]) - hts) / dKm[i]);
    const double Str = (hrs - hts) / d;
    double Luc;
    if (Stim < Str) {   // line of sight: the most intruding point
        double vmax = -1e30;
        for (size_t i = 1; i + 1 < n; i++) {
            const double di = dKm[i];
            const double v = (hM[i] + 500 * Ce * di * (d - di) - (hts * (d - di) + hrs * di) / d) * std::sqrt(0.002 * d / (lambda * di * (d - di)));
            vmax = std::max(vmax, v);
        }
        Luc = knifeEdgeJ(vmax);
    } else {            // the equivalent knife edge where the steepest lines from both ends meet
        double Srim = -1e30;
        for (size_t i = 1; i + 1 < n; i++) Srim = std::max(Srim, (hM[i] + 500 * Ce * dKm[i] * (d - dKm[i]) - hrs) / (d - dKm[i]));
        const double dbp = clampd((hrs - hts + Srim * d) / (Stim + Srim), 1e-3, d - 1e-3);
        const double vb = (hts + Stim * dbp - (hts * (d - dbp) + hrs * dbp) / d) * std::sqrt(0.002 * d / (lambda * dbp * (d - dbp)));
        Luc = knifeEdgeJ(vb);
    }
    return Luc + (1 - std::exp(-Luc / 6.0)) * (10 + 0.02 * d);
}

TerrainLoss terrainLoss(const Profile& pr, double txHeightM, double rxHeightM, double fMHz) {
    TerrainLoss o;
    const size_t n = pr.dKm.size();
    if (n < 3) return o;
    const double hts = pr.hM.front() + txHeightM, hrs = pr.hM.back() + rxHeightM, d = pr.dKm.back();
    double mean = 0;
    for (double h : pr.hM) mean += h;
    mean /= (double)n;
    o.effTxM = std::max(hts - mean, 1.0);
    o.effRxM = std::max(hrs - mean, 1.0);
    const double La = bullingtonDb(pr.dKm, pr.hM, hts, hrs, fMHz);
    const std::vector<double> flat(n, 0.0);
    const double Ls = bullingtonDb(pr.dKm, flat, o.effTxM, o.effRxM, fMHz);
    const double Lsph = diffractionLossDb(fMHz, d, o.effTxM, o.effRxM);
    o.diffractionDb = La + std::max(Lsph - Ls, 0.0);
    double Stim = -1e30;
    for (size_t i = 1; i + 1 < n; i++) Stim = std::max(Stim, (pr.hM[i] + 500.0 / kAe * pr.dKm[i] * (d - pr.dKm[i]) - hts) / pr.dKm[i]);
    o.los = Stim < (hrs - hts) / d;
    // horizon elevation angles (mrad) at both ends, the other end when nothing is higher (P.452)
    double tt = (hrs - hts) / d - 1000.0 * d / (2 * kAe), tr = (hts - hrs) / d - 1000.0 * d / (2 * kAe);
    for (size_t i = 1; i + 1 < n; i++) {
        const double di = pr.dKm[i], dr = d - di;
        tt = std::max(tt, (pr.hM[i] - hts) / di - 1000.0 * di / (2 * kAe));
        tr = std::max(tr, (pr.hM[i] - hrs) / dr - 1000.0 * dr / (2 * kAe));
    }
    o.thetaMrad = std::max(0.0, 1000.0 * d / kAe + tt + tr);
    return o;
}

} // namespace dect2::prop
