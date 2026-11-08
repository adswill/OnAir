// Wi-SUN FAN / 802.15.4g SUN FSK frame layers against values that do not come from our own encoder: the CRC-32 check value, and two
// PSDUs received off air from a Wi-SUN FAN 1.1 network near Sydney (915 - 928 MHz, 150 kbit/s FSK), octets as sent (whitened), FCS included.
// Then frames built here and read back (the PAN advert and EAPOL fields no off-air frame of ours carries in the clear).
#include "dect2/wisun/wisun_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2::wisun;
static int fails = 0;
// The message is glued to a literal: a format that is not a string literal does not compile
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

static std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> v;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) { unsigned x; sscanf(s + i, "%2x", &x); v.push_back((uint8_t)x); }
    return v;
}

// A PAN configuration frame (PHR 0x0883: whitened, 131 octets, 4-octet FCS) on 916.2 MHz and a broadcast data frame (PHR 0x0889) on 927.0 MHz
static const char* kPc =
    "f9edcdf68f14998bc228c3a0c58465acbe17e9a307df08e64a65c45d8f5cb998a9cc7ac996390a406e4531182a86f0efef32f90ca860b6cfcf4510891bc81627b120570c23"
    "bb1f1d4e83bacb815cbd61eba5b47f188b7eb6dfd03681d2eb56e946ccd2225901186a6bba0578a3bfa3c6827f01aa3b0e18e3df3dabb5cd67b6a2588a9f";
static const char* kData =
    "f9ec98f6c271daf5c316dd67d3252197b516edb313dcc38be045d74a18f6c58ea9c96f76a579361aac73e451a2b8ea13079eea128b02e51cac8458a1e8936c8581d134f9df"
    "02ede15b40bf03fc29a0c977e6af2f1a9f704a136b175d37e4b817d431970cabafe864dbe073a4e30bbbefc824c9f7f25abe28b69b6407d76914d25fe2e37ee7a3261fca";

