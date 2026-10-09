// Mesh (LoRa) physical layer: known answers of the coding chain, encode -> decode for every SF / CR / LDRO, error correction,
// and modulation -> channelizer -> demodulator bit-exact at a clean SNR for SF 7..12 and the four bandwidths.
#include "data/mesh/testutil.h"
#include <cstring>
using namespace dect2;
using namespace meshtest;

static void kats() {
    // gr-lora_sdr lib/tables.h, whitening_seq[0..15]
    const uint8_t w[16] = {0xFF, 0xFE, 0xFC, 0xF8, 0xF0, 0xE1, 0xC2, 0x85, 0x0B, 0x17, 0x2F, 0x5E, 0xBC, 0x78, 0xF1, 0xE3};
    for (int i = 0; i < 16; i++) CHECK(lora::whitening(i) == w[i], "whitening[%d] = %02x", i, lora::whitening(i));
    // ... and its last entries (the table has 255 bytes: 0x3F, 0x7F end it)
    CHECK(lora::whitening(253) == 0x3F && lora::whitening(254) == 0x7F, "whitening end %02x %02x", lora::whitening(253), lora::whitening(254));
    // gr-lora_sdr hamming_dec_impl.cc cw_LUT / cw_LUT_cr5: the codewords of [d0 d1 d2 d3] (d0 = bit 0 of the nibble) as 8-bit values
    const uint8_t lut8[16] = {0, 23, 45, 58, 78, 89, 99, 116, 139, 156, 166, 177, 197, 210, 232, 255};
    const uint8_t lut5[16] = {0, 24, 40, 48, 72, 80, 96, 120, 136, 144, 160, 184, 192, 216, 232, 240};
    for (int n = 0; n < 16; n++) {
        const int nib = ((n >> 3) & 1) | ((n >> 2) & 1) << 1 | ((n >> 1) & 1) << 2 | (n & 1) << 3;   // [d0 d1 d2 d3] -> nibble
        for (int cr = 6; cr <= 8; cr++)
            CHECK(lora::hammingEncode((uint8_t)nib, cr) == (lut8[n] >> (8 - cr)), "hamming 4/%d of %x: %02x", cr, nib, lora::hammingEncode((uint8_t)nib, cr));
        CHECK(lora::hammingEncode((uint8_t)nib, 5) == (lut5[n] >> 3), "hamming 4/5 of %x", nib);
        // every single-bit error is corrected at 4/7 and 4/8, detected at 4/5 and 4/6
        for (int cr = 5; cr <= 8; cr++)
            for (int b = 0; b < cr; b++) {
                int e = 0;
                const uint8_t d = lora::hammingDecode((uint8_t)(lora::hammingEncode((uint8_t)nib, cr) ^ (1 << b)), cr, &e);
                if (cr >= 7) CHECK(d == nib && e == 1, "4/%d bit %d of %x -> %x (%d)", cr, b, nib, d, e);
                else CHECK(e == 2, "4/%d error not seen", cr);
            }
        // two errors are detected at 4/8
        int e = 0;
        lora::hammingDecode((uint8_t)(lora::hammingEncode((uint8_t)nib, 8) ^ 0x81), 8, &e);
        CHECK(e == 2, "4/8 double error of %x: %d", nib, e);
    }
    // sync word symbols: nibble << 3 as a signed 7-bit value (an SX1262 sending 0x2B at SF11 puts the second one at -40, not 88)
    CHECK(lora::syncSymbol(0x2B, 0, 11) == 16 && lora::syncSymbol(0x2B, 1, 11) == 2048 - 40, "0x2B SF11: %d %d", lora::syncSymbol(0x2B, 0, 11), lora::syncSymbol(0x2B, 1, 11));
    CHECK(lora::syncSymbol(0x2B, 1, 7) == 88, "0x2B SF7: %d", lora::syncSymbol(0x2B, 1, 7));
    CHECK(lora::syncSymbol(0x12, 0, 8) == 8 && lora::syncSymbol(0x12, 1, 8) == 16, "0x12 SF8");
    CHECK(lora::syncSymbol(0x34, 0, 12) == 24 && lora::syncSymbol(0x34, 1, 12) == 32, "0x34 SF12");
    // header checksum (gr-lora_sdr header_impl.cc): a header with all-zero nibbles has checksum 0; each nibble bit feeds the bits
    // the equations of header_impl.cc list
    CHECK(lora::headerChecksum(0, 0, 0) == 0, "checksum 0");
    CHECK(lora::headerChecksum(0x8, 0, 0) == 0x18, "n0 bit 3 -> c4 c3: %02x", lora::headerChecksum(0x8, 0, 0));
    CHECK(lora::headerChecksum(0, 0, 0x1) == 0x0B, "n2 bit 0 -> c3 c1 c0: %02x", lora::headerChecksum(0, 0, 1));
    CHECK(lora::headerChecksum(0, 0x1, 0) == 0x06, "n1 bit 0 -> c2 c1: %02x", lora::headerChecksum(0, 1, 0));
    // the CRC: CRC-16/XMODEM (poly 0x1021, init 0) of all but the last two bytes, XOR the last two (add_crc_impl.cc).
    // "123456789" + two zero bytes gives the XMODEM check value 0x31C3.
    const uint8_t s[11] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', 0, 0};
    CHECK(lora::payloadCrc(s, 11) == 0x31C3, "crc %04x", lora::payloadCrc(s, 11));
    // air time: SX1261/2 datasheet 6.1.4; Meshtastic LongFast (SF11, 250 kHz, 4/5, preamble 16), 50 bytes: 8 + ceil(400/44) * 5 = 58
    lora::Params lf; lf.sf = 11; lf.bwHz = 250e3; lf.cr = 5; lf.preamble = 16;
    CHECK(lora::dataSymbols(lf, 50) == 58, "symbols %d", lora::dataSymbols(lf, 50));
    CHECK(std::fabs(lora::airSeconds(lf, 50) - (16 + 4.25 + 58) * 8.192e-3) < 1e-9, "air time");
    CHECK(!lora::autoLdro(11, 250e3) && lora::autoLdro(11, 125e3) && lora::autoLdro(12, 125e3) && lora::autoLdro(12, 250e3) && lora::autoLdro(10, 62.5e3) && !lora::autoLdro(9, 62.5e3),
          "low data rate rule");
}

