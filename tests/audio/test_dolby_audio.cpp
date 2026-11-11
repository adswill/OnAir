// Dolby broadcast audio (core/src/audio/dolby_audio.cpp): detection of Dolby Atmos in E-AC-3 (JOC) and AC-4, the channel bed, the output
// layout choice and the IEC 61937 packer used for bitstream passthrough.
//   test_dolby_audio
// What is checked, and against what:
//   - E-AC-3 / AC-3 headers built bit by bit from the syntax of ETSI TS 102 366 annex E, every combination of acmod, LFE, block count,
//     stream type, mixing / informational metadata and the Atmos flag of TS 103 420 clause 8.3, with random values in the skipped fields
//   - real frames from FFmpeg's own AC-3 / E-AC-3 encoders (when the FFmpeg has them): the parser reads what the encoder wrote; an Atmos
//     flag inserted into a real frame still decodes to the same sound with FFmpeg's decoder (so the flag sits where the decoder skips it),
//     and FFmpeg 7.1+ reports the Atmos profile for it (an independent reader of the same bits)
//   - the bed of a 5.1 + dependent substream stream (7.1), fed in random packet splits with garbage between frames
//   - AC-4 DSI (TS 103 190-2 E.6 / E.10) built bit by bit: channel modes, objects, program id / uuid, long presentations, every truncation
//   - the DVB AC-4 descriptor and a PMT through the receiver's own PMT parser (descriptors 0x7F/0x15 and 0x7A, ATSC stream type 0x87)
//   - IEC 61937 bursts: preamble, data type, length units, repetition period, byte order, against FFmpeg's spdif muxer byte for byte
// No real Atmos broadcast recording was available: the Atmos streams here are simulated (headers built from the standard).
#include "dect2/audio/dolby_audio.h"
#include "dect2/tv/ts_proto.h"
#include "../../core/src/audio/bitstream_out.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

using namespace dect2;

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct BW {
    std::vector<uint8_t> b; size_t pos = 0;
    void put(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; i--) {
            if (pos / 8 >= b.size()) b.push_back(0);
            if ((v >> i) & 1) b[pos / 8] |= (uint8_t)(0x80 >> (pos % 8));
            pos++;
        }
    }
    void align() { while (pos % 8) put(0, 1); }
};

static const int kBlk[4] = {1, 2, 3, 6};
static std::mt19937 rng(20261111);
static uint32_t rnd(int bits) { return bits >= 32 ? (uint32_t)rng() : (uint32_t)rng() & ((1u << bits) - 1); }

struct Eac3Spec {
    int strmtyp = 0, subid = 0, fscod = 0, numblkscod = 3, acmod = 7, lfe = 1, chanmap = -1;
    bool mixmdate = false, infomdate = false, joc = false;
    int complexity = 0, bytes = 512, bsmod = 0;
};

static std::vector<uint8_t> genEac3(const Eac3Spec& e) {
    BW w;
    w.put(0x0B77, 16);
    w.put((uint32_t)e.strmtyp, 2); w.put((uint32_t)e.subid, 3); w.put((uint32_t)(e.bytes / 2 - 1), 11);
    w.put((uint32_t)e.fscod, 2);
    if (e.fscod == 3) w.put(0, 2); else w.put((uint32_t)e.numblkscod, 2);   // fscod2 48 kHz -> 24 kHz
    const int blocks = e.fscod == 3 ? 6 : kBlk[e.numblkscod];
    w.put((uint32_t)e.acmod, 3); w.put((uint32_t)e.lfe, 1); w.put(16, 5);
    for (int i = 0; i < (e.acmod ? 1 : 2); i++) { w.put(rnd(5), 5); const bool c = rnd(1); w.put(c, 1); if (c) w.put(rnd(8), 8); }
    if (e.strmtyp == 1) { w.put(e.chanmap >= 0, 1); if (e.chanmap >= 0) w.put((uint32_t)e.chanmap, 16); }
    w.put(e.mixmdate, 1);
    if (e.mixmdate) {
        if (e.acmod > 2) w.put(rnd(2), 2);
        if ((e.acmod & 1) && e.acmod > 2) w.put(rnd(6), 6);
        if (e.acmod & 4) w.put(rnd(6), 6);
        if (e.lfe) { const bool l = rnd(1); w.put(l, 1); if (l) w.put(rnd(5), 5); }
        if (e.strmtyp == 0) {
            for (int i = 0; i < (e.acmod ? 1 : 2); i++) { const bool p = rnd(1); w.put(p, 1); if (p) w.put(rnd(6), 6); }
            { const bool x = rnd(1); w.put(x, 1); if (x) w.put(rnd(6), 6); }
            const int mixdef = (int)rnd(2);
            w.put((uint32_t)mixdef, 2);
            if (mixdef == 1) w.put(rnd(5), 5);
            else if (mixdef == 2) w.put(rnd(12), 12);
            else if (mixdef == 3) { const int len = (int)rnd(3); w.put((uint32_t)len, 5); for (int k = 0; k < (len + 2) * 8; k++) w.put(rnd(1), 1); }
            if (e.acmod < 2) for (int i = 0; i < (e.acmod ? 1 : 2); i++) { const bool p = rnd(1); w.put(p, 1); if (p) w.put(rnd(14), 14); }
            const bool fm = rnd(1); w.put(fm, 1);
            if (fm) for (int b = 0; b < blocks; b++) { if (blocks == 1) w.put(rnd(5), 5); else { const bool x = rnd(1); w.put(x, 1); if (x) w.put(rnd(5), 5); } }
        }
    }
    w.put(e.infomdate, 1);
    if (e.infomdate) {
        w.put((uint32_t)e.bsmod, 3); w.put(rnd(2), 2);
        if (e.acmod == 2) w.put(rnd(4), 4);
        if (e.acmod >= 6) w.put(rnd(2), 2);
        for (int i = 0; i < (e.acmod ? 1 : 2); i++) { const bool a = rnd(1); w.put(a, 1); if (a) w.put(rnd(8), 8); }
        if (e.fscod < 3) w.put(rnd(1), 1);
    }
    if (e.strmtyp == 0 && e.fscod != 3 && e.numblkscod != 3) w.put(rnd(1), 1);
    if (e.strmtyp == 2) { if (e.numblkscod == 3 || e.fscod == 3) w.put(rnd(6), 6); else { const bool x = rnd(1); w.put(x, 1); if (x) w.put(rnd(6), 6); } }
    if (e.joc) { w.put(1, 1); w.put(1, 6); w.put(rnd(7) << 1 | 1u, 8); w.put((uint32_t)e.complexity, 8); }   // reserved bits random
    else { const bool other = rnd(1); w.put(other, 1); if (other) { w.put(0, 6); w.put(rnd(7) << 1, 8); } }   // addbsi without the flag
    while (w.b.size() < (size_t)e.bytes) w.b.push_back((uint8_t)rnd(8));
    w.b.resize((size_t)e.bytes);
    return w.b;
}

