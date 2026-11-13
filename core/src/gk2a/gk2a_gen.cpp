// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GK-2A LRIT / HRIT test signals (gk2a_gen.h).
#include "dect2/gk2a/gk2a_gen.h"
#include "dect2/common/des.h"
#include "dect2/gk2a/gk2a_tel.h"
#include "dect2/image/j2k.h"
#include <cmath>
#include <cstring>

namespace dect2 {

std::array<uint8_t, 8> gk2aTestKey() { return {'O', 'n', 'A', 'i', 'r', 'G', 'K', '2'}; }

std::vector<uint8_t> gk2aTestKeyFile() {
    std::vector<uint8_t> f{0, 1, 0, 1};
    const auto k = gk2aTestKey();
    f.insert(f.end(), k.begin(), k.end());
    return f;
}

std::vector<uint16_t> gk2aTestPicture(int ch, int& w, int& h) {
    w = 128; h = 96;
    std::vector<uint16_t> p((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const double dx = (x - w / 2.0) / (h / 2.0 - 2), dy = (y - h / 2.0) / (h / 2.0 - 2), r = std::sqrt(dx * dx + dy * dy);
            const double cloud = 0.5 + 0.5 * std::sin(x * 0.21 + std::sin(y * 0.13) * 2.0) * std::cos(y * 0.17 - x * 0.05);
            double v;
            if (r > 1) v = 12;                                        // space
            else if (ch == 0) v = 220 + 520 * cloud * (1 - 0.4 * r) + 90 * std::sqrt(1 - r * r);   // infrared: warm ground, bright cold cloud tops
            else v = 140 + 420 * cloud * std::sqrt(1 - r * r) + 120 * std::sqrt(1 - r * r);          // visible: bright clouds, dark sea
            if (ch == 0 && std::fabs(dx - 0.35) < 0.06 && std::fabs(dy + 0.2) < 0.25) v = 1000;     // a cold cloud top
            p[(size_t)y * w + x] = (uint16_t)std::max(0.0, std::min(1023.0, v));
        }
    return p;
}

std::vector<GoesTestFile> gk2aTestFiles(bool encryptVisible) {
    std::vector<GoesTestFile> v;
    static const int kChan[2] = {12, 2};            // IR105, VI006
    for (int c = 0; c < 2; c++) {
        int w, h;
        const std::vector<uint16_t> pic = gk2aTestPicture(c, w, h);
        const int segLines = h / kGk2aTestSegments;
        for (int s = 0; s < kGk2aTestSegments; s++) {
            const std::vector<uint8_t> cs = j2k::encodeLossless(pic.data() + (size_t)s * segLines * w, w, segLines, 10, 3);
            GoesTestFile f;
            f.vcid = 0; f.apid = kChan[c];
            goes::Headers hd;
            hd.fileType = 0;
            char nm[96];
            snprintf(nm, sizeof nm, "IMG_FD_143_%s_20261101_030000_%02d.lrit", goes::gk2aChannelName(kChan[c]), s + 1);
            hd.annotation = nm;
            hd.hasTime = true; hd.days = 25141; hd.msOfDay = 10800000;      // 2026-11-01 03:00:00 UTC
            hd.hasImage = true; hd.bpp = 10; hd.columns = w; hd.lines = segLines; hd.compression = 1;
            hd.hasNav = true; hd.projection = "GEOS(128.2)"; hd.cfac = 4;  hd.lfac = 4; hd.coff = w / 2; hd.loff = h / 2 - s * segLines;
            hd.hasSeg = true; hd.segSeq = s + 1; hd.maxSeg = kGk2aTestSegments; hd.startLine = s * segLines;
            std::vector<uint8_t> data = cs;
            if (c == 1 && encryptVisible) {
                hd.keyNo = 1;
                while (data.size() % 8) data.push_back(0);                  // 64 bit alignment of DES, counted in the data field length
                Des d; const auto k = gk2aTestKey(); d.setKey(k.data()); d.ecb(data.data(), data.size(), false);
            }
            hd.dataBits = (uint64_t)data.size() * 8;
            f.h = hd;
            const size_t chunk = 60000;
            for (size_t o = 0; o < data.size(); o += chunk) f.dataChunks.emplace_back(data.begin() + (std::ptrdiff_t)o, data.begin() + (std::ptrdiff_t)std::min(data.size(), o + chunk));
            f.dialect = 1;
            f.file = goes::makeFile(hd, data, 1);
            v.push_back(std::move(f));
        }
    }
    auto text = [&](int vc, int apid, const char* name, const std::string& body) {
        GoesTestFile f;
        f.vcid = vc; f.apid = apid;
        goes::Headers hd;
        hd.fileType = 2; hd.annotation = name;
        hd.hasTime = true; hd.days = 25141; hd.msOfDay = 10800000;
        const std::vector<uint8_t> d(body.begin(), body.end());
        f.dataChunks.push_back(d);
        hd.dataBits = (uint64_t)d.size() * 8;
        f.h = hd;
        f.dialect = 1;
        f.file = goes::makeFile(hd, d, 1);
        v.push_back(std::move(f));
    };
    text(4, 128, "ADD_ANT_143_20261101_030000_01.lrit", "OnAir GK-2A LRIT test signal\r\nOBSERVATION SCHEDULE (synthetic)\r\nFD every 10 min, nothing is transmitted.\r\n");
    text(5, 160, "ADD_SEA_007_20261101_030000_01.lrit", "OnAir GK-2A additional data test file\r\nSEA STATE: SYNTHETIC\r\n");
    return v;
}

namespace {
class Gk2aSynth : public ModeSynth {
public:
    Gk2aSynth(const SynthConfig& s, double rate, int link) : rate_(rate) {
        GoesGenConfig c;
        c.rate = rate;
        c.symbolRate = gk2aSymbolRate(link);
        c.qpsk = link == 1;
        c.snrDb = s.snrDb; c.cfoHz = s.cfoHz; c.sroPpm = s.sroPpm;
        c.scid = 0x68;
        c.fillFrames = link ? 3000 : 60;
        c.files = gk2aTestFiles(s.modeOpt[0] != 0);
        gen_ = std::make_unique<GoesGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<GoesGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeGk2aLritSynth(const SynthConfig& cfg, double sampleRate) { return sampleRate > 0 ? std::make_unique<Gk2aSynth>(cfg, sampleRate, 0) : nullptr; }
std::unique_ptr<ModeSynth> makeGk2aHritSynth(const SynthConfig& cfg, double sampleRate) { return sampleRate > 0 ? std::make_unique<Gk2aSynth>(cfg, sampleRate, 1) : nullptr; }

} // namespace dect2
