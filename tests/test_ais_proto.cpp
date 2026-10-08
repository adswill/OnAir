// AIS message layer: known answers from gpsd's test/sample.aivdm and sample.aivdm.chk (real sentences, with the values that the noaadata
// tools and the Maritec decoder gave), the frame check value of the X.25 FCS, NRZI and stuffing round trips, and !AIVDM sentences rebuilt
// bit for bit from the decoded payload. (The type 5 ETA hour is 14 as in sample.aivdm.chk, the regression file; the comment in sample.aivdm says 16.)
#include "dect2/ais_proto.h"
#include "dect2/ais_tel.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;
using namespace dect2::ais;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

// payload of one message from its sentences
static Bits fromSentences(const std::vector<std::string>& ss) {
    std::string all; int fill = 0, cnt = 0, no = 0;
    for (const auto& s : ss) {
        std::string c;
        if (!parseNmea(s, c, fill, cnt, no)) { printf("FAIL: sentence does not parse (checksum?): %s\n", s.c_str()); fails++; return {}; }
        all += c;
    }
    return unarmour(all, fill);
}

// exact: the sentences are split the way toNmea splits them (60 characters); some encoders split elsewhere, those are only decoded
static AisMsg dec(const std::vector<std::string>& ss, bool exact = true) {
    AisMsg m;
    const Bits b = fromSentences(ss);
    decodeMessage(b, m);
    // the sentence must come back exactly (armouring, fill bits, splitting, checksum)
    std::string c; int fill, cnt, no; char ch = 'A';
    parseNmea(ss[0], c, fill, cnt, no, &ch);
    const std::string seq = cnt > 1 ? ss[0].substr(ss[0].find(',', 10) + 1, 1) : "";
    const auto again = toNmea(b, ch, cnt > 1 ? atoi(seq.c_str()) : 0);
    CHECK(!exact || again == ss, "sentence rebuilt: %s -> %s", ss[0].c_str(), again.empty() ? "" : again[0].c_str());
    return m;
}

