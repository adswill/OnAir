// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// X-Midas BLUE files (core/src/source/bluefile.cpp): written by our writer in every type / format / byte order and read back, a hand-built
// header (big-endian, keywords, type 2000), damaged and truncated files, and the file-source probe (probeIqFile).
// Built standalone (no engine) with -DBLUE_ONLY for a quick check; the probe checks need the whole core.
#include "dect2/source/bluefile.h"
#ifndef BLUE_ONLY
#include "dect2/source/iq_file.h"
#include "dect2/source/iq_record.h"
#include "dect2/tx/tx_iqfile.h"
#endif
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace dect2;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); fails++; } } while (0)

static fs::path dir;
static std::string P(const char* n) { return (dir / n).string(); }

static std::vector<cf32> signal(size_t n) {
    std::vector<cf32> v(n);
    for (size_t i = 0; i < n; i++) v[i] = cf32(0.6f * std::cos(0.05f * (float)i), -0.4f * std::sin(0.031f * (float)i));
    return v;
}

static std::vector<uint8_t> slurp(const std::string& p) {
    std::vector<uint8_t> b;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return b;
    uint8_t t[4096]; size_t n;
    while ((n = fread(t, 1, sizeof t, f)) > 0) b.insert(b.end(), t, t + n);
    fclose(f);
    return b;
}
static void spit(const std::string& p, const std::vector<uint8_t>& b, size_t n = (size_t)-1) {
    FILE* f = fopen(p.c_str(), "wb");
    fwrite(b.data(), 1, n < b.size() ? n : b.size(), f);
    fclose(f);
}

static double tol(char kind) { return kind == 'B' ? 0.012 : kind == 'I' ? 1e-4 : kind == 'L' ? 1e-6 : 1e-6; }

static void roundTrip(bool complex, char kind, bool big) {
    char name[64];
    snprintf(name, sizeof name, "rt_%c%c_%s.tmp", complex ? 'C' : 'S', kind, big ? "be" : "le");
    const std::string path = P(name);
    const auto x = signal(5000);
    blue::WriteSpec s;
    s.complex = complex; s.kind = kind; s.bigEndian = big; s.sampleRate = 2.4e6; s.centerHz = 433.92e6; s.description = "round trip";
    blue::Writer w;
    std::string err;
    CHECK(w.open(path, s, err), "writer opens");
    // two blocks, so streaming appends
    CHECK(w.write(x.data(), 2000, err), "write 1");
    CHECK(w.write(x.data() + 2000, 3000, err), "write 2");
    CHECK(w.close(err), "close");
    blue::Info i;
    CHECK(blue::probe(path, i, err), name);
    CHECK(i.samples == 5000, "sample count");
    CHECK(i.complex == complex && i.kind == kind, "format letters");
    CHECK(i.headBig == big && i.dataBig == big, "byte order");
    CHECK(std::fabs(i.sampleRate - 2.4e6) < 1e-3, "rate from xdelta");
    CHECK(std::fabs(i.centerHz - 433.92e6) < 1, "centre frequency keyword");
    CHECK(i.description == "round trip", "description keyword");
    CHECK(!i.truncated, "not truncated");
    blue::Reader r;
    CHECK(r.open(path, err), "reader opens");
    std::vector<cf32> y(5000);
    size_t got = 0, g;
    while (got < 5000 && (g = r.read(y.data() + got, 777)) > 0) got += g;
    CHECK(got == 5000, "all samples read");
    CHECK(r.read(y.data(), 10) == 0, "end of data");
    double worst = 0;
    for (size_t k = 0; k < got; k++) {
        worst = std::max(worst, (double)std::fabs(y[k].real() - x[k].real()));
        worst = std::max(worst, complex ? (double)std::fabs(y[k].imag() - x[k].imag()) : (double)std::fabs(y[k].imag()));
    }
    CHECK(worst <= tol(kind), name);
    r.rewind();
    cf32 one;
    CHECK(r.read(&one, 1) == 1 && std::fabs(one.real() - x[0].real()) <= tol(kind), "rewind");
}