static std::vector<uint8_t> genAc3(int fscod, int frmsizecod, int acmod, int lfe, int bsmod) {
    BW w;
    w.put(0x0B77, 16); w.put(rnd(16), 16); w.put((uint32_t)fscod, 2); w.put((uint32_t)frmsizecod, 6);
    w.put(8, 5); w.put((uint32_t)bsmod, 3); w.put((uint32_t)acmod, 3);
    if ((acmod & 1) && acmod != 1) w.put(rnd(2), 2);
    if (acmod & 4) w.put(rnd(2), 2);
    if (acmod == 2) w.put(rnd(2), 2);
    w.put((uint32_t)lfe, 1);
    while (w.b.size() < 4000) w.b.push_back((uint8_t)rnd(8));
    return w.b;
}

static void testHeaders() {
    int n = 0;
    for (int strmtyp = 0; strmtyp < 3; strmtyp++)
        for (int acmod = 0; acmod < 8; acmod++)
            for (int lfe = 0; lfe < 2; lfe++)
                for (int nb = 0; nb < 5; nb++)   // 4 = the reduced-rate fscod 3
                    for (int md = 0; md < 4; md++)
                        for (int joc = 0; joc < 2; joc++) {
                            Eac3Spec e;
                            e.strmtyp = strmtyp; e.acmod = acmod; e.lfe = lfe; e.numblkscod = nb < 4 ? nb : 3; e.fscod = nb < 4 ? 0 : 3;
                            e.mixmdate = md & 1; e.infomdate = (md & 2) != 0; e.joc = joc; e.complexity = joc ? 1 + (int)rnd(4) % 16 : 0;
                            e.chanmap = strmtyp == 1 ? (int)rnd(16) : -1; e.bytes = 2 * (64 + (int)rnd(8)); e.bsmod = (int)rnd(3);
                            const std::vector<uint8_t> f = genEac3(e);
                            DolbyFrame d;
                            const bool ok = parseDolbyFrame(f.data(), f.size(), d);
                            n++;
                            CHECK(ok && d.eac3, "E-AC-3 st%d acmod%d lfe%d nb%d md%d joc%d: not parsed", strmtyp, acmod, lfe, nb, md, joc);
                            if (!ok) continue;
                            CHECK(d.acmod == acmod && d.lfe == lfe && d.frameType == strmtyp && d.frameBytes == e.bytes, "fields st%d acmod%d", strmtyp, acmod);
                            CHECK(d.blocks == (nb < 4 ? kBlk[nb] : 6) && d.sampleRate == (nb < 4 ? 48000 : 24000), "blocks/rate nb%d", nb);
                            CHECK(d.joc == (bool)joc && d.complexity == e.complexity, "JOC st%d acmod%d lfe%d nb%d md%d: joc %d cplx %d want %d", strmtyp, acmod, lfe, nb, md, d.joc, d.complexity, e.complexity);
                            CHECK(d.chanmap == e.chanmap, "chanmap");
                            if (e.infomdate) CHECK(d.bsmod == e.bsmod, "bsmod");
                        }
    printf("E-AC-3 headers: %d combinations\n", n);
    // AC-3: frame sizes of A/52 table 5.18 at the three rates, every frmsizecod; acmod / lfe / bsmod
    const int words48[19] = {64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024, 1152, 1280};
    for (int code = 0; code < 38; code++)
        for (int fs = 0; fs < 3; fs++) {
            const int acmod = code % 8, lfe = code & 1, bsmod = code % 7;
            const std::vector<uint8_t> f = genAc3(fs, code, acmod, lfe, bsmod);
            DolbyFrame d;
            CHECK(parseDolbyFrame(f.data(), f.size(), d) && !d.eac3, "AC-3 code %d fs %d", code, fs);
            const int w = fs == 0 ? words48[code >> 1] : fs == 2 ? words48[code >> 1] * 3 / 2 : 0;
            if (fs != 1) CHECK(d.frameBytes == 2 * w, "AC-3 size code %d fs %d: %d want %d", code, fs, d.frameBytes, 2 * w);
            CHECK(d.acmod == acmod && d.lfe == lfe && d.bsmod == bsmod, "AC-3 fields");
        }
    DolbyFrame d;
    std::vector<uint8_t> f = genAc3(1, 0, 2, 0, 0);
    CHECK(parseDolbyFrame(f.data(), f.size(), d) && d.frameBytes == 138, "44.1 kHz 32 kbps = 69 words: %d", d.frameBytes);
    f = genAc3(1, 1, 2, 0, 0);
    CHECK(parseDolbyFrame(f.data(), f.size(), d) && d.frameBytes == 140, "44.1 kHz odd code = 70 words");
    f = genAc3(1, 37, 2, 0, 0);
    CHECK(parseDolbyFrame(f.data(), f.size(), d) && d.frameBytes == 2 * 1394, "44.1 kHz 640 kbps = 1394 words: %d", d.frameBytes);
    // not Dolby / damaged
    const uint8_t junk[16] = {0x0B, 0x77, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    CHECK(!parseDolbyFrame(junk, sizeof junk, d), "fscod 3 + fscod2 3 rejected");
    CHECK(!parseDolbyFrame(f.data(), 5, d), "short buffer rejected");
}

static void testBeds() {
    CHECK(dolbyBedName(dolbyAcmodSpeakers(7, 1)) == "5.1", "5.1");
    CHECK(dolbyBedName(dolbyAcmodSpeakers(2, 0)) == "2.0", "2.0");
    CHECK(dolbyBedName(dolbyAcmodSpeakers(1, 0)) == "1.0", "1.0");
    CHECK(dolbyBedName(dolbyAcmodSpeakers(7, 1) | dolbyChanmapSpeakers(1 << (15 - 6))) == "7.1", "5.1 + Lrs/Rrs = 7.1");
    CHECK(dolbyBedName(dolbyAcmodSpeakers(7, 1) | dolbyChanmapSpeakers((1 << (15 - 6)) | (1 << (15 - 11)))) == "7.1.2", "7.1 + Vhl/Vhr = 7.1.2");
    // a stream: independent 5.1 with the Atmos flag + dependent substream with the back pair, sent in random splits with garbage
    for (int joc = 0; joc < 2; joc++)
        for (int trial = 0; trial < 20; trial++) {
            std::vector<uint8_t> st;
            for (int k = 0; k < 40; k++) {
                Eac3Spec i; i.acmod = 7; i.lfe = 1; i.joc = joc; i.complexity = 15; i.bytes = 768; i.mixmdate = k & 1; i.infomdate = (k & 2) != 0;
                Eac3Spec dd; dd.strmtyp = 1; dd.acmod = 2; dd.lfe = 0; dd.chanmap = 1 << (15 - 6); dd.bytes = 256;
                const auto a = genEac3(i), b = genEac3(dd);
                st.insert(st.end(), a.begin(), a.end());
                st.insert(st.end(), b.begin(), b.end());
                if (trial % 3 == 1 && k % 7 == 3) { const uint8_t g[5] = {0x0B, 0x77, 0x12, 0x00, 0x41}; st.insert(st.end(), g, g + 5); }   // junk with a false sync
            }
            DolbyStreamInfo si;
            for (size_t pos = 0; pos < st.size();) { const size_t n = std::min(st.size() - pos, (size_t)(1 + rnd(10) % 700)); si.feed(&st[pos], n); pos += n; }
            const DolbyStreamSummary& s = si.summary();
            CHECK(s.valid && s.eac3 && s.atmos == (bool)joc && s.bed == "7.1" && s.channels == 8, "stream joc %d trial %d: valid %d atmos %d bed %s", joc, trial, s.valid, s.atmos, s.bed.c_str());
            if (joc) CHECK(s.complexity == 15, "complexity %d", s.complexity);
            CHECK(s.frames >= 35, "frames %llu", (unsigned long long)s.frames);
            CHECK(s.bitrateKbps == (768 + 256) * 8 * 48000 / 1536 / 1000, "bit rate %d", s.bitrateKbps);
            const std::string line = dolbyFormatLine(s);
            CHECK(line == (joc ? "Dolby Atmos (E-AC-3 JOC), 7.1 bed" : "Dolby Digital Plus (E-AC-3) 7.1"), "line %s", line.c_str());
        }
}

static void testLayouts() {
    // automatic: the device decides, never more than the stream; forced layouts never invent channels
    CHECK(dolbyOutputChannels(0, 6, 2) == 2 && dolbyOutputChannels(0, 6, 0) == 2, "auto 5.1 on stereo / unknown device = stereo downmix");
    CHECK(dolbyOutputChannels(0, 6, 6) == 6 && dolbyOutputChannels(0, 6, 8) == 6, "auto 5.1 on 5.1/7.1 device");
    CHECK(dolbyOutputChannels(0, 8, 8) == 8 && dolbyOutputChannels(0, 8, 6) == 6, "auto 7.1");
    CHECK(dolbyOutputChannels(0, 2, 8) == 2, "auto stereo stream");
    CHECK(dolbyOutputChannels(1, 8, 8) == 2 && dolbyOutputChannels(2, 6, 8) == 2, "forced stereo / Lt-Rt");
    CHECK(dolbyOutputChannels(3, 8, 0) == 6 && dolbyOutputChannels(3, 2, 0) == 2, "forced 5.1");
    CHECK(dolbyOutputChannels(4, 8, 0) == 8 && dolbyOutputChannels(4, 6, 0) == 6 && dolbyOutputChannels(4, 1, 0) == 2, "forced 7.1");
    for (int l = 0; l < 5; l++) for (int s = 1; s <= 8; s++) for (int d = 0; d <= 8; d++) { const int c = dolbyOutputChannels(l, s, d); CHECK(c == 2 || c == 6 || c == 8, "only 2/6/8"); }
    CHECK(alsaIecDeviceName("", 192000) == "hdmi:AES0=0x06,AES1=0x82,AES2=0x00,AES3=0x0e" || alsaIecDeviceName("", 192000) == "", "ALSA 192 kHz: %s", alsaIecDeviceName("", 192000).c_str());
#if !defined(_WIN32) && !defined(__APPLE__)
    CHECK(alsaIecDeviceName("hdmi:CARD=1,DEV=0", 48000) == "hdmi:CARD=1,DEV=0,AES0=0x06,AES1=0x82,AES2=0x00,AES3=0x02", "ALSA with card");
    CHECK(alsaIecDeviceName("iec958:AES0=0x06", 48000) == "iec958:AES0=0x06", "ALSA user channel status kept");
#endif
}

// ---- AC-4 -------------------------------------------------------------------------------------------------------------------------------
struct PresSpec { int version = 1, config = 0, chMode = 4; bool coded = true; int pad = 3; bool longLen = false; };
static std::vector<uint8_t> genDsi(const std::vector<PresSpec>& ps, int bitstreamVersion, bool programId, bool uuid) {
    BW w;
    w.put(1, 3); w.put((uint32_t)bitstreamVersion, 7); w.put(1, 1); w.put(1, 4); w.put((uint32_t)ps.size(), 9);
    if (bitstreamVersion > 1) { w.put(programId, 1); if (programId) { w.put(0x1234, 16); w.put(uuid, 1); if (uuid) for (int i = 0; i < 16; i++) w.put(rnd(8), 8); } }
    w.put(rnd(2), 2); w.put(rnd(32), 32); w.put(rnd(32), 32);
    w.align();
    for (const PresSpec& p : ps) {
        BW q;
        q.put((uint32_t)p.config, 5);
        if (p.config != 6) {
            q.put(rnd(3), 3);
            const bool pid = rnd(1); q.put(pid, 1); if (pid) q.put(rnd(5), 5);
            q.put(rnd(2), 2); q.put(rnd(2), 2); q.put(rnd(5), 5); q.put(rnd(10), 10);
            q.put(p.coded, 1);
            if (p.coded) { q.put((uint32_t)p.chMode, 5); if (p.chMode >= 11 && p.chMode <= 14) { q.put(rnd(1), 1); q.put(rnd(2), 2); } q.put(0, 6); q.put(rnd(18), 18); }
            q.put(0, 1); q.put(0, 1);
        }
        q.align();
        for (int k = 0; k < p.pad; k++) q.put(rnd(8), 8);
        if (p.longLen) while (q.b.size() < 300) q.put(rnd(8), 8);
        w.put((uint32_t)p.version, 8);
        if (q.b.size() >= 255) { w.put(255, 8); w.put((uint32_t)(q.b.size() - 255), 16); } else w.put((uint32_t)q.b.size(), 8);
        for (uint8_t c : q.b) w.put(c, 8);
    }
    return w.b;
}

static void testAc4() {
    {   // 5.1 then 7.1.4: Atmos, bed 7.1.4
        PresSpec a; a.chMode = 4; PresSpec b; b.chMode = 12;
        const auto d = genDsi({a, b}, 2, true, true);
        const Ac4DsiInfo i = parseAc4DsiV1(d.data(), d.size());
        CHECK(i.valid && i.presentations.size() == 2 && i.immersive && i.bed == "7.1.4" && i.sampleRate == 48000, "7.1.4: valid %d n %zu imm %d bed %s", i.valid, i.presentations.size(), i.immersive, i.bed.c_str());
    }
    {   // stereo only: no Atmos
        PresSpec a; a.chMode = 1;
        const auto d = genDsi({a}, 1, false, false);
        const Ac4DsiInfo i = parseAc4DsiV1(d.data(), d.size());
        CHECK(i.valid && !i.immersive && i.bed == "stereo", "stereo: %s", i.bed.c_str());
    }
    {   // an EMDF-only presentation, a long one (pres_bytes 255 + add) with objects: Atmos "objects"
        PresSpec e; e.config = 6; PresSpec o; o.coded = false; o.longLen = true; o.config = 2;
        const auto d = genDsi({e, o}, 2, true, false);
        const Ac4DsiInfo i = parseAc4DsiV1(d.data(), d.size());
        CHECK(i.valid && i.presentations.size() == 2 && i.immersive && i.bed == "objects", "objects: n %zu imm %d bed %s", i.presentations.size(), i.immersive, i.bed.c_str());
    }
    {   // an unknown presentation version is skipped by its length; 5.1.2 (mode 10) counts as immersive (height channels)
        PresSpec x; x.version = 2; x.chMode = 15; PresSpec h; h.chMode = 10;
        const auto d = genDsi({x, h}, 2, false, false);
        const Ac4DsiInfo i = parseAc4DsiV1(d.data(), d.size());
        CHECK(i.valid && i.presentations.size() == 2 && i.immersive && i.bed == "5.1.2", "skip v2 + 5.1.2: %s", i.bed.c_str());
    }
    {   // every truncation: no crash, never claims more presentations than fit
        PresSpec a; a.chMode = 12; PresSpec b; b.coded = false;
        const auto d = genDsi({a, b}, 2, true, true);
        for (size_t n = 0; n <= d.size(); n++) { const Ac4DsiInfo i = parseAc4DsiV1(d.data(), n); CHECK(i.presentations.size() <= 2, "truncated"); }
        std::vector<uint8_t> bad = d; bad[0] = (uint8_t)((bad[0] & 0x1F) | (2 << 5));   // ac4_dsi_version 2: skipped
        CHECK(!parseAc4DsiV1(bad.data(), bad.size()).valid, "dsi version 2 is not read");
    }
    {   // DVB AC-4 descriptor: config (multichannel, dialogue enhancement) + the DSI
        PresSpec a; a.chMode = 12;
        const auto dsi = genDsi({a}, 2, false, false);
        std::vector<uint8_t> body = {0xC0, (uint8_t)(0x80 | (2 << 5)), (uint8_t)dsi.size()};
        body.insert(body.end(), dsi.begin(), dsi.end());
        const Ac4Descriptor ad = parseAc4Descriptor(body.data(), body.size());
        CHECK(ad.valid && ad.haveConfig && ad.dialogueEnhancement && ad.channelMode == 2 && ad.dsi.size() == dsi.size() && ad.dsiInfo.immersive, "AC-4 descriptor");
        const uint8_t only[2] = {0x80, 0x20};
        const Ac4Descriptor b = parseAc4Descriptor(only, 2);
        CHECK(b.valid && b.channelMode == 1 && !b.dialogueEnhancement && b.dsi.empty(), "AC-4 descriptor without TOC");
        const uint8_t cut[3] = {0x40, 0x09, 0x01};
        CHECK(!parseAc4Descriptor(cut, 3).valid, "AC-4 descriptor TOC longer than the descriptor");
    }
}

// a PMT through the receiver's own parser: DVB AC-4 (0x06 + 0x7F/0x15), DVB E-AC-3 (0x06 + 0x7A), ATSC E-AC-3 (0x87) and AC-3 (0x81)
static void testPmt() {
    std::vector<uint8_t> es;
    auto stream = [&](int type, int pid, const std::vector<uint8_t>& desc) {
        es.push_back((uint8_t)type); es.push_back((uint8_t)(0xE0 | (pid >> 8))); es.push_back((uint8_t)pid);
        es.push_back((uint8_t)(0xF0 | (desc.size() >> 8))); es.push_back((uint8_t)desc.size());
        es.insert(es.end(), desc.begin(), desc.end());
    };
    stream(0x02, 0x100, {});
    stream(0x06, 0x101, {0x7F, 0x04, 0x15, 0x80, 0x40, 0x00});
    stream(0x06, 0x102, {0x7A, 0x01, 0x00});
    stream(0x87, 0x103, {});
    stream(0x81, 0x104, {});
    std::vector<uint8_t> sec = {0x02, 0, 0, 0x00, 0x01, 0xC1, 0x00, 0x00, 0xE1, 0x00, 0xF0, 0x00};
    sec.insert(sec.end(), es.begin(), es.end());
    const int len = (int)sec.size() - 3 + 4;
    sec[1] = (uint8_t)(0xB0 | (len >> 8)); sec[2] = (uint8_t)len;
    const uint32_t crc = tsp::crc32(sec.data(), (int)sec.size());
    for (int k = 3; k >= 0; k--) sec.push_back((uint8_t)(crc >> (8 * k)));
    tsp::Pmt pmt;
    CHECK(tsp::parsePmt(sec.data(), (int)sec.size(), pmt) && pmt.streams.size() == 5, "PMT parsed: %zu streams", pmt.streams.size());
    if (pmt.streams.size() == 5) {
        CHECK(pmt.streams[1].codec == "AC-4" && pmt.streams[1].kind == "audio", "AC-4 descriptor: %s %s", pmt.streams[1].codec.c_str(), pmt.streams[1].kind.c_str());
        CHECK(pmt.streams[2].codec == "E-AC-3" && pmt.streams[2].kind == "audio", "E-AC-3 descriptor");
        CHECK(pmt.streams[3].codec == "E-AC-3" && pmt.streams[4].codec == "AC-3", "ATSC stream types");
    }
}

// ---- IEC 61937 ------------------------------------------------------------------------------------------------------------------------
static void testIecFraming() {
    // AC-3: one burst per frame, 1536 frames, Pd in bits, bsmod in Pc, the first payload word is the sync word
    std::vector<uint8_t> f = genAc3(0, 22, 7, 1, 3);   // 48 kHz, 448 kbps (code 22): 1792 bytes
    DolbyFrame d; parseDolbyFrame(f.data(), f.size(), d);
    f.resize((size_t)d.frameBytes);
    Iec61937Packer pk;
    std::vector<IecBurst> out;
    pk.push(f.data(), f.size(), out);
    CHECK(out.size() == 1 && out[0].words.size() == 1536 * 2, "AC-3 burst of 1536 frames");
    if (!out.empty()) {
        const auto& w = out[0].words;
        CHECK((uint16_t)w[0] == 0xF872 && (uint16_t)w[1] == 0x4E1F, "Pa Pb");
        CHECK((uint16_t)w[2] == (0x01 | 3 << 8), "Pc = 1 | bsmod 3 << 8: %04x", (uint16_t)w[2]);
        CHECK((uint16_t)w[3] == 1792 * 8, "Pd = length in bits: %d", (uint16_t)w[3]);
        CHECK((uint16_t)w[4] == 0x0B77, "byte order: the first payload word is the sync word 0x0B77, got %04x", (uint16_t)w[4]);
        CHECK(std::all_of(w.begin() + 4 + 896, w.end(), [](int16_t v) { return v == 0; }), "zero padding to the period");
    }
    CHECK(pk.linkRate() == 48000 && Iec61937Packer::repetitionPeriod(Iec61937Packer::Ac3) == 1536, "AC-3 link rate");
    // E-AC-3, six-block frames: one burst each, 6144 frames, Pd in bytes, data type 0x15, the link at 192 kHz
    Iec61937Packer pe;
    out.clear();
    for (int k = 0; k < 4; k++) {
        Eac3Spec e; e.bytes = 1536; e.joc = k & 1; e.complexity = 10;
        const auto fr = genEac3(e);
        pe.push(fr.data(), fr.size(), out);
    }
    pe.flush(out);
    CHECK(out.size() == 4 && pe.linkRate() == 192000, "E-AC-3 four bursts at 192 kHz: %zu", out.size());
    for (const IecBurst& b : out) CHECK(b.words.size() == 6144 * 2 && (uint16_t)b.words[2] == 0x15 && (uint16_t)b.words[3] == 1536 && (uint16_t)b.words[4] == 0x0B77, "E-AC-3 burst");
    // E-AC-3 with one-block frames: six frames (and their dependent substreams) per burst
    Iec61937Packer p1;
    out.clear();
    for (int k = 0; k < 12; k++) {
        Eac3Spec e; e.numblkscod = 0; e.bytes = 200;
        Eac3Spec dd; dd.strmtyp = 1; dd.acmod = 2; dd.lfe = 0; dd.chanmap = 1 << 9; dd.bytes = 100; dd.numblkscod = 0;
        const auto a = genEac3(e), b = genEac3(dd);
        p1.push(a.data(), a.size(), out);
        p1.push(b.data(), b.size(), out);
    }
    p1.flush(out);
    CHECK(out.size() == 2 && !out.empty() && (uint16_t)out[0].words[3] == 6 * 300, "1-block frames: 2 bursts of 6 x (200 + 100) bytes: %zu", out.size());
    // an odd-length payload: the last byte in the high half of the last word
    IecBurst b;
    const uint8_t odd[3] = {0x0B, 0x77, 0xAB};
    CHECK(Iec61937Packer::pack(odd, 3, 0x15, 3, 6144, b) && (uint16_t)b.words[5] == 0xAB00, "odd byte");
    std::vector<uint8_t> big(6144 * 4, 0x55);
    CHECK(!Iec61937Packer::pack(big.data(), big.size(), 0x15, (int)big.size(), 6144, b), "payload larger than the period is refused");
}

// ---- real frames from FFmpeg's encoders, the Atmos flag inserted, FFmpeg's decoder and spdif muxer as independent references ---------
static std::vector<std::vector<uint8_t>> encode(AVCodecID id, int bitrate) {
    std::vector<std::vector<uint8_t>> pk;
    const AVCodec* c = avcodec_find_encoder(id);
    if (!c) return pk;
    AVCodecContext* x = avcodec_alloc_context3(c);
    x->sample_rate = 48000; x->bit_rate = bitrate; x->sample_fmt = AV_SAMPLE_FMT_FLTP;
    AVChannelLayout l = AV_CHANNEL_LAYOUT_5POINT1;
    av_channel_layout_copy(&x->ch_layout, &l);
    if (avcodec_open2(x, c, nullptr) < 0) { avcodec_free_context(&x); return pk; }
    AVFrame* f = av_frame_alloc();
    f->nb_samples = x->frame_size; f->format = x->sample_fmt; av_channel_layout_copy(&f->ch_layout, &x->ch_layout);
    av_frame_get_buffer(f, 0);
    AVPacket* p = av_packet_alloc();
    for (int k = 0; k < 12; k++) {
        av_frame_make_writable(f);
        for (int ch = 0; ch < 6; ch++) { float* s = (float*)f->data[ch]; for (int i = 0; i < f->nb_samples; i++) s[i] = 0.3f * (float)sin(0.01 * (ch + 1) * (i + k * f->nb_samples)) + 0.01f * (float)((int)rnd(8) - 128) / 128.f; }
        f->pts = (int64_t)k * f->nb_samples;
        avcodec_send_frame(x, f);
        while (avcodec_receive_packet(x, p) >= 0) { pk.emplace_back(p->data, p->data + p->size); av_packet_unref(p); }
    }
    avcodec_send_frame(x, nullptr);
    while (avcodec_receive_packet(x, p) >= 0) { pk.emplace_back(p->data, p->data + p->size); av_packet_unref(p); }
    av_packet_free(&p); av_frame_free(&f); avcodec_free_context(&x);
    return pk;
}

// The frame with addbsie = 1, addbsil = 1, reserved 0, flag 1, complexity: 23 bits where the encoder wrote the one 0 bit, then the rest
// of the frame, padded to whole 16-bit words (frmsiz grows by two words; the CRC at the end is not valid any more)
static std::vector<uint8_t> withJoc(const std::vector<uint8_t>& fr, int complexity) {
    DolbyFrame d;
    if (!parseDolbyFrame(fr.data(), fr.size(), d) || d.addbsiBit < 0) return {};
    auto bit = [&](size_t i) { return (fr[i >> 3] >> (7 - (i & 7))) & 1; };
    BW w;
    for (int i = 0; i < d.addbsiBit; i++) w.put((uint32_t)bit((size_t)i), 1);
    if (bit((size_t)d.addbsiBit)) return {};   // the encoder already wrote addbsi
    w.put(1, 1); w.put(1, 6); w.put(1, 8); w.put((uint32_t)complexity, 8);
    for (size_t i = (size_t)d.addbsiBit + 1; i < fr.size() * 8 - 16; i++) w.put((uint32_t)bit(i), 1);
    while (w.pos % 16) w.put(0, 1);
    w.put(0, 16);
    std::vector<uint8_t> o = w.b;
    const int words = (int)o.size() / 2;
    o[2] = (uint8_t)((o[2] & 0xF8) | ((words - 1) >> 8)); o[3] = (uint8_t)(words - 1);   // frmsiz
    return o;
}

static bool decodeAll(AVCodecID id, const std::vector<std::vector<uint8_t>>& pk, std::vector<float>& pcm, int& channels, int& profile) {
    const AVCodec* c = avcodec_find_decoder(id);
    if (!c) return false;
    AVCodecContext* x = avcodec_alloc_context3(c);
    x->err_recognition = 0;   // the inserted flag invalidates the frame CRC: decode regardless (the bits read are the point)
    av_opt_set_int(x, "err_detect", 0, AV_OPT_SEARCH_CHILDREN);
    if (avcodec_open2(x, c, nullptr) < 0) { avcodec_free_context(&x); return false; }
    AVPacket* p = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    for (const auto& b : pk) {
        av_new_packet(p, (int)b.size()); memcpy(p->data, b.data(), b.size());
        if (avcodec_send_packet(x, p) >= 0)
            while (avcodec_receive_frame(x, f) >= 0) {
                channels = f->ch_layout.nb_channels;
                for (int i = 0; i < f->nb_samples; i++) for (int ch = 0; ch < channels; ch++) pcm.push_back(((float*)f->extended_data[ch])[i]);
                av_frame_unref(f);
            }
        av_packet_unref(p);
    }
    profile = x->profile;
    av_frame_free(&f); av_packet_free(&p); avcodec_free_context(&x);
    return true;
}

static std::vector<uint8_t> ffmpegSpdif(AVCodecID id, const std::vector<std::vector<uint8_t>>& pk) {
    std::vector<uint8_t> r;
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "spdif", nullptr) < 0 || !oc) return r;
    AVStream* st = avformat_new_stream(oc, nullptr);
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO; st->codecpar->codec_id = id; st->codecpar->sample_rate = 48000;
    AVChannelLayout l = AV_CHANNEL_LAYOUT_5POINT1; av_channel_layout_copy(&st->codecpar->ch_layout, &l);
    if (avio_open_dyn_buf(&oc->pb) < 0 || avformat_write_header(oc, nullptr) < 0) { avformat_free_context(oc); return r; }
    AVPacket* p = av_packet_alloc();
    int64_t pts = 0;
    for (const auto& b : pk) { av_new_packet(p, (int)b.size()); memcpy(p->data, b.data(), b.size()); p->stream_index = 0; p->pts = p->dts = pts; pts += 1536; av_write_frame(oc, p); av_packet_unref(p); }
    av_write_trailer(oc);
    uint8_t* buf = nullptr;
    const int n = avio_close_dyn_buf(oc->pb, &buf);
    oc->pb = nullptr;
    if (n > 0) r.assign(buf, buf + n);
    av_free(buf);
    av_packet_free(&p);
    avformat_free_context(oc);
    return r;
}

