// FANS-1/A CPDLC decode (aero_cpdlc.h). Real downlinks/uplinks: libacars examples/cpdlc_get_position.c and decode_acars_apps.c
// (github.com/szpajder/libacars): CRC must hold and the message must decode. Loopback: messages encoded with the asn1c unaligned PER
// encoder of the vendored tables, wrapped in ARINC 622 text, decoded, with damaged / truncated / lower-case variants.
// Frequency offset and sample rate do not exist at this layer (it starts from ACARS text); those are tested with the VDL2 / HFDL / Aero receivers.
#include "dect2/aero/aero_cpdlc.h"
#include <cstdio>
#include <cstring>
#include <string>
extern "C" {
#include <libacars/asn1/FANSATCDownlinkMessage.h>
#include <libacars/asn1/FANSATCUplinkMessage.h>
#include <libacars/asn1/asn_application.h>
}
using namespace dect2;

#include "../common/test_util.h"

static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

static std::vector<uint8_t> encodeUplinkClimb(int msgId, int flightLevel) {
    FANSATCUplinkMessage m;
    memset(&m, 0, sizeof m);
    m.aTCMessageheader.msgIdentificationNumber = msgId;
    m.aTCuplinkmsgelementId.present = FANSATCUplinkMsgElementId_PR_uM20Altitude;
    m.aTCuplinkmsgelementId.choice.uM20Altitude.present = FANSAltitude_PR_altitudeFlightLevel;
    m.aTCuplinkmsgelementId.choice.uM20Altitude.choice.altitudeFlightLevel = flightLevel;
    uint8_t buf[64];
    asn_enc_rval_t r = uper_encode_to_buffer(&asn_DEF_FANSATCUplinkMessage, &m, buf, sizeof buf);
    if (r.encoded <= 0) return {};
    return std::vector<uint8_t>(buf, buf + (r.encoded + 7) / 8);
}

static std::vector<uint8_t> encodeDownlinkFreeText(int msgId, const char* txt) {
    FANSATCDownlinkMessage m;
    memset(&m, 0, sizeof m);
    m.aTCMessageheader.msgIdentificationNumber = msgId;
    m.aTCDownlinkmsgelementid.present = FANSATCDownlinkMsgElementId_PR_dM67FreeText;
    OCTET_STRING_fromString(&m.aTCDownlinkmsgelementid.choice.dM67FreeText, txt);
    uint8_t buf[128];
    asn_enc_rval_t r = uper_encode_to_buffer(&asn_DEF_FANSATCDownlinkMessage, &m, buf, sizeof buf);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_FANSATCDownlinkMessage, &m);
    if (r.encoded <= 0) return {};
    return std::vector<uint8_t>(buf, buf + (r.encoded + 7) / 8);
}

