// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GOES HRIT layers above the demodulator (goes_proto.h): the header records both ways, the test files through CP_PDUs, M_PDUs and
// VCDUs (and through the CADU coding with byte errors Reed-Solomon must correct) back into products that match byte for byte: the
// Rice full disk in two segments, the plain mesoscale image, the JPEG, the admin text and the zipped EMWIN text; a lost VCDU marks the
// file it hit and the rest still decode.
#include "dect2/goes/goes_gen.h"
#include "dect2/goes/goes_proto.h"
#include "dect2/hfdig/hfdig_hamdrm.h"
#include "dect2/meteor/meteor_proto.h"
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

struct Result {
    std::vector<std::shared_ptr<goes::Product>> products;
    std::vector<std::vector<uint8_t>> files;
    uint64_t crc = 0, gaps = 0, fileErrors = 0;
    int scid = -1;
};

// mode 0: VCDUs straight; 1: through the CADU coding with errors; 2: one VCDU of the mesoscale image lost
static Result run(int mode) {
    const std::vector<GoesTestFile> tf = goesTestFiles();
    goes::Mux mux(0x7E);
    uint16_t counter = 0;
    for (const auto& f : tf) mux.addFile(f.vcid, goesFileOut(f, counter++));
    goes::Assembler a;
    Result r;
    a.onFile = [&](const std::vector<uint8_t>& f, int, int) { r.files.push_back(f); };
    std::mt19937 rng(11);
    uint8_t v[goes::kVcduBytes];
    int n = 0;
    bool dropped = false;
    while (mux.next(v)) {
        n++;
        if (mode == 1) {
            uint8_t cadu[meteor::kCaduBytes], back[meteor::kVcduBytes];
            meteor::encodeCadu(v, cadu);
            for (int e = 0; e < 40; e++) cadu[4 + rng() % meteor::kCodedBytes] ^= (uint8_t)(1 + rng() % 255);   // about 10 a codeword
            const int c = meteor::decodeCadu(cadu + 4, back);
            CHECK(c >= 0 && memcmp(back, v, sizeof back) == 0, "CADU %d: Reed-Solomon %d", n, c);
            memcpy(v, back, sizeof v);
        }
        if (mode == 2 && !dropped && (v[1] & 63) == 2 && ((v[2] << 16) | (v[3] << 8) | v[4]) == 1) { dropped = true; continue; }
        a.push(v);
        if (n % 3 == 0) { uint8_t f[goes::kVcduBytes]; mux.fill(f); a.push(f); }
    }
    r.products = a.products();
    r.crc = a.crcErrors; r.gaps = a.vcduGaps; r.fileErrors = a.fileErrors; r.scid = a.scid;
    return r;
}

static std::shared_ptr<goes::Product> find(const Result& r, int kind) {
    for (const auto& p : r.products) if (p->kind == kind) return p;
    return nullptr;
}

int main() {
    // headers both ways
    {
        goes::Headers h;
        h.fileType = 0; h.hasImage = true; h.bpp = 8; h.columns = 2712; h.lines = 226; h.compression = 1;
        h.hasNav = true; h.projection = "GEOS(-137.0)"; h.cfac = 20425338; h.lfac = -20425338; h.coff = 1356; h.loff = 1356;
        h.annotation = "OR_ABI-L2-CMIPF-M6C13_G18_s2026"; h.hasTime = true; h.days = 25121; h.msOfDay = 123456;
        h.hasSeg = true; h.imageId = 12; h.segSeq = 3; h.startLine = 452; h.maxSeg = 12; h.maxCol = 2712; h.maxRow = 2712;
        h.hasNoaa = true; h.productId = 18; h.subId = 1; h.parameter = 13; h.noaaCompression = 1;
        h.hasRice = true; h.riceFlags = 49; h.ricePpb = 16; h.riceLines = 1; h.dataBits = 123456;
        const std::vector<uint8_t> b = goes::buildHeaders(h);
        goes::Headers g;
        CHECK(goes::parseHeaders(b.data(), b.size(), g), "parse");
        CHECK(g.headerLength == b.size() && g.dataBits == 123456 && g.columns == 2712 && g.lines == 226 && g.compression == 1 && g.projection == "GEOS(-137.0)" &&
              g.lfac == -20425338 && g.annotation == h.annotation && g.days == 25121 && g.msOfDay == 123456 && g.segSeq == 3 && g.startLine == 452 &&
              g.maxSeg == 12 && g.productId == 18 && g.noaaCompression == 1 && g.riceFlags == 49 && g.ricePpb == 16, "fields");
        CHECK(goes::classify(g) == goes::KFullDisk, "full disk");
        CHECK(std::fabs(goes::headerTime(g) - (1791763200.0 + 123.456)) < 0.01, "time %.3f", goes::headerTime(g));
        CHECK(!goes::parseHeaders(b.data(), 10, g), "short");
    }
    int fw, fh, mw, mh;
    const std::vector<uint8_t> fd = goesTestFullDisk(fw, fh), meso = goesTestMeso(mw, mh);
    const std::vector<GoesTestFile> tf = goesTestFiles();
    for (int mode = 0; mode < 3; mode++) {
        const Result r = run(mode);
        printf("  mode %d: %zu products, %zu files, %llu CRC errors, %llu gaps\n", mode, r.products.size(), r.files.size(), (unsigned long long)r.crc, (unsigned long long)r.gaps);
        CHECK(r.scid == 0x7E, "spacecraft %d", r.scid);
        CHECK(r.files.size() == tf.size(), "files %zu", r.files.size());
        if (mode < 2) {
            CHECK(r.crc == 0 && r.gaps == 0 && r.fileErrors == 0, "errors");
            for (size_t i = 0; i < tf.size() && i < r.files.size(); i++) CHECK(r.files[i] == tf[i].file, "file %zu not byte exact (%zu vs %zu)", i, r.files[i].size(), tf[i].file.size());
        } else CHECK(r.gaps == 1 && r.fileErrors == 1, "gap %llu, file errors %llu", (unsigned long long)r.gaps, (unsigned long long)r.fileErrors);
        CHECK(r.products.size() == 5, "products %zu", r.products.size());
        auto p = find(r, goes::KFullDisk);
        CHECK(p && p->w == fw && p->h == fh && p->complete && p->segsDone == 2 && p->rowsDone == fh && p->pix == fd, "full disk image");
        auto m = find(r, goes::KMeso);
        if (mode < 2) CHECK(m && m->w == mw && m->h == mh && m->complete && !m->errors && m->pix == meso, "mesoscale image");
        else CHECK(m && m->errors, "mesoscale marked");
        auto j = find(r, goes::KImage);
        CHECK(j && j->data == hamdrmTestPicture() && goes::compressionName(j->hdr) == std::string("JPEG"), "JPEG");
        auto ad = find(r, goes::KAdmin);
        CHECK(ad && ad->text == goesTestAdminText(), "admin text");
        auto em = find(r, goes::KEmwin);
        CHECK(em && em->text == goesTestEmwinText() && em->innerName == "ZFPOUNOK.TXT", "EMWIN text");
    }
    printf(fails ? "goes proto: %d FAILED\n" : "goes proto: all passed\n", fails);
    return fails ? 1 : 0;
}