int main() {
    CHECK(crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u, "crc32 check value 0x%08X", crc32((const uint8_t*)"123456789", 9));
    {   // the whitening sequence (all zero octets come out as the sequence)
        uint8_t z[8] = {};
        whiten(z, 8);
        static const uint8_t want[8] = {0xF0, 0x0E, 0xCD, 0xF6, 0xC2, 0x19, 0x12, 0x75};
        CHECK(memcmp(z, want, 8) == 0, "pn9 %02x %02x %02x %02x", z[0], z[1], z[2], z[3]);
    }
    {
        const Phr p = parsePhr(0x0883);
        CHECK(!p.modeSwitch && !p.fcs2 && p.whitened && p.length == 131, "phr");
        CHECK(buildPhr(p) == 0x0883, "phr round trip");
    }
    {   // the PAN configuration frame
        std::vector<uint8_t> d = hex(kPc);
        CHECK(d.size() == 131, "length %zu", d.size());
        CHECK(!fcsOk(d.data(), d.size(), false), "the FCS of the whitened octets must not pass");
        whiten(d.data(), d.size());
        CHECK(fcsOk(d.data(), d.size(), false), "fcs");
        const Frame f = parse(d.data(), d.size() - 4);
        CHECK(f.ok && f.type == Data && f.version == 2 && f.secured && f.seqSuppressed && f.ieList, "header");
        CHECK(f.destMode == 0 && f.srcMode == 3 && f.hasSrcPan && f.srcPan == 0x0000, "addressing");
        CHECK(f.srcExt == 0x18DFC1FFFE8B0D4Dull && euiText(f.srcExt) == "18:DF:C1:FF:FE:8B:0D:4D", "source %s", euiText(f.srcExt).c_str());
        CHECK(f.secLevel == 6 && f.keyIdMode == 1 && f.keyIndex == 1 && f.frameCounter == 417967, "security %d %d %d %u", f.secLevel, f.keyIdMode, f.keyIndex, f.frameCounter);
        CHECK(f.utt == PC && f.ufsi == 0xE655CF, "utt %d %06X", f.utt, f.ufsi);
        CHECK(f.hasBt && f.bsn == 0x3995 && f.bio == 0xB5, "bt %d %u", f.bsn, f.bio);
        CHECK(f.hasLbc && f.lfnBcastMs == 60000, "lbc %u", f.lfnBcastMs);
        CHECK(f.ies == "UTT BT LBC HT1", "ies '%s'", f.ies.c_str());
        CHECK(f.encrypted && f.payloadLen == 85, "encrypted %zu", f.payloadLen);
        CHECK(f.info == "PAN Config, encrypted (key 1)", "info '%s'", f.info.c_str());
    }
    {   // the broadcast data frame
        std::vector<uint8_t> d = hex(kData);
        whiten(d.data(), d.size());
        CHECK(d.size() == 137 && fcsOk(d.data(), d.size(), false), "fcs");
        const Frame f = parse(d.data(), d.size() - 4);
        CHECK(f.ok && !f.seqSuppressed && f.seq == 0x55, "seq %d", f.seq);
        CHECK(f.srcExt == 0x18DFC1FFFE80C868ull, "source %s", euiText(f.srcExt).c_str());
        CHECK(f.utt == DataFrame && f.frameCounter == 736548 && f.hasBt && f.bsn == 0xC993, "data %d %u %d", f.utt, f.frameCounter, f.bsn);
        CHECK(dstText(f) == "broadcast", "dst %s", dstText(f).c_str());
    }
    {   // the ANZ channel plans
        const auto p = plansOf("ANZ");
        CHECK(p.size() == 2 && p[1].opClass == 2, "plans");
        CHECK(channelOf(p[1], 916.2) == 2 && channelOf(p[1], 927.0) == 29 && channelOf(p[1], 914.0) == -1, "channels");
        CHECK(std::abs(channelMhz(p[0], 63) - 927.8) < 1e-9, "last 200 kHz channel");
    }
    {   // the band plans against the Wi-SUN frequency band table: band, regulatory domain, spacings and channel counts; every channel
        // centre inside the band. Brazil's counts (90, 43, 27) come from a raster the table does not give: with the North American one and
        // no channel overlapping 907.5 - 915 MHz the counts land within 2 of them.
        struct Row { const char* id; int reg; double lo, hi; std::vector<std::pair<int, int>> sp; };
        const std::vector<Row> table = {
            {"CN470", 0x04, 470, 510, {{200, 199}}}, {"CN779", 0x04, 779, 787, {{200, 39}, {400, 19}}}, {"CN920", 0x04, 920.5, 924.5, {{250, 16}}},
            {"EU863", 0x03, 863, 870, {{100, 69}, {200, 35}}}, {"EU870", 0x03, 870, 876, {{100, 59}, {200, 30}}}, {"IN", 0x05, 865, 867, {{100, 19}, {200, 10}}},
            {"SG866", 0x10, 866, 869, {{100, 29}, {200, 15}, {400, 7}}}, {"MX", 0x06, 902, 928, {{200, 129}, {400, 64}}},
            {"NA", 0x01, 902, 928, {{200, 129}, {400, 64}, {600, 42}}}, {"BZ", 0x07, 902, 928, {{200, 90}, {400, 43}, {600, 27}}},
            {"ANZ", 0x08, 915, 928, {{200, 64}, {400, 32}}}, {"KR", 0x09, 917, 923.5, {{200, 32}, {400, 16}}}, {"PH", 0x0A, 915, 918, {{200, 14}, {400, 7}}},
            {"MY", 0x0B, 919, 923, {{200, 19}, {400, 10}}}, {"HK", 0x0C, 920, 925, {{200, 24}, {400, 12}}}, {"SG", 0x0D, 920, 925, {{200, 24}, {400, 12}}},
            {"TH", 0x0E, 920, 925, {{200, 24}, {400, 12}}}, {"VN", 0x0F, 920, 925, {{200, 24}, {400, 12}}},
            {"JP", 0x02, 920, 928, {{200, 38}, {400, 18}, {600, 12}}}, {"WW", 0x00, 2400, 2483.5, {{200, 416}, {400, 207}}},
        };
        CHECK(bandPlans().size() == table.size(), "%zu band plans", bandPlans().size());
        for (const Row& r : table) {
            const BandPlan* b = bandPlan(r.id);
            CHECK(b && b->regDomain == r.reg && b->loMhz == r.lo && b->hiMhz == r.hi, "%s band", r.id);
            const auto plans = plansOf(r.id);
            CHECK(plans.size() == r.sp.size(), "%s: %zu plans", r.id, plans.size());
            for (size_t i = 0; i < plans.size() && i < r.sp.size(); i++) {
                const ChannelPlan& p = plans[i];
                int n = 0;
                double first = 1e9, last = 0;
                for (int k = 0; k < p.channels; k++)
                    if (channelUsable(p, k)) { n++; first = std::min(first, channelMhz(p, k)); last = std::max(last, channelMhz(p, k)); }
                const bool brazil = std::string(r.id) == "BZ";
                CHECK(p.spacingKhz == r.sp[i].first && (brazil ? std::abs(n - r.sp[i].second) <= 2 : n == r.sp[i].second), "%s %g kHz: %d channels", r.id, p.spacingKhz, n);
                CHECK(first >= r.lo - 1e-9 && last <= r.hi + 1e-9, "%s %g kHz: %.3f - %.3f MHz outside the band", r.id, p.spacingKhz, first, last);
            }
        }
        CHECK(std::string(bandPlanFor(921.5).id) == "ANZ" && std::string(bandPlanFor(906).id) == "NA" && std::string(bandPlanFor(868).id) == "EU863" &&
              std::string(bandPlanFor(2440).id) == "WW", "automatic choice");
        const ChannelPlan bz = plansOf("BZ")[0];
        CHECK(channelOf(bz, 910.0) == -1 && channelOf(bz, 906.0) >= 0 && channelOf(bz, 916.0) >= 0, "Brazil's gap");
        CHECK(std::string(regDomainName(0x10)) == "Singapore (866 MHz)", "domain 0x10");
    }
    {   // a PAN advertisement, built and read back: the payload IEs in the clear
        Build b;
        b.utt = PA; b.src = 0x0007810000ABCDEFull; b.srcPanPresent = true; b.pan = 0x7A31; b.ufsi = 0x123456;
        b.us.present = true; b.us.dwellMs = 255; b.us.chPlan = 0; b.us.regDomain = 0x08; b.us.opClass = 2; b.us.chFunction = 2;
        b.panIe = true; b.panSize = 412; b.routingCost = 256; b.tpsVersion = 2;
        b.netName = "Example-FAN"; b.phyModes = {0x03, 0x05};
        std::vector<uint8_t> m = buildMpdu(b);
        addFcs(m, false);
        CHECK(fcsOk(m.data(), m.size(), false), "built fcs");
        const Frame f = parse(m.data(), m.size() - 4);
        CHECK(f.ok && !f.secured && f.utt == PA && f.hasSrcPan && f.srcPan == 0x7A31, "pa header");
        CHECK(f.us.present && f.us.dwellMs == 255 && f.us.regDomain == 8 && f.us.opClass == 2 && f.us.chFunction == 2, "us-ie");
        CHECK(f.hasPanIe && f.panSize == 412 && f.routingCost == 256 && f.tpsVersion == 2 && f.useParentBs && f.rplRouting, "pan-ie");
        CHECK(f.hasNetName && f.netName == "Example-FAN", "netname '%s'", f.netName.c_str());
        CHECK(f.phyModes.size() == 2 && f.phyModes[1] == 5, "pom");
        CHECK(f.ies == "UTT HT1 US PAN NETNAME POM", "ies '%s'", f.ies.c_str());
        CHECK(f.info == "PAN Advert \"Example-FAN\", PAN size 412, routing cost 256", "info '%s'", f.info.c_str());
        CHECK(scheduleText(f.us) == "Australia / NZ class 2, DH1CF, dwell 255 ms", "schedule '%s'", scheduleText(f.us).c_str());
    }
    {   // a joining node's EAPOL frame: the authenticator in the EA-IE, an EAP-TLS response through the MPX IE (KMP 1)
        Build b;
        b.utt = Eapol; b.src = 0x0007810000ABCDEFull; b.dst = 0x18DFC1FFFE8B0D4Dull; b.ea = 0x18DFC1FFFE80C868ull; b.seq = 9;
        b.mpxId = 0x0001; b.kmpId = 1;
        b.payload = {0x03, 0x00, 0x00, 0x06, 0x02, 0x17, 0x00, 0x06, 0x0D, 0x00};   // EAPOL v3, EAP packet; EAP response, TLS
        std::vector<uint8_t> m = buildMpdu(b);
        const Frame f = parse(m.data(), m.size());
        CHECK(f.ok && f.utt == Eapol && f.seq == 9 && f.destExt == b.dst && !f.hasDestPan && !f.hasSrcPan, "eapol header");
        CHECK(f.eaEui == b.ea && f.kmpId == 1 && f.eapolType == 0 && f.eapCode == 2 && f.eapType == 13, "eapol %d %d %d", f.kmpId, f.eapCode, f.eapType);
        CHECK(f.info.find("EAP response TLS") != std::string::npos && f.info.find("authenticator 18:DF:C1:FF:FE:80:C8:68") != std::string::npos,
              "info '%s'", f.info.c_str());
    }
    {   // a damaged frame: too short for the addresses
        const uint8_t d[] = {0x09, 0xE3, 0x00, 0x00, 0x4D};
        CHECK(!parse(d, sizeof d).ok, "short frame");
    }
    if (fails) printf("wisun proto: %d FAILED\n", fails);
    else printf("wisun proto: all passed\n");
    return fails ? 1 : 0;
}
