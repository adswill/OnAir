// See xband_gen.h. Formats from SatDump (GPL-3.0, Copyright Aang23 and contributors); code written for OnAir.
#include "dect2/xband/xband_gen.h"
#include "dect2/xband/xband_instr.h"
#include "dect2/xband/xband_proto.h"
#include "dect2/common/mathconst.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/rrc.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/hrpt/hrpt_tel.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

namespace dect2 {

namespace {
using math::kPi;
constexpr int kSpan = 8, kTabRes = 256;
}

uint16_t xbTestPixel(int ch, int x, int y) {
    // two waves over a ramp, different per channel: a wrong channel or an off-by-one line shows
    const int v = (x * 3 + y * (ch + 2) * 5 + (ch * 311) % 997 + ((x / 16 + y / 8) & 1 ? 200 : 0) + (((x * x * (ch + 3) + y * y * 7) >> 4) & 63)) & 4095;
    return (uint16_t)v;
}

namespace xband { uint16_t xbTestPixelXb(int ch, int x, int y) { return xbTestPixel(ch, x, y); } }

namespace {

// A test stream of packets for an instrument
struct Source {
    const int P;
    int nextLine = 0;
    // VIIRS
    int seg = 0, ch = 0, step = -1, viirsSeq[64] = {};
    Source(int p) : P(p) {}
    void viirsGroup(xband::Mux& m) {
        // one scan of all channels (a shorter pass of the channels at test time: every channel, full width)
        uint8_t t[8] = {0x24, 0x00, 0x01, 0x02, (uint8_t)(seg >> 8), (uint8_t)seg, 0, 0};
        static const int diffOf[] = {};
        (void)diffOf;
        const uint32_t pat = 0xC0FFEE55;
        std::vector<std::vector<std::vector<uint16_t>>> rawOf((size_t)xband::viirsCount());
        for (int c = 0; c < xband::viirsCount(); c++) {
            int a, zh, ref, sc, dec; const int* zw, *os; xband::viirsChannelInfo(c, a, zh, zw, os, ref, sc, dec);
            std::vector<std::vector<uint16_t>> finalV((size_t)zh * 6);
            for (int l = 0; l < zh; l++) {
                int xo = 0;
                for (int z = 0; z < 6; z++) {
                    auto& v = finalV[(size_t)l * 6 + z];
                    v.resize((size_t)zw[z]);
                    for (int x = 0; x < zw[z]; x++) {
                        const int y = seg * zh + (zh - 1 - l);
                        v[(size_t)x] = (uint16_t)std::min<int>((xbTestPixel(c, xo + x, y) * 2) & 0x3FFF, 0x3FFF);
                    }
                    xo += zw[z];
                }
            }
            rawOf[(size_t)c] = std::move(finalV);
        }
        // the differences on the air: difference = value - reference + 16383
        const auto idxOf = [](int apid) { for (int c = 0; c < xband::viirsCount(); c++) { int a, zh, r, sc, d; const int* zw, *os; xband::viirsChannelInfo(c, a, zh, zw, os, r, sc, d); if (a == apid) return c; } return -1; };
        for (int c = 0; c < xband::viirsCount(); c++) {
            int a, zh, ref, sc, dec; const int* zw, *os; xband::viirsChannelInfo(c, a, zh, zw, os, ref, sc, dec);
            std::vector<std::vector<uint16_t>> sent = rawOf[(size_t)c];
            if (ref >= 0) {
                const int r = idxOf(ref);
                for (int l = 0; l < zh; l++) for (int z = 0; z < 6; z++) {
                    auto& v = sent[(size_t)l * 6 + z];
                    const auto& rv = rawOf[(size_t)r][(size_t)(l / dec) * 6 + z];
                    for (int x = 0; x < zw[z]; x++) v[(size_t)x] = (uint16_t)(v[(size_t)x] - rv[(size_t)x / dec] + 16383);
                }
            }
            int n = 0;
            for (int l = 0; l < zh; l++) if (ref >= -1) n++;
            m.add(a, 1, viirsSeq[c & 63]++ & 0x3FFF, xband::viirsBuildSegmentHeader(t, n));
            for (int l = 0; l < zh; l++) {
                std::vector<std::vector<uint16_t>> zones(6);
                for (int z = 0; z < 6; z++) {
                    zones[(size_t)z].reserve((size_t)zw[z] * os[z]);
                    for (int x = 0; x < zw[z]; x++) for (int o = 0; o < os[z]; o++) zones[(size_t)z].push_back(sent[(size_t)l * 6 + z][(size_t)x]);
                }
                m.add(a, l == zh - 1 ? 2 : 0, viirsSeq[c & 63]++ & 0x3FFF, xband::viirsBuildBody(l, pat, zones));
            }
        }
        seg++;
    }
    static uint16_t modisPix(int c, int x, int y) { return xbTestPixel(c, x, y); }
    int modisScan = 0;
    void modisScanOut(xband::Mux& m) {
        const int line = nextLine;
        for (int pos = 1; pos <= 1354; pos += 1) {
            for (int part = 0; part < 2; part++) {
                std::vector<uint8_t> pk;
                xband::modisDayPacket(modisScan & 7, pos, part == 1, pk, modisPix, line);
                m.add(64, part == 0 ? 1 : 2, pos * 2 & 0x3FFF, pk);
            }
        }
        modisScan++;
        nextLine += 10;
    }
    void avhrrLine(xband::Mux& m) {
        hrpt::Line l;
        l.day = 12000; l.ms = (uint32_t)(1000 + nextLine * 167);
        for (int c = 0; c < 5; c++) for (int x = 0; x < kHrptWidth; x++) l.px[(size_t)c * kHrptWidth + x] = (uint16_t)(xbTestPixel(c, x, nextLine) >> 2);
        auto u = hrpt::encodeAvhrrPacket(l);
        // the on-board targets (10-bit words of the packet from byte 14): PRTs, 10 black body views and 10 space views of channels 3b / 4 / 5
        const auto put = [&](int word, int v) { for (int k = 0; k < 10; k++) { const int bit = word * 10 + k; const size_t b = (size_t)14 + bit / 8; const uint8_t m = (uint8_t)(0x80 >> (bit % 8)); if ((v >> (9 - k)) & 1) u[b] |= m; else u[b] &= (uint8_t)~m; } };
        for (int i = 0; i < 3; i++) put(10295 + 2 + i, 300 + i);
        for (int i = 0; i < 10; i++) for (int j = 0; j < 3; j++) { put(10305 + 5 * i + j + 2, 400 + j * 20 + (i & 1)); put(5 * i + j + 2, 980 + j); }
        m.add(hrpt::kApid3a, 3, nextLine & 0x3FFF, u);
        nextLine++;
    }
    // DMSP RTD: an alarm (line start) frame and 490 pixel frames
    void dmspFrames(std::deque<uint8_t>& bits) {
        const auto frameBits = [&](const uint8_t words[16]) {
            std::vector<int> b;
            for (int i = 12; i >= 0; i--) b.push_back((0b1010110011111 >> i) & 1);
            b.push_back(0);
            for (int i = 1; i <= 15; i++) for (int k = 7; k >= 0; k--) b.push_back((words[i] >> k) & 1);
            while (b.size() < 150) b.push_back(0);
            for (int x : b) bits.push_back((uint8_t)x);
        };
        uint8_t alarm[16] = {};
        for (int i = 1; i < 13; i++) alarm[i] = (i & 1) == 0 ? 0x07 : 0xFB;
        alarm[15] = 0; // forward scan
        frameBits(alarm);
        for (int pos = 0; pos < 490; pos++) {
            uint8_t w[16] = {};
            for (int i = 0; i < 15; i++) w[1 + i] = (uint8_t)(((xbTestPixel(0, pos * 15 + i, nextLine) >> 6) & 63) << 2);
            for (int k = 0; k < 3; k++) {
                const uint8_t v = (uint8_t)(xbTestPixel(1, pos * 15 + k * 5, nextLine) >> 4);
                const int a = 1 + k * 5;
                w[a] |= (v >> 6) & 3; w[a + 1] |= (v >> 4) & 3; w[a + 2] |= (v >> 2) & 3; w[a + 3] |= v & 3;
            }
            w[1] |= 1;
            frameBits(w);
        }
        nextLine++;
    }
    void amsrLine(xband::Mux& m) {
        for (int pkt = 0; pkt < 20; pkt++) {
            std::vector<uint8_t> d(1018, 0);
            for (int i = 0; i < 25; i++) for (int c = 0; c < 20; c++) {
                const size_t off = (size_t)10 + 2 * (i * 20 + c);
                const int px = pkt * 25 + i;
                int v = xbTestPixel(c, px, nextLine);
                if (v >= 2048) { d[off] = (uint8_t)((v - 2048) >> 8 & 7); d[off + 1] = (uint8_t)(v - 2048); }
                else { d[off] = (uint8_t)(0x08 | (v >> 8 & 7)); d[off + 1] = (uint8_t)v; }
            }
            m.add(1576, pkt == 19 ? 1 : 0, 0, d);
        }
        nextLine++;
    }
    // METimage: 20 channels x 58 packets, 56 groups of 25 values a packet (first value of a group unused)
    void metimageSegment(xband::Mux& m) {
        static const struct { int marker, depth; bool rev; } ch[20] = {{30,18,true},{28,18,true},{26,18,true},{25,17,true},{23,17,true},{24,17,true},{22,17,true},{4,17,false},{12,17,false},{8,17,false},{17,17,false},{15,18,false},{16,18,false},{20,18,false},{40,18,false},{39,18,false},{37,17,false},{34,17,false},{35,17,false},{33,17,false}};
        const int seg = nextLine;
        for (int c = 0; c < 20; c++) for (int pk = 0; pk < 58; pk++) {
            const int depth = ch[c].depth;
            std::vector<uint8_t> d(depth == 17 ? 3027 - 6 : 3202 - 6, 0);
            d[15] = (uint8_t)ch[c].marker;
            const int offset = pk * 56;
            d[19] = (uint8_t)(offset >> 8); d[20] = (uint8_t)offset;
            std::vector<uint32_t> vals(1400, 0);
            for (int i = 0; i < 56; i++) for (int l = 0; l < 24; l++) {
                // the rows are reversed left to right after reassembly, and 56 columns are trimmed: plane x = 3248 - 1 - (offset + i) - 56
                const int planeX = 3248 - 1 - (offset + i) - 56;
                const int v = planeX >= 0 ? xbTestPixel(c, planeX, seg * 24 + l) : 0;
                vals[(size_t)i * 25 + 1 + (ch[c].rev ? 23 - l : l)] = (uint32_t)v << (depth - 12);
            }
            uint64_t sr = 0; int have = 0; size_t o = 44;
            for (uint32_t v : vals) {
                sr = sr << depth | v; have += depth;
                while (have >= 8 && o < d.size()) { d[o++] = (uint8_t)(sr >> (have - 8)); have -= 8; }
            }
            m.add(1443, 3, 0, d);
        }
        nextLine++;
    }
    void vegLine(xband::Mux& m) {
        // the first channel of the first group
        std::vector<uint8_t> d(7818, 0);
        std::vector<uint16_t> w(5200);
        for (int x = 0; x < 5200; x++) w[(size_t)x] = xbTestPixel(0, x, nextLine);
        xband::pack12(w.data(), 5200, &d[18]);
        m.add(290, 3, 0, d);
        nextLine++;
    }
    void tirsLine(xband::Mux& m) {
        std::vector<uint16_t> w(4000, 0);
        for (int i = 0; i < 640; i++) { w[(size_t)i] = xbTestPixel(0, i * 2, nextLine); w[(size_t)1941 + i] = xbTestPixel(0, i * 2 + 1, nextLine); }
        std::vector<uint8_t> d(5848, 0);
        xband::pack12(w.data(), (5848 - 31) * 2 / 3, &d[31]);
        m.add(1792, 3, 0, d);
        nextLine++;
    }
};

}

struct XbandGenerator::Impl {
    XbandGenConfig c;
    int P;
    XbLink L;
    xband::Cadu cadu;
    xband::Mux mux;
    Source src;
    double rs = 1;
    int syms = 2;
    std::vector<uint8_t> noFrame;
    std::vector<float> tab, I, Q;
    int64_t symN = 0;
    double sTime = 0, tstep = 0, phase = 0, freq = 0, amp = 1, sigma = 0, scale = 1;
    std::deque<uint8_t> bits;
    std::mt19937 rng;
    dect2::normal_distribution<float> nd{0.f, 1.f};
    hrpt::CcsdsEncoder enc;
    int nrz = 0;
    uint64_t caduN = 0;
    std::vector<uint8_t> frame;
    bool swapRail = false;
    int railPrevI = 0, railPrevQ = 0;
    std::vector<uint8_t> extra;