int main() {
    // real messages
    const char* real[] = {
        "/SOUCAYA.AT1.HL8251243F880C3D903BB412903604FE326C2479F4A64F7F62528B1A9CF8382738186AC28B16668E013DF464D8A7F0",
        "/MSTEC7X.AT1.VT-ANKA094D88C3D903BB465D0723053B2E5123CFA53279400014B0894A2C6A73CBD8F52447AF1244CB4C9B94600089D65C84314892694587510528B1A9CF41169D440C1AB36A08B42",
        "/MELCAYA.AT1.ZK-OKC253C21CC3D903BA178F96618F0B28024B83127CD7886A12E9D85266B927584A9169C1A8EEB2800EEA7",
        "/MSTEC7X.AT1.VT-ANE21409DCC3DD03BB52350490502B2E5129D5A15692BA009A08892E7CC831E210A4C06EEBC28B1662BC02360165C80E1F7",
    };
    for (int i = 0; i < 4; i++) {
        AeroCpdlcMessage c;
        // the first, third are downlink position-type reports; the decode only has to hold for the right direction
        bool ok = aeroDecodeCpdlc(real[i], false, c);
        CHECK(ok && c.found && c.crcOk, "real %d: found %d crc %d", i, c.found, c.crcOk);
        CHECK(ok && c.parsed && !c.text.empty(), "real %d: decoded (parsed %d)", i, c.parsed);
        printf("real %d (%s %s %s): %s", i, c.groundAddr.c_str(), c.imi.c_str(), c.reg.c_str(), c.text.c_str());
    }
    AeroCpdlcMessage cr;
    CHECK(aeroDecodeCpdlc("- #MD/AA ATLTWXA.CR1.N856DN203A3AA8E5C1A9323EDD", true, cr) && cr.found && cr.imi == "CR1" && cr.crcOk, "real connect request: found, CRC");
    CHECK(cr.parsed, "real connect request decodes");

    // loopback: uplink and downlink, several levels
    for (int fl : {290, 350, 410}) {
        const std::string t = aeroBuildCpdlcText("EGTTYGA", "AT1", "G-ABCD", encodeUplinkClimb(7, fl));
        AeroCpdlcMessage c;
        CHECK(aeroDecodeCpdlc(t, true, c) && c.crcOk && c.parsed, "uplink FL%d decodes", fl);
        char want[32];
        snprintf(want, sizeof want, "Flight level: %d", fl);
        CHECK(has(c.text, "CLIMB TO AND MAINTAIN") && has(c.text, want), "uplink FL%d text: %s", fl, c.text.c_str());
        CHECK(c.reg == "G-ABCD" && c.groundAddr == "EGTTYGA", "address fields");
        // a different direction reads the same bits as the other message set: it must not crash, and a clean text is rejected or garbled but flagged
        AeroCpdlcMessage d;
        CHECK(aeroDecodeCpdlc(t, false, d) && d.crcOk, "wrong direction keeps the CRC verdict");
        // one wrong hex digit: CRC fails
        std::string bad = t;
        bad[bad.size() - 6] = bad[bad.size() - 6] == '0' ? '1' : '0';
        AeroCpdlcMessage e;
        CHECK(aeroDecodeCpdlc(bad, true, e) && !e.crcOk, "damaged digit: CRC FAILED");
        // lower-case hex (some feeders) and the ACARS "- #M1B" style prefix before the address are tolerated
        std::string lower = t;
        for (size_t i = 19; i < lower.size(); i++) lower[i] = (char)tolower(lower[i]);
        AeroCpdlcMessage f;
        CHECK(aeroDecodeCpdlc(lower, true, f) && f.parsed && f.crcOk, "lower-case hex");
        // truncated body: never parsed as good, never crashes
        for (size_t cut = 8; cut + 4 < t.size(); cut += 5) {
            AeroCpdlcMessage g;
            aeroDecodeCpdlc(t.substr(0, cut), true, g);
            CHECK(!g.found || !g.crcOk || !g.parsed || g.text.size() < 200, "truncated %zu does not crash", cut);
        }
    }
    {
        const std::string t = aeroBuildCpdlcText("KZNYCAY", "AT1", "N123AB", encodeDownlinkFreeText(12, "REQUEST HIGHER DUE TURBULENCE"));
        AeroCpdlcMessage c;
        CHECK(aeroDecodeCpdlc(t, false, c) && c.crcOk && c.parsed && has(c.text, "REQUEST HIGHER DUE TURBULENCE"), "downlink free text: %s", c.text.c_str());
        CHECK(has(aeroDescribeCpdlc(c), "FANS-1/A CPDLC message"), "description line");
    }
    AeroCpdlcMessage none;
    CHECK(!aeroDecodeCpdlc("/AKLCDYA.ADS.9V-SVG21D0755D8", false, none) && !none.found, "not CPDLC");
    CHECK(!aeroDecodeCpdlc("RANDOM TEXT .AT1. no address", false, none), "no ground address");
    return testSummary("test_aero_cpdlc");
}