// header builder for the hand-made files
static void put(std::vector<uint8_t>& b, size_t off, uint64_t v, int n, bool big) { for (int i = 0; i < n; i++) b[off + (big ? n - 1 - i : i)] = (uint8_t)(v >> (8 * i)); }
static void putd(std::vector<uint8_t>& b, size_t off, double d, bool big) { uint64_t u; memcpy(&u, &d, 8); put(b, off, u, 8, big); }

static std::vector<uint8_t> handFile(bool big, const char* fmt2, int type, double xdelta, int xunits, size_t nbytes, double declared) {
    std::vector<uint8_t> b(512 + nbytes, 0);
    memcpy(&b[0], "BLUE", 4);
    memcpy(&b[4], big ? "IEEE" : "EEEI", 4);
    memcpy(&b[8], big ? "IEEE" : "EEEI", 4);
    putd(b, 32, 512, big); putd(b, 40, declared, big);
    put(b, 48, (uint64_t)type, 4, big);
    b[52] = (uint8_t)fmt2[0]; b[53] = (uint8_t)fmt2[1];
    putd(b, 256, 0.0, big); putd(b, 264, xdelta, big); put(b, 272, (uint64_t)xunits, 4, big);
    return b;
}

int main() {
    dir = fs::temp_directory_path() / "onair_test_bluefile";
    fs::create_directories(dir);

    for (bool c : {true, false}) for (char k : {'B', 'I', 'L', 'F', 'D'}) for (bool be : {false, true}) roundTrip(c, k, be);

    std::string err;
    // big-endian CI hand-built file with an extended header holding one string and one double keyword, and type 2000
    {
        std::vector<uint8_t> b = handFile(true, "CI", 2000, 1.0 / 250000.0, 1, 4 * 8, 32);
        put(b, 276, 4, 4, true);   // subsize
        const int16_t v[16] = {1000, -1000, 2000, -2000, 3000, -3000, 4000, -4000, 5000, 0, -5000, 0, 100, 200, 300, 400};
        for (int k = 0; k < 16; k++) put(b, 512 + 2 * k, (uint16_t)v[k], 2, true);
        // data is 32 bytes -> padded to 512 -> extended header at block 2
        b.resize(1024, 0);
        std::vector<uint8_t> e;
        auto kw = [&](const char* tag, char type, const std::vector<uint8_t>& val) {
            const size_t total = (8 + val.size() + strlen(tag) + 7) & ~(size_t)7;
            const size_t o = e.size(); e.resize(o + total, 0);
            put(e, o, total, 4, true); put(e, o + 4, total - val.size(), 2, true); e[o + 6] = (uint8_t)strlen(tag); e[o + 7] = (uint8_t)type;
            memcpy(&e[o + 8], val.data(), val.size()); memcpy(&e[o + 8 + val.size()], tag, strlen(tag));
        };
        std::vector<uint8_t> d(8); { uint64_t u; double f = 98.5e6; memcpy(&u, &f, 8); put(d, 0, u, 8, true); }
        kw("RF_FREQ", 'D', d);
        kw("COMMENT", 'A', {'h', 'i', ' ', 't', 'h', 'e', 'r', 'e'});
        std::vector<uint8_t> l(4); put(l, 0, 42, 4, true);
        kw("COUNT", 'L', l);
        put(b, 24, 2, 4, true); put(b, 28, e.size(), 4, true);
        b.insert(b.end(), e.begin(), e.end());
        spit(P("hand.blue"), b);
        blue::Info i;
        CHECK(blue::probe(P("hand.blue"), i, err), "hand-built big-endian file");
        CHECK(i.type == 2000 && i.subsize == 4 && i.samples == 8, "type 2000 fields");
        CHECK(std::fabs(i.sampleRate - 250000) < 1e-6, "rate");
        CHECK(std::fabs(i.centerHz - 98.5e6) < 1e-3, "keyword double");
        CHECK(i.description == "hi there", "keyword text");
        bool cnt = false; for (auto& kv : i.keywords) if (kv.first == "COUNT" && kv.second == "42") cnt = true;
        CHECK(cnt, "keyword long");
        blue::Reader r;
        CHECK(r.open(P("hand.blue"), err), "open hand");
        cf32 y[8];
        CHECK(r.read(y, 8) == 8, "8 samples");
        CHECK(std::fabs(y[0].real() - 1000 / 32768.0) < 1e-6 && std::fabs(y[0].imag() + 1000 / 32768.0) < 1e-6, "big-endian int16 values");
        CHECK(std::fabs(y[4].real() - 5000 / 32768.0) < 1e-6, "sample 4");
    }
    // mixed byte orders: little header, big data (allowed by the format)
    {
        std::vector<uint8_t> b = handFile(false, "SF", 1000, 1e-6, 1, 8, 8);
        memcpy(&b[8], "IEEE", 4);
        const float f[2] = {0.5f, -0.25f};
        for (int k = 0; k < 2; k++) { uint32_t u; memcpy(&u, &f[k], 4); put(b, 512 + 4 * k, u, 4, true); }
        spit(P("mixed.tmp"), b);
        blue::Info i; blue::Reader r; cf32 y[2];
        CHECK(r.open(P("mixed.tmp"), err) && r.read(y, 2) == 2, "mixed orders read");
        CHECK(y[0] == cf32(0.5f, 0) && y[1] == cf32(-0.25f, 0), "mixed order real float");
    }
    // other types: uint8 offset binary and int64
    {
        std::vector<uint8_t> b = handFile(false, "CO", 1000, 1e-6, 1, 4, 4);
        b[512] = 128; b[513] = 255; b[514] = 0; b[515] = 128;
        spit(P("u8.tmp"), b);
        blue::Reader r; cf32 y[2];
        CHECK(r.open(P("u8.tmp"), err) && r.read(y, 2) == 2, "CO read");
        CHECK(y[0].real() == 0.f && y[0].imag() > 0.99f && y[1].real() == -1.f, "uint8 offset binary");
    }
    // header problems
    {
        auto bad = [&](const char* name, std::vector<uint8_t> b, const char* why) {
            spit(P(name), b);
            blue::Info i; std::string e;
            CHECK(!blue::probe(P(name), i, e) && !e.empty(), why);
        };
        auto base = handFile(false, "CF", 1000, 1e-6, 1, 64, 64);
        auto b1 = base; b1[0] = 'X'; bad("bad_magic.tmp", b1, "wrong tag");
        auto b2 = base; memcpy(&b2[4], "XXXX", 4); bad("bad_rep.tmp", b2, "wrong head_rep");
        auto b3 = base; b3[52] = 'V'; bad("bad_fmt.tmp", b3, "vector format refused");
        auto b4 = base; b4[53] = 'Z'; bad("bad_kind.tmp", b4, "unknown sample type");
        auto b5 = base; put(b5, 48, 3000, 4, false); bad("bad_type.tmp", b5, "type 3000 refused");
        auto b6 = base; putd(b6, 32, 100, false); bad("bad_start.tmp", b6, "data_start inside the header");
        auto b7 = base; putd(b7, 32, 1e12, false); bad("far_start.tmp", b7, "data_start past the file");
        auto b8 = base; put(b8, 12, 1, 4, false); bad("detached.tmp", b8, "detached header");
        auto b9 = base; putd(b9, 32, std::nan(""), false); bad("nan_start.tmp", b9, "NaN data_start");
        bad("short.tmp", std::vector<uint8_t>(100, 0), "shorter than a header");
        bad("empty.tmp", {}, "empty file");
    }
    // truncated data and a header that was never finished (data_size 0)
    {
        auto b = handFile(false, "CF", 1000, 1e-6, 1, 8 * 10 + 3, 8 * 100);
        spit(P("trunc.tmp"), b);
        blue::Info i;
        CHECK(blue::probe(P("trunc.tmp"), i, err), "truncated file still opens");
        CHECK(i.truncated && i.samples == 10, "truncated: whole samples only");
        auto z = handFile(false, "CF", 1000, 1e-6, 1, 8 * 7, 0);
        spit(P("unfinished.tmp"), z);
        CHECK(blue::probe(P("unfinished.tmp"), i, err) && i.sizeFromFile && i.samples == 7, "data_size 0: size from the file");
        // a damaged extended header (record length points past the end) keeps the file readable
        auto d = handFile(false, "CF", 1000, 1e-6, 1, 0, 0);
        d.resize(1024, 0);
        put(d, 24, 1, 4, false); put(d, 28, 16, 4, false);
        put(d, 512, 9999, 4, false);
        spit(P("badext.tmp"), d);
        CHECK(blue::probe(P("badext.tmp"), i, err) && i.keywords.empty(), "damaged keyword record ignored");
        // no sample rate in the axis
        auto n = handFile(false, "CF", 1000, 0.0, 0, 16, 16);
        spit(P("norate.tmp"), n);
        CHECK(blue::probe(P("norate.tmp"), i, err) && i.sampleRate == 0, "no rate is reported as 0");
        // a frequency axis (spectrum) is not a sample rate
        auto fq = handFile(false, "CF", 1000, 100.0, 3, 16, 16);
        spit(P("freqaxis.tmp"), fq);
        CHECK(blue::probe(P("freqaxis.tmp"), i, err) && i.sampleRate == 0, "frequency axis is not a rate");
    }
#ifndef _WIN32
    // a file larger than 4 GiB is described by its header alone (sparse file, no samples written)
    {
        auto b = handFile(false, "CF", 1000, 1e-6, 1, 0, 8.0 * 600000000);   // 4.8 GB declared
        b.resize(512);
        spit(P("big.tmp"), b);
        std::error_code ec;
        fs::resize_file(P("big.tmp"), 512 + 8ull * 600000000, ec);
        if (!ec) {
            blue::Info i;
            CHECK(blue::probe(P("big.tmp"), i, err) && i.samples == 600000000, "4.8 GB file: sample count above 32 bits");
        }
    }
#endif
    // the recorder-style raw bytes path
    {
        blue::WriteSpec s; s.kind = 'F'; s.sampleRate = 1e6;
        blue::Writer w; std::string e;
        const auto x = signal(100);
        CHECK(w.open(P("raw.tmp"), s, e) && w.writeBytes(x.data(), 100 * sizeof(cf32), e) && w.close(e), "writeBytes");
        blue::Info i; CHECK(blue::probe(P("raw.tmp"), i, e) && i.samples == 100, "writeBytes count");
    }
    CHECK(!blue::Writer().open(P("norate_w.tmp"), blue::WriteSpec(), err), "writer needs a sample rate");

#ifndef BLUE_ONLY
    // the file-source probe: complex float / int16 / int8 play straight from the file, real data is copied to cf32, BLUE under any name
    {
        IqFileInfo f; std::string e;
        CHECK(probeIqFile(P("rt_CF_le.tmp"), f, e) && f.blue && f.format == FileFormat::CF32 && f.offsetBytes == 512 && f.endBytes == 512 + 5000 * 8 && !f.bigEndian, "probe CF le");
        CHECK(std::fabs(f.sampleRate - 2.4e6) < 1e-3 && std::fabs(f.centerHz - 433.92e6) < 1, "probe rate and centre");
        CHECK(probeIqFile(P("rt_CI_be.tmp"), f, e) && f.format == FileFormat::CI16 && f.bigEndian, "probe CI big-endian");
        CHECK(probeIqFile(P("rt_CB_le.tmp"), f, e) && f.format == FileFormat::CS8, "probe CB");
        CHECK(probeIqFile(P("rt_SF_le.tmp"), f, e) && f.converted && f.format == FileFormat::CF32 && f.offsetBytes == 0, "probe real: converted copy");
        const auto cv = slurp(f.dataPath);
        CHECK(cv.size() == 5000 * 8, "converted copy size");
        float v[2]; memcpy(v, cv.data() + 8 * 100, 8);
        CHECK(std::fabs(v[0] - signal(101)[100].real()) < 1e-6 && v[1] == 0.f, "converted real sample has Q = 0");
#ifndef _WIN32
        {   // the converted copy lives in a private per-user folder (0700) and is 0600, not loose in the shared temp folder
            namespace fs2 = std::filesystem;
            const fs2::path cp = f.dataPath;
            CHECK(cp.parent_path() != fs2::temp_directory_path() && cp.parent_path().filename().string().rfind("onair-", 0) == 0, "converted copy is in a per-user folder");
            std::error_code ec2;
            const auto dp = fs2::status(cp.parent_path(), ec2).permissions();
            const auto fp = fs2::status(cp, ec2).permissions();
            CHECK((dp & (fs2::perms::group_all | fs2::perms::others_all)) == fs2::perms::none, "folder is private (0700)");
            CHECK((fp & (fs2::perms::group_all | fs2::perms::others_all)) == fs2::perms::none, "copy is private (0600)");
        }
#endif
        CHECK(probeIqFile(P("rt_CD_be.tmp"), f, e) && f.converted, "probe complex double: converted");
        CHECK(probeIqFile(P("rt_CF_le.tmp"), f, e, false) && !f.converted, "listing probe does not convert");
        CHECK(probeIqFile(P("rt_SL_le.tmp"), f, e, false) && f.converted && f.dataPath == P("rt_SL_le.tmp"), "listing probe of a real file leaves the path");
        fs::copy_file(P("rt_CF_le.tmp"), P("noext_signal"), fs::copy_options::overwrite_existing);
        CHECK(probeIqFile(P("noext_signal"), f, e) && f.blue, "BLUE tag found under another name");
        CHECK(!probeIqFile(P("bad_magic.tmp"), f, e) && !e.empty(), "a .tmp that is not BLUE says so");
        CHECK(!probeIqFile(P("norate.tmp"), f, e) && !e.empty(), "no sample rate is an error for playback");
        // the recorder writes a BLUE file for a .tmp name: rate and centre in the header, float and 8-bit
        for (FileFormat ff : {FileFormat::CF32, FileFormat::CS8}) {
            const std::string rp = P(ff == FileFormat::CF32 ? "rec_f.tmp" : "rec_b.tmp");
            IqRecorder rec;
            rec.setSigmf(true, 100.5e6, "fm");
            CHECK(rec.start(rp, ff, 1.0e6, e), "recorder starts a BLUE file");
            const auto x = signal(4096);
            rec.push(x.data(), 4096); rec.push(x.data(), 4096);
            rec.stop();
            blue::Info bi;
            CHECK(blue::probe(rp, bi, e) && bi.samples == 8192 && bi.sampleRate == 1.0e6 && std::fabs(bi.centerHz - 100.5e6) < 1 && bi.description == "fm", "recorded BLUE header");
            CHECK(!fs::exists(P(ff == FileFormat::CF32 ? "rec_f.sigmf-meta" : "rec_b.sigmf-meta")), "no SigMF files beside a BLUE recording");
            blue::Reader br; cf32 y;
            CHECK(br.open(rp, e) && br.read(&y, 1) == 1 && std::fabs(y.real() - x[0].real()) < 0.012, "recorded sample reads back");
        }
        CHECK(blueRecordingName("onair_fm_1.000MHz_2Msps_x.cf32") == "onair_fm_1.000MHz_2Msps_x.tmp", "BLUE recording name");
        // transmit to an IQ file in the BLUE formats
        for (IqFileFormat tf : {IqFileFormat::BlueCf32, IqFileFormat::BlueCi16, IqFileFormat::BlueCs8}) {
            IqFileTxConfig c; c.format = tf; c.centerHz = 50e6;
            c.path = iqFilePath(dir.string(), "txout", tf);
            CHECK(c.path.size() > 4 && c.path.substr(c.path.size() - 4) == ".tmp", "BLUE TX file extension");
            auto sink = makeIqFileTxSink(c);
            TxTune tt; tt.sampleRate = 250000; tt.centerHz = 50e6;
            const auto x = signal(1000);
            CHECK(sink->open(tt, e) && sink->write(x.data(), 1000, e), "TX BLUE open and write");
            sink->close();
            blue::Info bi;
            CHECK(blue::probe(c.path, bi, e) && bi.samples == 1000 && bi.sampleRate == 250000 && bi.centerHz == 50e6, "TX BLUE header");
            fs::remove(c.path);
        }
        const auto list = listRecordingFiles(dir.string());
        int blues = 0;
        for (auto& r : list) if (r.blue) { blues++; if (r.fileName == "rt_CF_le.tmp") CHECK(std::fabs(r.seconds - 5000 / 2.4e6) < 1e-9, "listing length"); }
        CHECK(blues >= 20, "listing shows the BLUE files");
        for (auto& r : list) CHECK(r.fileName != "bad_magic.tmp" && r.fileName != "short.tmp", "damaged .tmp not listed");
    }
#endif

    std::error_code ec;
    fs::remove_all(dir, ec);
    printf(fails ? "bluefile: %d FAILED\n" : "bluefile: all passed\n", fails);
    return fails ? 1 : 0;
}