    static int scidFor(int p) {
        switch (p) { case kXbNpp: return 159; case kXbJpss2: return 177; case kXbAquaAura: return 154; case kXbTerra: return 42; case kXbMetopDump: return 11; case kXbMetopSg: return 3; default: return 1; }
    }
    static int vcidFor(int p) {
        switch (p) { case kXbNpp: case kXbJpss2: return 16; case kXbAquaAura: return 30; case kXbTerra: return 42; case kXbMetopDump: return 9; case kXbGcomW1: return 17; case kXbLandsatX: return 5; case kXbMetopSg: return 16; case kXbProbaS: return 4; default: return 0; }
    }
    explicit Impl(const XbandGenConfig& cfg) : c(cfg), P(cfg.profile), L(xbLink(cfg.profile)), cadu(L), mux(L, scidFor(cfg.profile), vcidFor(cfg.profile)), src(cfg.profile), rng(cfg.seed) {
        rs = L.symbolRate;
        syms = L.mod == kXbBpsk ? 1 : 2;
        amp = c.amp;
        tab.resize((size_t)(2 * kSpan * kTabRes + 1));
        double e = 0;
        for (size_t i = 0; i < tab.size(); i++) {
            const double t = (double)i / kTabRes - kSpan;
            const double w = 0.5 + 0.5 * std::cos(kPi * t / kSpan);
            tab[i] = (float)(dsp::rrc(t, L.alpha) * w);
            e += (double)tab[i] * tab[i] / kTabRes;
        }
        scale = amp / std::sqrt(syms == 2 ? 2 * e : e);
        if (c.snrDb < 99) { const double n0 = amp * amp / (std::pow(10.0, c.snrDb / 10) * rs); sigma = std::sqrt(n0 * c.rate / 2); }
        I.assign(64, 0); Q.assign(64, 0);
        tstep = rs * (1 + c.clockPpm * 1e-6) / c.rate;
        phase = c.phaseDeg * kPi / 180;
        frame.resize((size_t)L.caduBytes);
    }

