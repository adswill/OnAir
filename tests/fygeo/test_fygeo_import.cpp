// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FengYun file import: a .cadu file (1024 byte frames, no radio) through the FY-4 file layers, an encrypted file (kept and counted, never decoded),
// a damaged picture segment (listed with errors), a file with a lost packet, and a .svissr file (44356 byte descrambled frames) into the five pictures.
#include "dect2/fygeo/fygeo_gen.h"
#include "dect2/fygeo/fygeo_rx.h"
#include "dect2/fygeo/fygeo_tel.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<uint8_t> caduFile(const std::vector<Fy4TestFile>& files, int dropPacketOf = -1) {
    goes::Mux mux(0x64, fygeo::kZoneBytes);
    uint16_t cnt = 0;
    int n = 0;
    for (const Fy4TestFile& f : files) {
        goes::FileOut o = fy4FileOut(f, cnt++);
        if (n++ == dropPacketOf && o.chunks.size() > 3) o.chunks.erase(o.chunks.begin() + 2);   // one packet lost: the sequence count jumps
        mux.addFile(f.vcid, o);
    }
    std::vector<uint8_t> out;
    uint8_t v[8 + fygeo::kZoneBytes];
    while (mux.next(v)) {
        out.push_back(0x1A); out.push_back(0xCF); out.push_back(0xFC); out.push_back(0x1D);
        out.insert(out.end(), v, v + 1020);
    }
    mux.fill(v);
    out.push_back(0x1A); out.push_back(0xCF); out.push_back(0xFC); out.push_back(0x1D);
    out.insert(out.end(), v, v + 1020);
    return out;
}

static FygeoTelemetry feedAndGet(FygeoReceiver& rx, double rate) {
    std::vector<cf32> z(16384, cf32(1e-4f, 0));
    for (int i = 0; i < 60; i++) rx.feed(z.data(), z.size());   // 0.5 s: enough for a report
    FygeoTelemetry t;
    rx.telemetry(t, 0);
    (void)rate;
    return t;
}

int main() {
    {   // a clean .cadu file
        FygeoReceiver rx;
        rx.setLink(kFy4aLrit);
        rx.configure(2e6);
        int files = 0;
        rx.setFileCallback([&](const std::vector<uint8_t>&, int, int) { files++; });
        rx.importFrames(caduFile(fy4TestFiles(0)));
        rx.importFrames(caduFile(fy4TestFiles(1)));
        FygeoTelemetry t = feedAndGet(rx, 2e6);
        CHECK(files == 10, "%d files (two rounds of four pictures segments and a message)", files);
        int done = 0;
        for (const auto& p : t.products) { std::lock_guard<std::mutex> lk(p->mu); if (p->complete && p->w > 0) done++; }
        CHECK(done == 4, "%d complete pictures", done);
        CHECK(t.imageSegments == 8 && t.imagesDone == 4, "%llu segments, %llu pictures", (unsigned long long)t.imageSegments, (unsigned long long)t.imagesDone);
        // the pictures of the two rounds differ in time and are two products each channel
        int c2 = 0;
        for (const auto& p : t.products) if (p->name.find("C02") != std::string::npos) c2++;
        CHECK(c2 == 2, "%d C02 pictures", c2);
        for (const auto& p : t.products) {
            if (p->w == 0 || p->name.find("C12") == std::string::npos) continue;
            const std::vector<uint16_t> ref = fy4TestPicture(12);
            std::lock_guard<std::mutex> lk(p->mu);
            int diff = 0;
            for (size_t i = 0; i < ref.size(); i++) diff += p->pix[i] != (ref[i] >> 4);
            CHECK(diff == 0, "C12 pixels differ: %d", diff);
        }
    }
    {   // an encrypted file, a damaged segment, a lost packet
        FygeoReceiver rx;
        rx.setLink(kFy4bLrit);
        rx.configure(2e6);
        std::vector<Fy4TestFile> fs = fy4TestFiles(0);
        Fy4TestFile enc = fs[0];
        enc.h.hasKey = true; enc.h.key = 77;
        enc.h.annotation = "FY4A-_AGRI--_N_DISK_1047E_L1-_FDI-_C001_NOM_20261101120000_20261101120417_4000M_V0001.HDF";
        enc.file = fygeo::buildFy4Headers(enc.h);
        enc.file.insert(enc.file.end(), enc.dataChunks[0].begin(), enc.dataChunks[0].end());
        Fy4TestFile bad = fs[2];                       // channel 12 segment 1: its code stream cut short
        bad.dataChunks.resize(bad.dataChunks.size() / 2);
        std::vector<Fy4TestFile> all = {enc, bad, fs[3], fs[4]};
        rx.importFrames(caduFile(all));
        FygeoTelemetry t = feedAndGet(rx, 2e6);
        CHECK(t.encryptedFiles == 1, "%llu encrypted", (unsigned long long)t.encryptedFiles);
        bool encListed = false, errs = false;
        for (const auto& p : t.products) {
            std::lock_guard<std::mutex> lk(p->mu);
            if (p->text.find("Encrypted") != std::string::npos) encListed = true;
            if (p->w > 0 && p->errors) errs = true;
        }
        CHECK(encListed, "the encrypted file is listed");
        CHECK(errs, "the damaged segment marks its picture with errors");
    }
    {   // S-VISSR frames from a file
        FygeoReceiver rx;
        rx.setLink(kFy2Svissr);
        rx.configure(2.4e6);
        std::vector<uint8_t> d;
        for (int line = 0; line < 6; line++) {
            std::vector<uint8_t> ir[4], vis((size_t)4 * fygeo::kSvissrVisWidth), f((size_t)fygeo::kSvissrFrameBytes);
            for (auto& v : ir) v.resize(fygeo::kSvissrIrWidth);
            svissrTestLine(line, ir[0].data(), ir[1].data(), ir[2].data(), ir[3].data(), vis.data());
            fygeo::svissrPack(f.data(), line, ir[0].data(), ir[1].data(), ir[2].data(), ir[3].data(), vis.data());
            d.insert(d.end(), f.begin(), f.end());
        }
        rx.importFrames(d);
        FygeoTelemetry t = feedAndGet(rx, 2.4e6);
        CHECK(t.svFrames == 6 && t.svLine == 5, "%llu frames, line %d", (unsigned long long)t.svFrames, t.svLine);
        int pics = 0;
        for (const auto& p : t.products) { std::lock_guard<std::mutex> lk(p->mu); if (p->rowsDone > 0) pics++; }
        CHECK(pics == 5, "%d pictures", pics);
        // a second picture: the counter wraps
        std::vector<uint8_t> ir[4], vis((size_t)4 * fygeo::kSvissrVisWidth), f((size_t)fygeo::kSvissrFrameBytes);
        for (auto& v : ir) v.resize(fygeo::kSvissrIrWidth);
        svissrTestLine(0, ir[0].data(), ir[1].data(), ir[2].data(), ir[3].data(), vis.data());
        std::vector<uint8_t> d2;
        for (int c : {2000, 2001, 5, 6}) {   // far on, then the next picture
            fygeo::svissrPack(f.data(), c, ir[0].data(), ir[1].data(), ir[2].data(), ir[3].data(), vis.data());
            d2.insert(d2.end(), f.begin(), f.end());
        }
        rx.importFrames(d2);
        t = feedAndGet(rx, 2.4e6);
        CHECK(t.svImages == 2, "%d pictures started", t.svImages);
        CHECK(t.products.size() == 10, "%zu products", t.products.size());
    }
    return testSummary("fygeo_import");
}
