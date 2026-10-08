// Aero SU layer: SU CRC, block sizes, ISU/SSU assembly, ACARS reassembly, logons, system tables.
// Sources: JAERO JAERO/aerol.cpp and aerol.h (AeroLcrc16, setSettings block sizes, ISUData::update, ParserISU::parse,
// system table decoding). The real ACARS block is the one in test_aero_acars_real.cpp.
#include "dect2/aero_su.h"
#include "dect2/aero_acars.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// JAERO's bit-serial CRC (AeroLcrc16::calcusingbits), as an independent reference
static uint16_t jaeroCrc(const uint8_t* p, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 8; k++) {
            const int bit = (p[i] >> k) & 1, cb = crc & 1;
            crc >>= 1;
            if (cb ^ bit) crc ^= 0x8408;
        }
    return uint16_t(~crc);
}

static AeroAcars realMsg() {
    AeroAcars m;
    m.aesId = 0x4B1A2C; m.gesId = 0x12; m.uplink = true;
    m.mode = "2"; m.registration = "HB-JHM"; m.label = "A6"; m.blockId = "Q";
    m.text = "/PIKCPYA.ADS.HB-JHM07040B000C000D010E011000440D";
    return m;
}

// feeds SUs through blocks of the given rate; fill-in pads the last block
static void runRate(int rate, const std::vector<AeroSu>& sus, std::vector<AeroAcars>& acars, std::vector<AeroLogon>& logons,
                    AeroSuDecoder& dec, double& t) {
    const size_t per = aeroSusPerBlock(rate);
    for (size_t i = 0; i < sus.size(); i += per) {
        std::vector<AeroSu> part(sus.begin() + long(i), sus.begin() + long(std::min(sus.size(), i + per)));
        const std::vector<uint8_t> blk = aeroBuildBlock(part, rate);
        CHECK(blk.size() == aeroBlockBytes(rate), "block size");
        for (const AeroSu& s : aeroSplitSus(blk.data(), blk.size(), rate)) dec.feed(s, t, acars, logons);
        t += 0.5;
    }
}