    bool nextPayload() {
        while (true) {
            if (L.mpduData > 0 && L.caduBytes > 0) {
                if (mux.next(frame.data(), cadu)) { caduN++; return true; }
                switch (P) {
                case kXbNpp: case kXbJpss2: src.viirsGroup(mux); break;
                case kXbAquaAura: case kXbTerra: src.modisScanOut(mux); break;
                case kXbMetopDump: src.avhrrLine(mux); break;
                case kXbGcomW1: src.amsrLine(mux); break;
                case kXbLandsatX: src.tirsLine(mux); break;
                case kXbMetopSg: src.metimageSegment(mux); break;
                case kXbProbaS: src.vegLine(mux); break;
                default: return fallback();
                }
                continue;
            }
            return fallback();
        }
    }
    bool fallback() {
        // links without an instrument in the test signal: frames of a counting pattern on virtual channel 1
        std::memset(frame.data(), 0, (size_t)L.caduBytes);
        const int data = cadu.dataBytes();
        if (L.tmFrames) {
            frame[4] = 0; frame[5] = (uint8_t)(1 << 1); frame[7] = (uint8_t)caduN;
            frame[8] = 7; frame[9] = 0xFF;
        } else if (data >= 7) {
            frame[4] = (uint8_t)(1 << 6); frame[5] = 1;
            frame[6] = (uint8_t)(caduN >> 16); frame[7] = (uint8_t)(caduN >> 8); frame[8] = (uint8_t)caduN;
        }
        for (int i = 11; i < 4 + data; i++) frame[(size_t)i] = (uint8_t)(i * 7 + caduN);
        cadu.encode(frame.data());
        caduN++;
        return true;
    }