static void coding() {
    std::mt19937 g(5);
    for (int sf = 7; sf <= 12; sf++)
        for (int cr = 5; cr <= 8; cr++)
            for (int ldro = 0; ldro <= 1; ldro++)
                for (int len : {1, 2, 3, 16, 37, 100, 255}) {
                    lora::Params p; p.sf = sf; p.cr = cr; p.ldro = ldro; p.crc = (len % 2) == 1 || len > 2;
                    const auto pay = randomBytes(g, (size_t)len);
                    auto sym = lora::encode(p, pay.data(), pay.size());
                    CHECK((int)sym.size() == lora::dataSymbols(p, pay.size()), "sf%d cr%d ldro%d len %d: %zu symbols, formula %d", sf, cr, ldro, len, sym.size(), lora::dataSymbols(p, pay.size()));
                    for (int pass = 0; pass < 2; pass++) {
                        // pass 1: one bin off by +-1 in every block (what noise does most): corrected at 4/7 and 4/8, and in the header
                        auto s2 = sym;
                        if (pass == 1) {
                            if (cr < 7) break;
                            for (size_t i = 0; i < s2.size(); i++) {
                                const bool reduced = i < 8 || ldro;
                                const size_t blk = i < 8 ? i : (i - 8) % (size_t)cr;
                                if (blk == 3) s2[i] = (uint16_t)((s2[i] + (reduced ? 4 : 1) * ((i & 1) ? 1 : -1) + (1 << sf)) % (1 << sf));
                            }
                        }
                        lora::FrameDecoder fd;
                        fd.start(sf, ldro);
                        for (auto v : s2) fd.push(v);
                        std::vector<uint8_t> out; bool ok = false; int corr = 0;
                        CHECK(fd.header().ok && fd.header().len == len && fd.header().cr == cr && fd.header().crc == p.crc, "header sf%d cr%d len %d", sf, cr, len);
                        CHECK(fd.done() && fd.finish(out, ok, corr), "finish sf%d cr%d len %d", sf, cr, len);
                        CHECK(out == pay && (ok || !p.crc), "payload sf%d cr%d ldro%d len %d pass %d crc %d", sf, cr, ldro, len, pass, ok);
                        if (pass == 1) CHECK(corr > 0, "no corrections counted");
                    }
                }
}

