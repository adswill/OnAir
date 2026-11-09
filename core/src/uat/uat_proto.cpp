// UAT protocol layer: wraps dump978 uat_decode and maps into UatTelemetry types.
#include "dect2/uat/uat_proto.h"
#include "dect2/uat/uat_twgo.h"
#include "dect2/adsb/adsb_msg.h"
#include "dect2/uat/uat_decode_c.h"
#include "dect2/uat/uat_frame.h"
#include <cstdio>
#include <cstring>
#include "dect2/common/str.h"

namespace dect2 {

namespace {

std::string hexOf(const uint8_t* d, int n) { return dect2::str::toHex(d, n > 0 ? (size_t)n : 0); }

void fillAcFromMdb(const uat_adsb_mdb& m, UatAircraft& ac) {
    ac.icao = m.address & 0xFFFFFF;
    if (m.callsign_type == CS_CALLSIGN && m.callsign[0]) ac.callsign = m.callsign;
    if (m.callsign_type == CS_SQUAWK) { ac.hasSquawk = true; ac.squawk = atoi(m.callsign); }
    if (m.position_valid) { ac.hasPos = true; ac.lat = m.lat; ac.lon = m.lon; }
    if (m.altitude_type == ALT_BARO) { ac.hasAlt = true; ac.altFt = m.altitude; }
    if (m.altitude_type == ALT_GEO) { ac.hasGeoAlt = true; ac.geoAltFt = m.altitude; }
    if (m.speed_valid) { ac.hasSpeed = true; ac.speedKt = m.speed; }
    if (m.track_type != TT_INVALID) { ac.hasHeading = true; ac.headingDeg = (float)m.track; }
    if (m.vert_rate) { ac.hasVrate = true; ac.vrateFpm = m.vert_rate; }
    ac.ground = m.airground_state == AG_GROUND;
    ac.nic = m.nic;
    ac.tisb = m.address_qualifier == AQ_TISB_ICAO || m.address_qualifier == AQ_TISB_OTHER;
}

std::string fisbTextFromApdu(const fisb_apdu& ap) {
    if (!ap.data || ap.length == 0) return {};
    std::string t((const char*)ap.data, (const char*)ap.data + ap.length);
    while (!t.empty() && (t.back() == '\r' || t.back() == '\n' || t.back() == ' ')) t.pop_back();
    return t;
}

} // namespace

std::string uatFisbProductName(uint16_t productId) {
    switch (productId) {
    case 0: return "METAR";
    case 1: return "TAF";
    case 2: return "NEXRAD";
    case 8: return "AIRMET";
    case 10: return "SIGMET";
    case 12: return "GAIRMET";
    case 413: return "Winds aloft";
    default: {
        char b[32];
        snprintf(b, sizeof b, "FIS-B %u", productId);
        return b;
    }
    }
}

bool uatDecodeFrame(const UatRawFrame& raw, UatDecoded& out) {
    out = UatDecoded();
    out.hex = hexOf(raw.data, raw.dataBytes);
    uint8_t frame[UAT_UPLINK_FRAME_BYTES];
    std::memcpy(frame, raw.data, (size_t)raw.dataBytes);
    if (raw.uplink) {
        uat_uplink_mdb mdb;
        memset(&mdb, 0, sizeof mdb);
        uat_decode_uplink_mdb(frame, &mdb);
        if (!mdb.app_data_valid) {
            out.kind = UatDecoded::Kind::Other;
            out.summary = "UAT uplink (no application data)";
            return true;
        }
        for (unsigned i = 0; i < mdb.num_info_frames; i++) {
            const auto& fr = mdb.info_frames[i];
            if (!fr.is_fisb) continue;
            if (fr.fisb.product_id == 63 || fr.fisb.product_id == 64) uatNexradDecode((uint16_t)fr.fisb.product_id, fr.fisb.data, (size_t)fr.fisb.length, out.nexrad);
            if (out.kind == UatDecoded::Kind::Fisb) continue;   // the text of the first product stands for the frame
            out.kind = UatDecoded::Kind::Fisb;
            out.fisb.productId = fr.fisb.product_id;
            out.fisb.title = uatFisbProductName(fr.fisb.product_id);
            out.fisb.text = fisbTextFromApdu(fr.fisb);
            if (fr.fisb.product_id >= 8 && fr.fisb.product_id <= 16) {
                std::string tw;
                if (uatTwgoDecode((uint16_t)fr.fisb.product_id, fr.fisb.data, (size_t)fr.fisb.length, tw, out.overlays)) out.fisb.text = tw;
            }
            out.fisb.timeSec = raw.timeSec;
            out.summary = out.fisb.title;
            if (!out.fisb.text.empty()) out.summary += ": " + out.fisb.text.substr(0, 120);
        }
        if (out.kind == UatDecoded::Kind::Fisb) return true;
        out.kind = UatDecoded::Kind::Other;
        out.summary = "UAT uplink application data";
        return true;
    }

    const bool shortFrame = (frame[0] >> 3) == 0;
    const int mdbType = (frame[0] >> 3) & 0x1f;
    if (mdbType == 0 || mdbType == 1) {
        uat_adsb_mdb mdb;
        memset(&mdb, 0, sizeof mdb);
        uat_decode_adsb_mdb(frame, &mdb);
        fillAcFromMdb(mdb, out.ac);
        out.kind = out.ac.tisb ? UatDecoded::Kind::Tisb : UatDecoded::Kind::Adsb;
        char b[160];
        if (out.ac.hasPos && out.ac.hasAlt)
            snprintf(b, sizeof b, "%s %06X %.4f %.4f %d ft", out.ac.tisb ? "TIS-B" : "UAT ADS-B", out.ac.icao, out.ac.lat, out.ac.lon, out.ac.altFt);
        else if (!out.ac.callsign.empty())
            snprintf(b, sizeof b, "%s %06X %s", out.ac.tisb ? "TIS-B" : "UAT ADS-B", out.ac.icao, out.ac.callsign.c_str());
        else
            snprintf(b, sizeof b, "%s %06X type %d", out.ac.tisb ? "TIS-B" : "UAT ADS-B", out.ac.icao, mdbType);
        out.summary = b;
        return true;
    }

    out.kind = UatDecoded::Kind::Other;
    char b[80];
    snprintf(b, sizeof b, "UAT MDB type %d (%s frame)", mdbType, shortFrame ? "short" : "long");
    out.summary = b;
    return true;
}

} // namespace dect2