    // Oceansat-2: one bit a symbol through the mod 4 difference; the other bit is filled
    int osState = 0, osFill = 0;
    void oceansatBits() {
        std::vector<uint8_t> f;
        xband::ocmBuildFrame(f, src.nextLine++);
        for (int i = 0; i < 737760; i++) {
            const int b = f[(size_t)i >> 3] >> (7 - (i & 7)) & 1;
            osFill = (osFill * 5 + 7) & 3;
            const int d = b << 1 | (osFill & 1);
            osState = (osState + d) & 3;
            // quadrant: 0 (I+, Q+), 1 (I-, Q+), 2 (I-, Q-), 3 (I+, Q-)
            bits.push_back((uint8_t)(osState == 0 || osState == 3)); bits.push_back((uint8_t)(osState == 0 || osState == 1));
        }
    }
    // Kanopus-V: PMSS frames into 3056-byte blocks, re-randomised and interleaved over 8 sets, QPSK differential
    int qdX = 0, qdY = 0; bool qdFirst = true; int ccdLine = 0, ccdId = 1; std::vector<uint8_t> pmss;
    void kanopusBlock() {
        while (pmss.size() < 3056) {
            std::vector<uint8_t> frm(1960, 0);
            for (int i = 0; i < 16; i += 2) { frm[(size_t)i] = 0xf5; frm[(size_t)i + 1] = 0x0a; }
            const int line = ccdLine;
            frm[1944] = (uint8_t)ccdId;
            const int cnt = line << 1;
            frm[1945] = (uint8_t)cnt; frm[1946] = (uint8_t)(cnt >> 8 & 15);
            for (int x = 0; x < 1928; x++) frm[(size_t)16 + x] = (uint8_t)(xbTestPixel(ccdId, x, ccdPass * 495 + line) >> 4);
            pmss.insert(pmss.end(), frm.begin(), frm.end());
            if (++ccdLine == 495) { ccdLine = 0; if (++ccdId > 10) { ccdId = 1; ccdPass++; } }
        }
        static const uint8_t der[48] = {0x55,0xE2,0xA4,0x6A,0x30,0xA7,0xDA,0x16,0x24,0x86,0x39,0x07,0x0D,0x1D,0xEC,0x64,0xF0,0x3D,0xD7,0x66,0x98,0x08,0x15,0x94,0xA0,0xBA,0x5B,0x22,0x3E,0x67,0x40,0x1B,0x54,0x3A,0xB7,0x2B,0x9E,0xB0,0x4B,0xD3,0xB6,0xF3,0x8D,0xF1,0xE5,0xC4,0x27,0x36};
        static const uint8_t sy[4] = {0b00000100, 0b10110011, 0b11100011, 0b01110101};
        uint8_t dei[8][384] = {};
        int y = 0;
        for (int i = 0; i < 8; i++) for (int k = 0; k < 48; k++) for (int f : {1, 0, 3, 2, 5, 4, 7, 6}) { if (f < 2 && k == 0) continue; dei[f][i * 48 + k] = pmss[(size_t)y++]; }
        pmss.erase(pmss.begin(), pmss.begin() + 3056);
        for (int f = 0; f < 8; f++) for (int i = 0; i < 8; i++) for (int k = 0; k < 48; k++) dei[f][i * 48 + k] ^= der[k];
        for (int f = 0; f < 2; f++) for (int s = 0; s < 4; s++) dei[f][s * 48] = (uint8_t)(sy[s] ^ der[0]);
        // the receiver reads bit f of each interleaved byte; the sync bytes must arrive unscrambled: dei[f][s*48] carries sy[s] ^ der[0] and is XORed back there
        std::vector<int> stream(24576);
        for (int i = 0; i < 24576; i++) stream[(size_t)i] = dei[i % 8][i / 64] >> (7 - (i / 8 % 8)) & 1;
        for (int f = 0; f < 2; f++) for (int s = 0; s < 4; s++) for (int k = 0; k < 8; k++) { stream[(size_t)(s * 384 + k) * 8 + f] = (sy[s] >> (7 - k)) & 1; }
        for (size_t i = 0; i + 1 < stream.size(); i += 2) {
            int xo = stream[i], yo = stream[i + 1] ^ 1, xi, yi;
            if (qdFirst) { qdFirst = false; bits.push_back((uint8_t)qdX); bits.push_back((uint8_t)qdY); }
            // inverse of the receiver: try both branches of the differential rule
            xi = qdX ^ xo; yi = qdY ^ yo;
            if ((xi ^ yi) == 1) { xi = qdX ^ yo; yi = qdY ^ xo; if ((xi ^ yi) != 1) { xi = qdX ^ xo; yi = qdY ^ yo; } }
            qdX = xi; qdY = yi;
            bits.push_back((uint8_t)qdX); bits.push_back((uint8_t)qdY);
        }
    }
    int ccdPass = 0;