static void modem() {
    std::mt19937 g(7);
    struct Case { int sf; double bw; int cr; bool ldro; double rate; double off; };
    const Case cases[] = {
        {7, 125e3, 5, false, 1e6, 100e3}, {8, 62.5e3, 8, false, 2e6, -207e3}, {9, 250e3, 5, false, 2e6, 0}, {10, 125e3, 6, false, 2.4e6, 300e3},
        {11, 250e3, 5, false, 2e6, -300e3}, {11, 125e3, 8, true, 2e6, 50e3}, {12, 125e3, 8, true, 1e6, -20e3}, {12, 500e3, 7, false, 2e6, 0},
        {7, 500e3, 5, false, 1e6, 0},
    };
    for (const auto& c : cases) {
        lora::Params p; p.sf = c.sf; p.bwHz = c.bw; p.cr = c.cr; p.ldro = c.ldro; p.preamble = 12; p.syncWord = 0x2B;
        std::vector<lora::TxFrame> fr;
        std::vector<std::vector<uint8_t>> pays;
        double t = 0.05;
        for (int i = 0; i < 3; i++) {
            lora::TxFrame f;
            f.p = p;
            pays.push_back(randomBytes(g, 10 + 30 * i));
            f.data = lora::encode(p, pays.back().data(), pays.back().size());
            f.startSec = t; f.freqHz = c.off; f.amp = 0.3f; f.phase0 = 0.1 * i;
            t = f.endSec() + 0.02;
            fr.push_back(f);
        }
        const auto x = render(fr, t + 0.1, c.rate, 0.01);
        Chain ch(c.rate, c.off, p);
        ch.feedAll(x, 8192);
        int got = 0;
        for (size_t i = 0; i < ch.frames.size() && i < pays.size(); i++) got += ch.frames[i].crcOk && ch.frames[i].payload == pays[i];
        CHECK(got == 3 && ch.frames.size() == 3, "SF%d BW %.1f kHz CR 4/%d at %.1f Msps: %d of 3 frames, %zu decoded", c.sf, c.bw / 1e3, c.cr, c.rate / 1e6, got, ch.frames.size());
        for (size_t i = 0; i < ch.frames.size(); i++) if (!(ch.frames[i].crcOk && i < pays.size() && ch.frames[i].payload == pays[i]))
            printf("  frame %zu: crc %d len %d (sent %zu) corrected %d cfo %.1f sfo %.1f\n", i, ch.frames[i].crcOk, ch.frames[i].hdr.len, i < pays.size() ? pays[i].size() : 0, ch.frames[i].corrected, ch.frames[i].cfoHz, ch.frames[i].sfoPpm);
        if (!ch.frames.empty()) {
            const auto& f = ch.frames[0];
            printf("SF%-2d BW %5.1f kHz 4/%d %4.1f Msps: %d/3 frames, cfo %+.1f Hz, snr %.1f dB, sync %02x\n", c.sf, c.bw / 1e3, c.cr, c.rate / 1e6, got, f.cfoHz, f.snrDb, f.syncWord);
        }
    }
}

int main() {
    kats();
    coding();
    modem();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