int main() {
    // ---- the FCS: X.25 / ISO 13239 check value of "123456789" is 0x906E (CRC-16/IBM-SDLC in the CRC catalogue)
    {
        Bits b;
        for (const char* p = "123456789"; *p; p++) for (int i = 0; i < 8; i++) b.push_back(((uint8_t)*p >> i) & 1);   // bytes least significant bit first
        CHECK((uint16_t)~fcsRegister(b.data(), b.size()) == 0x906E, "FCS check value");
        Bits f = b; appendFcs(f);
        CHECK(fcsRegister(f.data(), f.size()) == kFcsGood && fcsOk(f), "residue after the FCS");
        f[10] ^= 1;
        CHECK(!fcsOk(f), "a flipped bit is caught");
    }
    // ---- stuffing, NRZI, flags
    {
        std::mt19937 rng(5);
        for (int t = 0; t < 200; t++) {
            Bits d(20 + rng() % 400);
            for (auto& x : d) x = (rng() % 4) != 0;        // many ones: stuffing happens
            const Bits s = stuff(d);
            for (size_t i = 0, ones = 0; i < s.size(); i++) { ones = s[i] ? ones + 1 : 0; if (ones > 5) { CHECK(false, "six ones after stuffing"); break; } }
            Bits o;
            CHECK(destuff(s, o) && o == d, "stuff / destuff round trip");
            for (int lv = 0; lv < 2; lv++) CHECK(nrziDecode(nrziEncode(d, lv), lv) == d, "NRZI round trip");
        }
        Bits six(20, 1), o;
        CHECK(!destuff(six, o), "six ones in a row are refused");
        // a burst: training, flag, frame, flag; decode from its line bits
        Bits payload; putU(payload, 0, 6, 1); putU(payload, 6, 2, 0); putU(payload, 8, 30, 123456789); putU(payload, 38, 130, 0x5555);
        for (int lead = 0; lead < 40; lead += 13) {
            Bits line = burstLineBits(payload);
            Bits pre(lead);
            for (auto& x : pre) x = rng() & 1;
            pre.insert(pre.end(), line.begin(), line.end());
            const HdlcResult r = hdlcFrames(nrziDecode(pre, 1));
            CHECK(r.good.size() == 1 && r.good[0] == payload, "burst frame found (lead %d, %zu frames)", lead, r.good.size());
        }
        Bits line = burstLineBits(payload);
        line[24 + 8 + 60] ^= 1;                 // one bit error inside the frame
        const HdlcResult r = hdlcFrames(nrziDecode(line, 1));
        CHECK(r.good.empty() && r.bad >= 1, "damaged frame is counted as bad (%d)", r.bad);
    }
    // ---- armouring
    {
        Bits b; putU(b, 0, 6, 0); putU(b, 6, 6, 39); putU(b, 12, 6, 40); putU(b, 18, 6, 63);
        int fill; const std::string s = armour(b, fill);
        CHECK(s == "0W`w" && fill == 0, "armour table: %s", s.c_str());
        Bits c = unarmour(s, 0);
        CHECK(c == b, "unarmour");
        Bits odd(10, 1); const std::string s2 = armour(odd, fill);
        CHECK(fill == 2 && unarmour(s2, fill) == odd, "fill bits");
        std::string ch; int f, cn, no;
        CHECK(!parseNmea("!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4B", ch, f, cn, no), "bad checksum refused");
    }
    // ---- type 1, 2, 3 (sample.aivdm)
    {
        AisMsg m = dec({"!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A"});
        CHECK(m.valid && m.type == 1 && m.mmsi == 371798000 && m.navStatus == 0, "type 1 header");
        CHECK(m.hasRot && m.rotDegMin < -700, "rot -127");
        CHECK(near(m.sog, 12.3, 1e-3) && near(m.lon, -123.395383333, 1e-6) && near(m.lat, 48.3816333333, 1e-6), "type 1 position %f %f %f", m.sog, m.lon, m.lat);
        CHECK(near(m.cog, 224, 1e-3) && m.heading == 215 && m.second == 33 && m.cls == AIS_CLASS_A, "type 1 course");
        m = dec({"!AIVDM,1,1,,A,16SteH0P00Jt63hHaa6SagvJ087r,0*42"});
        CHECK(m.valid && m.type == 1 && m.mmsi == 440348000 && !m.hasRot && near(m.sog, 0) && near(m.lon, -42454920 / 600000.0, 1e-6) && near(m.lat, 25848090 / 600000.0, 1e-6) &&
              near(m.cog, 93.4, 1e-3) && m.heading == -1, "type 1 second sample");
        m = dec({"!AIVDM,1,1,,A,38Id705000rRVJhE7cl9n;160000,0*40"});
        CHECK(m.valid && m.type == 3 && m.mmsi == 563808000 && m.navStatus == 5 && near(m.lon, -45796520 / 600000.0, 1e-6) && near(m.lat, 22146000 / 600000.0, 1e-6) &&
              near(m.cog, 252.0, 1e-3) && m.heading == 352 && m.second == 35, "type 3");
        m = dec({"!AIVDM,1,1,,B,25Cjtd0Oj;Jp7ilG7=UkKBoB0<06,0*60"});
        CHECK(m.valid && m.type == 2 && m.mmsi == 356302000 && m.heading == 91 && near(m.sog, 13.9, 1e-3), "type 2");
    }
    // ---- type 4
    {
        AisMsg m = dec({"!AIVDM,1,1,,A,403OviQuMGCqWrRO9>E6fE700@GO,0*4D"});
        CHECK(m.valid && m.type == 4 && m.mmsi == 3669702 && m.year == 2007 && m.month == 5 && m.day == 14 && m.hour == 19 && m.minute == 57 && m.sec == 39, "type 4 time");
        CHECK(near(m.lon, -76.35236166667, 1e-6) && near(m.lat, 36.88376666667, 1e-6) && m.epfd == 7 && m.cls == AIS_CLASS_BASE, "type 4 position");
    }
    // ---- type 5, two sentences
    {
        AisMsg m = dec({"!AIVDM,2,1,1,A,55?MbV02;H;s<HtKR20EHE:0@T4@Dn2222222216L961O5Gf0NSQEp6ClRp8,0*1C", "!AIVDM,2,2,1,A,88888888880,2*25"});
        CHECK(m.valid && m.type == 5 && m.mmsi == 351759000 && m.imo == 9134270 && m.callsign == "3FOF8" && m.name == "EVER DIADEM", "type 5 names: '%s' '%s'", m.callsign.c_str(), m.name.c_str());
        CHECK(m.shipType == 70 && m.dimA == 225 && m.dimB == 70 && m.dimC == 1 && m.dimD == 31 && m.epfd == 1, "type 5 dimensions");
        CHECK(m.etaMonth == 5 && m.etaDay == 15 && m.etaHour == 14 && m.etaMin == 0 && near(m.draughtM, 12.2, 1e-3) && m.destination == "NEW YORK", "type 5 voyage: %s eta %d-%d %d:%d draught %f", m.destination.c_str(), m.etaMonth, m.etaDay, m.etaHour, m.etaMin, m.draughtM);
        m = dec({"!AIVDM,2,1,2,A,542M92h00001@<7;?G0PD4i@R0<tqA8tj37>220o0h:2240Ht500000000000000,0*3C", "!AIVDM,2,2,2,A,0000002,2*24"}, false);
        CHECK(m.valid && m.type == 5, "type 5 with 420 bits (a transmitter that leaves out the spare bits)");
    }
    // ---- 6 and 8: header only
    {
        AisMsg m = dec({"!AIVDM,1,1,,B,6B?n;be:cbapalgc;i6?Ow4,2*4A"});
        CHECK(m.valid && m.type == 6 && m.repeat == 1 && m.mmsi == 150834090 && m.destMmsi == 313240222 && m.dac == 669 && m.fi == 11, "type 6 header: %d/%d", m.dac, m.fi);
        m = dec({"!AIVDM,1,1,,A,85Mwp`1Kf3aCnsNvBWLi=wQuNhA5t43N`5nCuI=p<IBfVqnMgPGs,0*47"});
        CHECK(m.valid && m.type == 8 && m.mmsi == 366999712 && m.dac == 366 && m.fi == 56, "type 8 header: %d/%d", m.dac, m.fi);
    }
    // ---- 9, 14
    {
        AisMsg m = dec({"!AIVDM,1,1,,A,91b77=h3h00nHt0Q3r@@07000<0b,0*69"});
        CHECK(m.valid && m.type == 9 && m.mmsi == 111265591 && m.altitudeM == 15 && near(m.sog, 0) && near(m.lon, 7128960 / 600000.0, 1e-6) && near(m.lat, 34667073 / 600000.0, 1e-6) &&
              m.second == 28 && m.cls == AIS_CLASS_SAR, "type 9");
        m = dec({"!AIVDM,1,1,,A,>5?Per18=HB1U:1@E=B0m<L,2*51"});
        CHECK(m.valid && m.type == 14 && m.mmsi == 351809000 && m.text == "RCVD YR TEST MSG", "type 14: '%s'", m.text.c_str());
        m = dec({"!AIVDM,1,1,,A,>3R1p10E3;;R0USCR0HO>0@gN10kGJp,2*7F"});
        CHECK(m.valid && m.mmsi == 237008900 && m.text == "EP228 IX48 FG3 DK7 PL56.", "type 14 b: '%s'", m.text.c_str());
        m = dec({"!AIVDM,1,1,,A,>4aDT81@E=@,2*2E"});
        CHECK(m.valid && m.mmsi == 311764000 && m.text == "TEST", "type 14 c: '%s'", m.text.c_str());
    }
    // ---- 18, 19
    {
        AisMsg m = dec({"!AIVDM,1,1,,A,B52K>;h00Fc>jpUlNV@ikwpUoP06,0*4C"});
        CHECK(m.valid && m.type == 18 && m.mmsi == 338087471 && near(m.sog, 0.1, 1e-3) && near(m.lon, -74.0721316667, 1e-6) && near(m.lat, 40.68454, 1e-6) &&
              near(m.cog, 79.6, 1e-3) && m.heading == -1 && m.second == 49 && m.cls == AIS_CLASS_B, "type 18");
        m = dec({"!AIVDM,1,1,,B,C5N3SRgPEnJGEBT>NhWAwwo862PaLELTBJ:V00000000S0D:R220,0*0B"});
        CHECK(m.valid && m.type == 19 && m.mmsi == 367059850 && near(m.sog, 8.7, 1e-3) && near(m.lon, -88.8103916667, 1e-6) && near(m.lat, 29.543695, 1e-6) &&
              near(m.cog, 335.9, 1e-3) && m.second == 46 && m.name == "CAPT.J.RIMES" && m.shipType == 70 && m.dimA == 5 && m.dimB == 21 && m.dimC == 4 && m.dimD == 4, "type 19: '%s'", m.name.c_str());
    }
    // ---- 21 with name extension
    {
        AisMsg m = dec({"!AIVDM,2,1,5,B,E1mg=5J1T4W0h97aRh6ba84<h2d;W:Te=eLvH50```q,0*46", "!AIVDM,2,2,5,B,:D44QDlp0C1DU00,2*36"}, false);
        CHECK(m.valid && m.type == 21 && m.mmsi == 123456789 && m.aidType == 20 && m.name == "CHINA ROSE MURPHY EXPRESS ALERT", "type 21 name: '%s'", m.name.c_str());
        CHECK(near(m.lon, -73619155 / 600000.0, 1e-6) && near(m.lat, 28752371 / 600000.0, 1e-6) && m.dimA == 5 && m.dimB == 5 && m.dimC == 5 && m.dimD == 5 && m.second == 50 &&
              !m.offPosition && !m.virtualAid && m.cls == AIS_CLASS_ATON, "type 21 fields");
    }
    // ---- 24 A and B
    {
        AisMsg a = dec({"!AIVDM,1,1,,A,H42O55i18tMET00000000000000,2*6D"});
        CHECK(a.valid && a.type == 24 && a.partNo == 0 && a.mmsi == 271041815 && a.name == "PROGUY", "type 24 A: '%s'", a.name.c_str());
        AisMsg b = dec({"!AIVDM,1,1,,A,H42O55lti4hhhilD3nink000?050,0*40"});
        CHECK(b.valid && b.partNo == 1 && b.mmsi == 271041815 && b.shipType == 60 && b.callsign == "TC6163" && b.dimA == 0 && b.dimB == 15 && b.dimC == 0 && b.dimD == 5 &&
              b.vendor == "1D0", "type 24 B: '%s' '%s'", b.callsign.c_str(), b.vendor.c_str());
        const Bits pb = fromSentences({"!AIVDM,1,1,,A,H42O55lti4hhhilD3nink000?050,0*40"});
        CHECK(getU(pb, 66, 4) == 12 && getU(pb, 70, 20) == 199796, "type 24 B model and serial");
    }
    // ---- 27, in the 96 bit and the 168 bit form
    {
        AisMsg m = dec({"!AIVDM,1,1,,A,KCQ9r=hrFUnH7P00,0*41"});
        CHECK(m.valid && m.type == 27 && m.mmsi == 236091959 && m.navStatus == 3 && near(m.lon, -92521 / 600.0, 1e-9) && near(m.lat, 52239 / 600.0, 1e-9) && m.bits == 96, "type 27 (96 bits)");
        m = dec({"!AIVDM,1,1,,B,KC5E2b@U19PFdLbMuc5=ROv62<7m,0*16"});
        CHECK(m.valid && m.type == 27 && m.mmsi == 206914217 && m.navStatus == 2 && near(m.lon, 82214 / 600.0, 1e-9) && near(m.lat, 2904 / 600.0, 1e-9) && near(m.sog, 57) && near(m.cog, 167), "type 27 (168 bits)");
    }
    // ---- the rest: unknown types keep their header, a wrong length is refused
    {
        AisMsg m;
        const Bits b = fromSentences({"!AIVDM,1,1,,B,:5MlU41GMK6@,0*6C"});    // type 10
        CHECK(!decodeMessage(b, m) && m.type == 10 && m.mmsi == 366814480, "type 10 is counted, not decoded");
        Bits s = fromSentences({"!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A"});
        s.resize(150);
        CHECK(!decodeMessage(s, m), "short type 1 refused");
    }
    // ---- built messages come back (the generator uses these routines)
    {
        Bits b; putU(b, 0, 6, 18); putU(b, 8, 30, 470123456); putS(b, 57, 28, (int32_t)(55.05 * 600000)); putS(b, 85, 27, (int32_t)(-25.0 * 600000));
        putU(b, 46, 10, 123); putU(b, 112, 12, 1234); putU(b, 124, 9, 511); putU(b, 133, 6, 17); b.resize(168, 0);
        AisMsg m;
        CHECK(decodeMessage(b, m) && m.mmsi == 470123456 && near(m.lon, 55.05, 1e-5) && near(m.lat, -25.0, 1e-5) && near(m.sog, 12.3, 1e-3) && near(m.cog, 123.4, 1e-3), "built type 18");
        Bits t; putText(t, 0, 120, "Jebel Ali 1"); CHECK(getText(t, 0, 120) == "JEBEL ALI 1", "text round trip");
    }
    if (fails) { printf("%d checks failed\n", fails); return 1; }
    printf("ais_proto: ok\n");
    return 0;
}