    void pushBits() {
        if (P == kXbDmsp) { src.dmspFrames(bits); return; }
        if (P == kXbOceansat2) { oceansatBits(); return; }
        if (P == kXbKanopus) { kanopusBlock(); return; }
        nextPayload();
        for (int i = 0; i < L.caduBytes * 8; i++) {
            int b = frame[(size_t)i >> 3] >> (7 - (i & 7)) & 1;
            if (L.fec == kXbFecConv) {
                if (L.nrzm) { nrz ^= b; b = nrz; }
                uint8_t a, bb;
                enc.push(b, a, bb);
                if (L.swapPairs) std::swap(a, bb);
                bits.push_back(a); bits.push_back(bb);
            } else if (L.railDiff) {
                // per-rail NRZ-M: even bit numbers on I, odd on Q
                if (i & 1) { railPrevQ ^= b; bits.push_back((uint8_t)railPrevQ); }
                else { railPrevI ^= b; bits.push_back((uint8_t)railPrevI); }
            } else {
                if (L.nrzm) { nrz ^= b; b = nrz; }
                bits.push_back((uint8_t)b);
            }
        }
    }

    void nextSym() {
        if (syms == 2) {
            while (bits.size() < 2) pushBits();
            int a = bits.front(); bits.pop_front();
            int b = bits.front(); bits.pop_front();
            float x = a ? 1.f : -1.f, y = b ? 1.f : -1.f;
            float ax, by;
            switch (c.ambiguity & 3) { case 0: ax = x; by = y; break; case 1: ax = -y; by = x; break; case 2: ax = -x; by = -y; break; default: ax = y; by = -x; break; }
            if (c.ambiguity & 4) by = -by;
            I[(size_t)(symN & 63)] = ax; Q[(size_t)(symN & 63)] = by;
        } else {
            while (bits.empty()) pushBits();
            I[(size_t)(symN & 63)] = bits.front() ? 1.f : -1.f; bits.pop_front();
            Q[(size_t)(symN & 63)] = 0;
        }
        symN++;
    }

