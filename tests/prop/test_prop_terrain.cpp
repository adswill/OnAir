// Propagation terrain: SRTM .hgt and GeoTIFF parsing (built in memory), height lookups, path profiles, Bullington / delta-Bullington
// diffraction, and the model using a profile when one is loaded.
#include "dect2/prop/prop.h"
#include "dect2/prop/prop_terrain.h"
#include "dect2/winlink/wl_gzip.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
using namespace dect2::prop;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { printf("FAIL line %d: %s = %.4f, want %.4f\n", __LINE__, #a, _a, _b); fails++; } } while (0)

// a 1201 x 1201 SRTM3 tile: 100 m everywhere, optionally a north-south ridge of `ridge` m at longitude lonRidge
static std::vector<uint8_t> hgt(double ridge, double lonRidge, int lonDeg) {
    std::vector<uint8_t> b(1201u * 1201u * 2u);
    for (int r = 0; r < 1201; r++)
        for (int c = 0; c < 1201; c++) {
            const double lon = lonDeg + c / 1200.0;
            int16_t h = 100;
            if (ridge > 0 && std::fabs(lon - lonRidge) < 0.004) h = (int16_t)ridge;
            if (r == 5 && c == 5) h = -32768;   // one void
            b[2 * ((size_t)r * 1201 + c)] = (uint8_t)((uint16_t)h >> 8);
            b[2 * ((size_t)r * 1201 + c) + 1] = (uint8_t)((uint16_t)h & 0xff);
        }
    return b;
}

// a minimal one-strip GeoTIFF, 4 x 3 samples, value 10 r + c, 0.01 degree pixels with the top-left corner at 55 N 25 E
struct TiffW {
    bool be = false;
    std::vector<uint8_t> b;
    void u16(uint32_t v) { if (be) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); } else { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); } }
    void u32(uint32_t v) { if (be) for (int i = 3; i >= 0; i--) b.push_back((uint8_t)(v >> (8 * i))); else for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i))); }
    void f64(double d) { uint64_t v; std::memcpy(&v, &d, 8); if (be) for (int i = 7; i >= 0; i--) b.push_back((uint8_t)(v >> (8 * i))); else for (int i = 0; i < 8; i++) b.push_back((uint8_t)(v >> (8 * i))); }
};
static std::vector<uint8_t> geotiff(bool be, bool asFloat, int compression, int modelType) {
    TiffW t; t.be = be;
    const int w = 4, h = 3, bps = asFloat ? 4 : 2;
    // layout: header 8, data at 8, doubles after the data, then the IFD
    t.b.push_back(be ? 'M' : 'I'); t.b.push_back(be ? 'M' : 'I');
    t.u16(42);
    const uint32_t dataOff = 8, dataLen = w * h * bps;
    const uint32_t scaleOff = dataOff + dataLen, tieOff = scaleOff + 24, keysOff = tieOff + 48, ifdOff = keysOff + 24;
    t.u32(ifdOff);
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            if (asFloat) { float f = (float)(10 * r + c); uint32_t v; std::memcpy(&v, &f, 4); t.u32(v); }
            else t.u16((uint32_t)(10 * r + c));
        }
    t.f64(0.01); t.f64(0.01); t.f64(0);
    t.f64(0); t.f64(0); t.f64(0); t.f64(25.0); t.f64(55.0); t.f64(0);
    const uint16_t keys[12] = {1, 1, 0, 2, 1024, 0, 1, (uint16_t)modelType, 1025, 0, 1, 1};
    for (uint16_t k : keys) t.u16(k);
    struct E { uint16_t tag, type; uint32_t count, val; };
    const E es[] = {{256, 3, 1, (uint32_t)w}, {257, 3, 1, (uint32_t)h}, {258, 3, 1, (uint32_t)(bps * 8)}, {259, 3, 1, (uint32_t)compression},
                    {273, 4, 1, dataOff}, {277, 3, 1, 1}, {278, 3, 1, (uint32_t)h}, {279, 4, 1, dataLen}, {339, 3, 1, asFloat ? 3u : 2u},
                    {33550, 12, 3, scaleOff}, {33922, 12, 6, tieOff}, {34735, 3, 12, keysOff}};
    t.u16((uint32_t)(sizeof es / sizeof es[0]));
    for (const E& e : es) {
        t.u16(e.tag); t.u16(e.type); t.u32(e.count);
        if (e.type == 3 && e.count == 1) { t.u16(e.val); t.u16(0); }   // a SHORT sits in the first two bytes of the value field
        else t.u32(e.val);
    }
    t.u32(0);
    return t.b;
}