int main() {
    // CRC: CRC-16/IBM-SDLC (X-25) check value is 0x906E; equals JAERO's bit-serial routine on random data
    CHECK(aeroSuCrc((const uint8_t*)"123456789", 9) == 0x906E, "X-25 check value %04X", aeroSuCrc((const uint8_t*)"123456789", 9));
    std::mt19937 rng(7);
    for (int i = 0; i < 200; i++) {
        uint8_t d[10];
        for (auto& x : d) x = uint8_t(rng());
        CHECK(aeroSuCrc(d, 10) == jaeroCrc(d, 10), "crc equals JAERO's");
    }

    CHECK(aeroBlockBytes(600) == 24 && aeroBlockBytes(1200) == 36 && aeroBlockBytes(10500) == 312 && aeroBlockBytes(5) == 0, "block bytes");
    CHECK(aeroSusPerBlock(600) == 2 && aeroSusPerBlock(1200) == 3 && aeroSusPerBlock(10500) == 26, "SUs per block");

    // fill-in SU and the all-zero SU JAERO accepts
    { AeroSu f = aeroFillSu(); CHECK(f.crcOk && f.type == 0x01 && f.bytes[0] == 0x01, "fill"); 
      uint8_t z[12] = {}; auto v = aeroSplitSus(z, 12, 600); CHECK(v.size() == 1 && v[0].crcOk && v[0].type == 0, "zero SU"); }

    // logon / logoff
    { AeroSu l = aeroLogonSu(0x4B1A2C, 0x12, true);
      CHECK(l.type == 0x11 && l.aesId == 0x4B1A2C && l.gesId == 0x12 && l.bytes[1] == 0x4B && l.bytes[4] == 0x12, "logon SU layout");
      AeroSu o = aeroLogonSu(0x4B1A2C, 0x12, false);
      std::vector<AeroSu> sus{l, o, aeroFillSu()};
      AeroSuDecoder d; std::vector<AeroAcars> a; std::vector<AeroLogon> lg; double t = 0;
      runRate(600, sus, a, lg, d, t);
      CHECK(lg.size() == 2 && lg[0].logon && !lg[1].logon && lg[0].aesId == 0x4B1A2C && lg[0].gesId == 0x12, "logons decoded %zu", lg.size());
      CHECK(a.empty(), "no message from logons"); }

    // the real ACARS message through the ISU/SSU layer at every rate
    const int rates[3] = {600, 1200, 10500};
    for (int rate : rates) {
        const AeroAcars m = realMsg();
        std::vector<AeroSu> sus = aeroAcarsToSus(m);
        CHECK(sus.size() == 10, "ISU of the real block: 1 + 9 SUs, got %zu", sus.size());
        CHECK(sus[0].type == 0x71 && sus[0].bytes[6] == 9 && (sus[0].bytes[7] >> 4) == 1, "ISU head fields");
        CHECK(sus[1].type == 0xC8 && sus[9].type == 0xC0, "SSU numbering %02X .. %02X", sus[1].type, sus[9].type);
        AeroSuDecoder d; std::vector<AeroAcars> a; std::vector<AeroLogon> lg; double t = 0;
        // a system table and a logon in front, fill-in between
        std::vector<AeroSu> all{aeroSystemTableSu(0x12, 0), aeroLogonSu(m.aesId, m.gesId, true)};
        all.insert(all.end(), sus.begin(), sus.end());
        all.push_back(aeroFillSu());
        runRate(rate, all, a, lg, d, t);
        CHECK(a.size() == 1, "rate %d: %zu messages", rate, a.size());
        if (a.size() == 1) {
            const AeroAcars& r = a[0];
            CHECK(r.aesId == m.aesId && r.gesId == m.gesId && r.uplink, "ids");
            CHECK(r.registration == "HB-JHM" && r.label == "A6" && r.blockId == "Q" && r.mode == "2", "header fields");
            CHECK(r.text == m.text && r.crcOk && r.labelText == "ADS-C uplink", "text '%s'", r.text.c_str());
        }
        CHECK(lg.size() == 1, "logon seen");
    }

    // several messages, two aircraft interleaved at SU level (their ISUs have different QNO/REF), multi-block message
    {
        AeroAcars a1 = realMsg();
        AeroAcars a2; a2.aesId = 0x710123; a2.gesId = 0x12; a2.registration = "A6-EEB"; a2.label = "H1"; a2.mode = "2";
        a2.text = "METAR OMDB 071200Z 31008KT 9999 FEW040 36/18 Q1004 NOSIG";
        AeroAcars a3; a3.aesId = 0x710124; a3.gesId = 0x12; a3.registration = "VT-ANA"; a3.label = "5Z"; a3.mode = "2";
        for (int i = 0; i < 40; i++) a3.text += "ATIS INFORMATION " + std::to_string(i) + " ";   // > 220 chars: two blocks
        auto s1 = aeroAcarsToSus(a1, 1, 3), s2 = aeroAcarsToSus(a2, 2, 5), s3 = aeroAcarsToSus(a3, 0, 9);
        CHECK(a3.text.size() > 220, "long text");
        std::vector<AeroSu> mix;
        // SSUs of the first ISU interleaved with the second ISU's SUs
        size_t i = 0, j = 0;
        while (i < s1.size() || j < s2.size()) {
            if (i < s1.size()) mix.push_back(s1[i++]);
            if (j < s2.size()) mix.push_back(s2[j++]);
        }
        mix.insert(mix.end(), s3.begin(), s3.end());
        AeroSuDecoder d; std::vector<AeroAcars> a; std::vector<AeroLogon> lg; double t = 0;
        runRate(10500, mix, a, lg, d, t);
        CHECK(a.size() == 3, "three messages, got %zu", a.size());
        int found = 0;
        for (auto& r : a) {
            if (r.registration == "HB-JHM") { found++; CHECK(r.text == a1.text, "msg1"); }
            if (r.registration == "A6-EEB") { found++; CHECK(r.text == a2.text && r.blockId == "A", "msg2"); }
            if (r.registration == "VT-ANA") { found++; CHECK(r.text == a3.text && r.crcOk && r.label == "5Z", "msg3 (two blocks) len %zu", r.text.size()); }
        }
        CHECK(found == 3, "all three found");
    }

    // bad SU CRC: the message is dropped, the decoder carries on
    {
        auto s = aeroAcarsToSus(realMsg());
        s[4].bytes[5] ^= 0x04;
        s[4].crcOk = aeroSuCrc(s[4].bytes, 10) == uint16_t(s[4].bytes[10] | (s[4].bytes[11] << 8));
        std::vector<uint8_t> blk;
        AeroSuDecoder d; std::vector<AeroAcars> a; std::vector<AeroLogon> lg; double t = 0;
        runRate(10500, s, a, lg, d, t);
        // via split the CRC is recomputed from bytes: flip in the bytes themselves
        std::vector<AeroSu> s2 = aeroAcarsToSus(realMsg());
        std::vector<uint8_t> raw = aeroBuildBlock(s2, 10500);
        raw[12 * 4 + 5] ^= 0x04;
        auto sp = aeroSplitSus(raw.data(), raw.size(), 10500);
        CHECK(!sp[4].crcOk && sp[4].typeName == "Bad CRC" && sp[3].crcOk, "corrupt SU flagged");
        AeroSuDecoder d2; std::vector<AeroAcars> a2; std::vector<AeroLogon> l2;
        for (auto& x : sp) d2.feed(x, 0, a2, l2);
        CHECK(a2.empty(), "message with a lost SSU is not output");
        // the next message still decodes
        AeroAcars m = realMsg(); m.text = "AFTER";
        for (auto& x : aeroAcarsToSus(m)) d2.feed(x, 1, a2, l2);
        CHECK(a2.size() == 1 && a2[0].text == "AFTER", "decoder recovers");
    }

    // downlink: message number and flight id in the text, block id is a digit
    {
        AeroAcars m; m.aesId = 0x4B1A2C; m.gesId = 0x12; m.uplink = false; m.registration = "HB-JHM"; m.label = "H1";
        m.msgNo = "M12A"; m.flight = "LX123"; m.text = "POS N2512E05530";
        AeroSuDecoder d; std::vector<AeroAcars> a; std::vector<AeroLogon> lg; double t = 0;
        runRate(1200, aeroAcarsToSus(m), a, lg, d, t);
        CHECK(a.size() == 1 && !a[0].uplink && a[0].msgNo == "M12A" && a[0].flight == "LX123" && a[0].text == m.text,
              "downlink fields (%zu)", a.size());
    }

    // non-ACARS user data is passed on as hex
    {
        AeroSu head = aeroFillSu();
        uint8_t raw[12] = {0x71, 0x4B, 0x1A, 0x2C, 0x12, 0x01, 0x01, 0x30, 0xDE, 0xAD};
        AeroSu h; std::memcpy(h.bytes, raw, 10);
        uint16_t c = aeroSuCrc(h.bytes, 10); h.bytes[10] = c & 255; h.bytes[11] = c >> 8;
        uint8_t raw2[12] = {0xC0, 0x01, 0xBE, 0xEF, 0x55};
        AeroSu s2; std::memcpy(s2.bytes, raw2, 12);
        c = aeroSuCrc(s2.bytes, 10); s2.bytes[10] = c & 255; s2.bytes[11] = c >> 8;
        std::vector<uint8_t> blk;
        for (auto* p : {&h, &s2}) blk.insert(blk.end(), p->bytes, p->bytes + 12);
        auto sus = aeroSplitSus(blk.data(), blk.size(), 600);
        AeroSuDecoder d; std::vector<AeroAcars> a; std::vector<AeroLogon> lg;
        for (auto& x : sus) d.feed(x, 0, a, lg);
        CHECK(a.size() == 1 && a[0].text == "DEADBEEF55" && a[0].labelText == "Non-ACARS user data", "non-ACARS data");
        (void)head;
    }

    // system information round trip (field positions as JAERO reads them)
    {
        AeroSu s = aeroSystemTableSu(0x12, 0);
        AeroSysInfo i = aeroParseSystemSu(s);
        CHECK(i.kind == 1 && i.gesId == 0x12 && i.lsu == 0 && i.freqMHz[0] > 1544.9 && i.freqMHz[0] < 1546.1 && i.freqMHz[1] > 1620, "GES channels %.4f %.4f", i.freqMHz[0], i.freqMHz[1]);
        AeroSu s3 = aeroSystemTableSu(0x12, 3);
        i = aeroParseSystemSu(s3);
        CHECK(i.kind == 1 && i.lsu == 3 && i.names == "Rsmc5 Rsmc6 Rsmc7", "lsu 3");
        AeroSu sat = aeroSatelliteIdSu(5, 64.5, 1545.0125, 1545.5, 3);
        i = aeroParseSystemSu(sat);
        CHECK(i.kind == 2 && i.satId == 5 && i.seq == 3 && i.lonDeg > 64.4 && i.lonDeg < 64.6 && i.freqMHz[0] > 1545.01 && i.freqMHz[0] < 1545.015 && i.freqMHz[1] > 1545.49, "satellite SU");
        AeroSu w = aeroSatelliteIdSu(2, -98.0, 1545.0, 0, 0);
        i = aeroParseSystemSu(w);
        CHECK(i.lonDeg < -97.4 && i.lonDeg > -98.6 && i.freqMHz[1] == 0, "west longitude %.2f", i.lonDeg);
        AeroSu cc = aeroChannelControlSu(0x12, 10500, 1545.075, true);
        i = aeroParseSystemSu(cc);
        CHECK(i.kind == 3 && i.bitRate == 10500 && i.gesId == 0x12 && i.spotBeam && i.freqMHz[0] > 1545.07 && i.freqMHz[0] < 1545.08, "channel control");
        for (int r : {600, 1200, 5250, 8400}) CHECK(aeroParseSystemSu(aeroChannelControlSu(1, r, 1545.0)).bitRate == r, "bit rate code %d", r);
        CHECK(aeroDescribeSu(cc).find("10500 bit/s") != std::string::npos, "describe: %s", aeroDescribeSu(cc).c_str());
        CHECK(aeroDescribeSu(aeroLogonSu(0x4B1A2C, 0x12, true)).find("4B1A2C") != std::string::npos, "describe logon");
        for (int p = 0; p < 6; p++) CHECK(aeroSystemTableSu(7, p).crcOk, "part %d", p);
    }

    // random blocks: no crash, nearly no accepted SUs
    {
        AeroSuDecoder d; std::vector<AeroAcars> a; std::vector<AeroLogon> lg;
        size_t ok = 0, total = 0;
        for (int b = 0; b < 3000; b++) {
            std::vector<uint8_t> blk(312);
            for (auto& x : blk) x = uint8_t(rng());
            for (auto& s : aeroSplitSus(blk.data(), blk.size(), 10500)) { total++; ok += s.crcOk; d.feed(s, b * 0.5, a, lg); }
        }
        CHECK(ok <= 5, "random SUs accepted: %zu of %zu", ok, total);
    }
    // short block and unknown rate
    { uint8_t z[5] = {}; CHECK(aeroSplitSus(z, 5, 600).empty() && aeroSplitSus(z, 5, 77).empty(), "short / unknown"); }

    if (fails) return 1;
    printf("aero_su_layers OK\n");
    return 0;
}