    float h(double t) const {
        const double a = (t + kSpan) * kTabRes;
        if (a < 0 || a >= (double)tab.size() - 1) return 0;
        const int i = (int)a;
        const float fr = (float)(a - i);
        return tab[(size_t)i] + fr * (tab[(size_t)i + 1] - tab[(size_t)i]);
    }

    void generate(cf32* out, size_t n) {
        const bool oq = L.mod == kXbOqpsk;
        for (size_t i = 0; i < n; i++) {
            while (symN - kSpan - 2 < (int64_t)sTime) nextSym();
            const int64_t k0 = (int64_t)std::floor(sTime);
            float a = 0, b = 0;
            for (int64_t k = k0 - kSpan + 1; k <= k0 + kSpan; k++) {
                if (k < 0) continue;
                a += I[(size_t)(k & 63)] * h(sTime - (double)k);
                if (syms == 2) b += Q[(size_t)(k & 63)] * h(sTime - (double)k - (oq ? 0.5 : 0.0));
            }
            std::complex<double> s((double)a * scale, (double)b * scale);
            s *= std::polar(1.0, phase);
            phase += 2 * kPi * (c.cfoHz + freq) / c.rate;
            freq += c.driftHzPerS / c.rate;
            if (phase > 2 * kPi) phase -= 2 * kPi; else if (phase < -2 * kPi) phase += 2 * kPi;
            cf32 v((float)s.real(), (float)s.imag());
            if (sigma > 0) v += cf32((float)sigma * nd(rng), (float)sigma * nd(rng));
            out[i] = v;
            sTime += tstep;
        }
    }
};

XbandGenerator::XbandGenerator(const XbandGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
XbandGenerator::~XbandGenerator() = default;
void XbandGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }
uint64_t XbandGenerator::caduCount() const { return p_->caduN; }

std::vector<uint8_t> xbandTestCadus(int profile, int frames) {
    XbandGenConfig c; c.profile = profile;
    XbandGenerator::Impl g(c);
    std::vector<uint8_t> out;
    out.reserve((size_t)frames * g.L.caduBytes);
    for (int i = 0; i < frames; i++) { g.nextPayload(); out.insert(out.end(), g.frame.begin(), g.frame.end()); }
    return out;
}

namespace {
class Synth : public ModeSynth {
public:
    Synth(const XbandGenConfig& c) : gen_(c), r_(c.rate) {}
    double sampleRate() const override { return r_; }
    void generate(cf32* out, size_t n) override { gen_.generate(out, n); }
private:
    XbandGenerator gen_;
    double r_;
};
}

std::unique_ptr<ModeSynth> makeXbandSynth(const SynthConfig& cfg, double sampleRate, int profile) {
    XbandGenConfig c;
    c.profile = profile; c.rate = sampleRate; c.snrDb = cfg.snrDb; c.cfoHz = cfg.cfoHz;
    return std::make_unique<Synth>(c);
}

} // namespace dect2