int main() {
    // SRTM .hgt
    ElevTile t;
    std::string err;
    CHECK(parseHgt("N54E025.hgt", hgt(0, 0, 25), t, err));
    CHECK(t.w == 1201 && t.h == 1201);
    NEAR(t.latTop, 55, 1e-12); NEAR(t.lonLeft, 25, 1e-12); NEAR(t.latBottom(), 54, 1e-9); NEAR(t.lonRight(), 26, 1e-9);
    double m = 0;
    CHECK(t.heightAt(54.5, 25.5, m)); NEAR(m, 100, 1e-9);
    CHECK(!t.heightAt(53.9, 25.5, m));
    CHECK(t.heightAt(55.0 - 5 / 1200.0, 25.0 + 5 / 1200.0, m));   // the void sample: its neighbours fill in
    NEAR(m, 100, 1e-9);
    CHECK(parseHgt("s01w072.HGT", hgt(0, 0, -72), t, err));
    NEAR(t.latTop, 0, 1e-12); NEAR(t.lonLeft, -72, 1e-12);
    CHECK(!parseHgt("terrain.hgt", hgt(0, 0, 25), t, err) && err.find("N54E025") != std::string::npos);
    CHECK(!parseHgt("N54E025.hgt", std::vector<uint8_t>(1000), t, err) && err.find("size") != std::string::npos);

    // GeoTIFF, both byte orders, int16 and float32
    for (int k = 0; k < 2; k++) {
        ElevTile g;
        const bool ok = parseGeoTiff(geotiff(k == 1, k == 1, 1, 2), g, err);
        if (!ok) printf("  geotiff %d: %s\n", k, err.c_str());
        CHECK(ok && g.w == 4 && g.h == 3);
        NEAR(g.lonLeft, 25.005, 1e-9); NEAR(g.latTop, 54.995, 1e-9);
        CHECK(g.heightAt(54.995, 25.005, m)); NEAR(m, 0, 1e-9);
        CHECK(g.heightAt(54.985, 25.015, m)); NEAR(m, 11, 1e-9);
        CHECK(g.heightAt(54.99, 25.01, m)); NEAR(m, 5.5, 1e-9);
        CHECK(!g.heightAt(55.5, 25.01, m));
    }
    CHECK(!parseGeoTiff(geotiff(false, false, 5, 2), t, err) && err.find("compressed") != std::string::npos);
    CHECK(!parseGeoTiff(geotiff(false, false, 1, 1), t, err) && err.find("projected") != std::string::npos);
    CHECK(!parseGeoTiff(std::vector<uint8_t>{'P', 'K', 3, 4, 0, 0, 0, 0}, t, err));

    // Bullington: a clear flat path has almost no loss; an obstacle cutting the line has a lot
    {
        std::vector<double> d, h;
        for (int i = 0; i <= 50; i++) { d.push_back(i); h.push_back(0); }
        CHECK(bullingtonDb(d, h, 400, 400, 150) < 1);   // well clear of the first Fresnel zone
        h[25] = 200;
        const double L = bullingtonDb(d, h, 100, 100, 150);
        CHECK(L > 15);
        // the same obstacle seen at a higher frequency costs more
        CHECK(bullingtonDb(d, h, 100, 100, 1500) > L);
    }
    // delta-Bullington over flat terrain is the smooth-earth diffraction
    Terrain flat;
    CHECK(parseHgt("N54E025.hgt", hgt(0, 0, 25), t, err));
    flat.tiles.push_back(t);
    {
        const Profile pr = pathProfile(flat, 54.5, 25.1, 54.5, 25.9, 100);
        CHECK(pr.complete && pr.dKm.size() == 100);
        NEAR(pr.dKm.back(), greatCircleKm(54.5, 25.1, 54.5, 25.9), 1e-6);
        const TerrainLoss tl = terrainLoss(pr, 30, 10, 150);
        NEAR(tl.effTxM, 30, 1e-6); NEAR(tl.effRxM, 10, 1e-6);
        NEAR(tl.diffractionDb, diffractionLossDb(150, pr.dKm.back(), 30, 10), 0.01);
    }
    // a ridge between transmitter and receiver
    Terrain hill;
    CHECK(parseHgt("N54E025.hgt", hgt(450, 25.5, 25), t, err));
    hill.tiles.push_back(t);
    CHECK(hill.describe().find("1 file") == 0);
    {
        const Profile pr = pathProfile(hill, 54.5, 25.2, 54.5, 25.8, 200);
        const TerrainLoss tl = terrainLoss(pr, 10, 10, 145);
        CHECK(!tl.los && tl.diffractionDb > 20);
        Params p; p.txLat = 54.5; p.txLon = 25.2; p.fMHz = 145; p.txHeightM = 10; p.rxHeightM = 10; p.txPowerDbw = 14; p.env = Env::Quiet;
        // 4.0 (P.1812 over the profile): compare with the same tile without the ridge
        Terrain level; ElevTile lt; std::string e4;
        CHECK(parseHgt("N54E025.hgt", hgt(0, 0, 25), lt, e4));
        level.tiles.push_back(lt);
        p.terrain = &level;
        const Point smooth = evaluateAt(p, 54.5, 25.8);
        const Point beforeLevel = evaluateAt(p, 54.5, 25.4);
        p.terrain = &hill;
        const Point withHill = evaluateAt(p, 54.5, 25.8);
        CHECK(withHill.terrainUsed && (withHill.model == Model::Terrain || withHill.model == Model::Troposcatter || withHill.model == Model::Ducting));
        printf("  145 MHz, 10 m antennas, 38 km: level tile %.1f dB, with a 350 m ridge %.1f dB (P.1812)\n", smooth.pathLossDb, withHill.pathLossDb);
        CHECK(withHill.pathLossDb > smooth.pathLossDb + 3);   // the level path is already beyond the horizon (spherical diffraction): the ridge adds a few dB more
        // in front of the ridge nothing changes much; beyond the tile the model falls back to a flat profile
        const Point before = evaluateAt(p, 54.5, 25.4);
        CHECK(std::fabs(before.pathLossDb - beforeLevel.pathLossDb) < 3);
        const Point outside = evaluateAt(p, 54.5, 26.5);
        CHECK(!outside.terrainUsed && outside.flatEarth);
        // the 3.9 model is still there
        Params old = p; old.pathModel = PathModel::Legacy39;
        CHECK(evaluateAt(old, 54.5, 25.8).model == Model::Terrain || evaluateAt(old, 54.5, 25.8).model == Model::Troposcatter);
    }
    // load from a file path (written to the current directory, removed again)
    {
        const char* path = "N54E025.hgt";
        FILE* f = std::fopen(path, "wb");
        if (f) {
            const auto b = hgt(0, 0, 25);
            std::fwrite(b.data(), 1, b.size(), f);
            std::fclose(f);
            Terrain tt;
            CHECK(loadElevationFile(path, tt, err) && tt.tiles.size() == 1);
            CHECK(loadElevationFile(path, tt, err) && tt.tiles.size() == 1);   // the same file twice: replaced
            std::remove(path);
        }
        Terrain tt;
        CHECK(!loadElevationFile("does-not-exist.hgt", tt, err) && !err.empty());
    }
    // 4.0: downloadable tiles (names, URL, which tiles a path needs) and the .hgt.gz reader
    CHECK(demTileName(54, 25) == "N54E025");
    CHECK(demTileName(-34, -71) == "S34W071");
    CHECK(demTileUrl(25, 55) == "https://s3.amazonaws.com/elevation-tiles-prod/skadi/N25/N25E055.hgt.gz");
    {
        const auto one = demTilesFor(54.5, 25.5, 10, NAN, NAN);
        CHECK(one.size() == 1 && one[0].first == 54 && one[0].second == 25);
        const auto path = demTilesFor(54.5, 25.5, 10, 54.5, 27.5);   // 130 km east: 3 tiles in a row
        CHECK(path.size() == 3);
        CHECK(demTilesFor(54.5, 25.5, 200, 40, 10, 9).size() <= 9);
        CHECK(demTilesFor(75, 25, 10, NAN, NAN).empty());               // beyond SRTM's 60 N
    }
    {
        const std::vector<uint8_t> raw = hgt(450, 25.5, 25);
        const std::vector<uint8_t> gz = dect2::winlink::gzipCompress(raw);
        FILE* f = std::fopen("N54E025.hgt.gz", "wb");
        CHECK(f != nullptr);
        if (f) { std::fwrite(gz.data(), 1, gz.size(), f); std::fclose(f); }
        Terrain tz; std::string e2;
        CHECK(loadElevationFile("N54E025.hgt.gz", tz, e2));
        ElevTile direct; std::string e3;
        CHECK(parseHgt("N54E025.hgt", raw, direct, e3));
        double a = 0, b = 0;
        CHECK(tz.heightAt(54.5, 25.5, a) && direct.heightAt(54.5, 25.5, b));
        NEAR(a, b, 1e-6);
        std::remove("N54E025.hgt.gz");
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("prop_terrain: ok\n");
    return 0;
}