static void testReal() {
    for (AVCodecID id : {AV_CODEC_ID_AC3, AV_CODEC_ID_EAC3}) {
        const bool e = id == AV_CODEC_ID_EAC3;
        const auto pk = encode(id, e ? 384000 : 448000);
        if (pk.empty()) { printf("note: this FFmpeg has no %s encoder: real-frame checks skipped\n", e ? "E-AC-3" : "AC-3"); continue; }
        DolbyStreamInfo si;
        for (const auto& b : pk) {
            DolbyFrame d;
            CHECK(parseDolbyFrame(b.data(), b.size(), d) && d.eac3 == e && d.acmod == 7 && d.lfe == 1 && d.frameBytes == (int)b.size() && d.sampleRate == 48000 && !d.joc,
                  "%s encoder frame: eac3 %d acmod %d lfe %d bytes %d/%zu", e ? "E-AC-3" : "AC-3", d.eac3, d.acmod, d.lfe, d.frameBytes, b.size());
            si.feed(b.data(), b.size());
        }
        CHECK(si.summary().bed == "5.1" && !si.summary().atmos, "real stream bed %s", si.summary().bed.c_str());
        // IEC 61937 against FFmpeg's spdif muxer, byte for byte
        Iec61937Packer pkr;
        std::vector<IecBurst> bursts;
        for (const auto& b : pk) pkr.push(b.data(), b.size(), bursts);
        pkr.flush(bursts);
        std::vector<uint8_t> mine;
        for (const IecBurst& bu : bursts) for (int16_t w : bu.words) { mine.push_back((uint8_t)((uint16_t)w & 0xff)); mine.push_back((uint8_t)((uint16_t)w >> 8)); }
        const std::vector<uint8_t> ref = ffmpegSpdif(id, pk);
        if (ref.empty()) printf("note: this FFmpeg has no spdif muxer: IEC 61937 cross-check skipped\n");
        else CHECK(ref == mine, "%s IEC 61937: %zu bytes vs FFmpeg spdif %zu bytes, first difference at %zu", e ? "E-AC-3" : "AC-3", mine.size(), ref.size(),
                   (size_t)(std::mismatch(mine.begin(), mine.begin() + (std::ptrdiff_t)std::min(mine.size(), ref.size()), ref.begin()).first - mine.begin()));
        if (!e) continue;
        // the Atmos flag inserted into real frames: parsed, and FFmpeg's decoder gives the same sound (and the Atmos profile on 7.1+)
        std::vector<std::vector<uint8_t>> joc;
        for (const auto& b : pk) joc.push_back(withJoc(b, 11));
        bool allOk = true;
        for (const auto& b : joc) { DolbyFrame d; allOk = allOk && !b.empty() && parseDolbyFrame(b.data(), b.size(), d) && d.joc && d.complexity == 11 && d.acmod == 7; }
        CHECK(allOk, "JOC flag inserted into encoder frames and read back");
        std::vector<float> a, b; int ca = 0, cb = 0, pa = 0, pb = 0;
        if (decodeAll(id, pk, a, ca, pa) && decodeAll(id, joc, b, cb, pb)) {
            CHECK(ca == 6 && cb == 6 && a.size() == b.size() && !a.empty(), "decoded both: %d/%d ch, %zu/%zu samples", ca, cb, a.size(), b.size());
            double diff = 0; for (size_t i = 0; i < std::min(a.size(), b.size()); i++) diff = std::max(diff, (double)std::fabs(a[i] - b[i]));
            CHECK(diff == 0.0, "the flag changes nothing in the sound FFmpeg decodes: max diff %g", diff);
#ifdef AV_PROFILE_EAC3_DDP_ATMOS
            CHECK(pb == AV_PROFILE_EAC3_DDP_ATMOS && pa != AV_PROFILE_EAC3_DDP_ATMOS, "FFmpeg reports Dolby Atmos for the flagged stream only: %d / %d", pa, pb);
#else
            printf("note: this FFmpeg is older than 7.1 (no Atmos profile): the decode check stands alone\n");
#endif
        }
    }
}

int main() {
    testHeaders();
    testBeds();
    testLayouts();
    testAc4();
    testPmt();
    testIecFraming();
    testReal();
    printf("%d checks, %d failed\n", checks, fails);
    if (fails) return 1;
    printf("dolby_audio: OK (Atmos streams simulated from the standards; passthrough not tested on hardware)\n");
    return 0;
}
