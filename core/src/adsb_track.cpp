// ADS-B frame validation and the aircraft table.
#include "dect2/adsb_track.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>

namespace dect2 {

using namespace adsb;

static const size_t kMaxRecords = 2000;     // bound on the table, whatever the band carries
static const size_t kMaxListed = 128;       // aircraft in one telemetry report
static const size_t kMaxFrames = 64;
static const size_t kMaxTrack = 30;
static const double kMaxSpeedKt = 1800;     // faster than this between two positions is a jump, not a flight
static const double kReplyLevelDb = 25.0;   // an address / parity reply is refused when its level differs from the aircraft's mean level by more than this
static const int kReplyAltFt = 5000;        // ... or its altitude from the last ADS-B altitude (up to 30 s old) by more than this

AdsbTracker::AdsbTracker() {}

void AdsbTracker::reset() {
    rec_.clear();
    frames_.clear();
    good_ = bad_ = corrected_ = 0;
    std::memset(dfCount_, 0, sizeof dfCount_);
    window_ = 0;
    std::memset(windows_, 0, sizeof windows_);
    maxRange_ = 0;
    now_ = 0;
    snrEma_ = levelEma_ = 0; snrInit_ = false;
}

void AdsbTracker::setReference(double lat, double lon) { refLat_ = lat; refLon_ = lon; refValid_ = true; }
void AdsbTracker::clearReference() { refValid_ = false; }

void AdsbTracker::closeWindow() {
    for (int i = 0; i < 3; i++) windows_[i] = windows_[i + 1];
    windows_[3] = window_;
    window_ = 0;
}

bool AdsbTracker::knownAddress(uint32_t icao, double now) const {
    auto it = rec_.find(icao);
    return it != rec_.end() && known(&it->second, now);
}

AdsbTracker::Rec& AdsbTracker::touch(uint32_t key, double now) {
    auto it = rec_.find(key);
    if (it == rec_.end()) {
        if (rec_.size() >= kMaxRecords) {   // drop the one that has been silent longest
            auto oldest = rec_.begin();
            for (auto i = rec_.begin(); i != rec_.end(); ++i) if (i->second.lastSeen < oldest->second.lastSeen) oldest = i;
            rec_.erase(oldest);
        }
        it = rec_.emplace(key, Rec()).first;
        touchedNew_ = true;
        it->second.a.icao = key;
        it->second.firstSeen = now;
        it->second.level = 0;
    }
    return it->second;
}

// ---------------------------------------------------------------- the checks

static uint32_t bitsAt(const AdsbRaw& r, int first, int n) { return getBits(r.bytes, first, n); }

// A single flipped bit, or two among the least certain ones, that turns the frame into a DF17 / 18 (long) or DF11 (short) with a known address.
bool AdsbTracker::tryCorrect(AdsbRaw& raw, int len, uint32_t, double now) {
    if (opt_.fixBits <= 0) return false;
    for (int L : {112, 56}) {
        if (len != 0 && len != L) continue;
        const uint32_t rem = crc24(raw.bytes, L);
        if (rem == 0) continue;
        auto fits = [&](const uint8_t* b, int& df, uint32_t& icao) {
            df = (int)getBits(b, 1, 5);
            if (L == 112 && df != 17 && df != 18) return false;
            if (L == 56 && df != 11) return false;
            icao = getBits(b, 9, 24);
            return opt_.fixUnknown || knownAddress(icao, now);
        };
        if (L == 56 && rem < 128) continue;   // an interrogator code, not an error
        int pos = singleBitPosition(L, rem);
        if (pos >= 0) {
            uint8_t t[14];
            std::memcpy(t, raw.bytes, 14);
            t[pos >> 3] ^= (uint8_t)(0x80 >> (pos & 7));
            int df; uint32_t icao;
            if (fits(t, df, icao)) { std::memcpy(raw.bytes, t, 14); raw.bits = L; return true; }
        }
        if (opt_.fixBits >= 2) {
            // the eight least certain bits: 28 pairs, each tested against the remainder
            int idx[112];
            std::iota(idx, idx + L, 0);
            const int K = 8;
            std::partial_sort(idx, idx + K, idx + L, [&](int a, int b) { return raw.conf[a] < raw.conf[b]; });
            uint32_t syn[8];
            for (int k = 0; k < K; k++) syn[k] = singleBitSyndrome(L, idx[k]);
            for (int a = 0; a < K; a++) for (int b = a + 1; b < K; b++) {
                if ((syn[a] ^ syn[b]) != rem) continue;
                uint8_t t[14];
                std::memcpy(t, raw.bytes, 14);
                t[idx[a] >> 3] ^= (uint8_t)(0x80 >> (idx[a] & 7));
                t[idx[b] >> 3] ^= (uint8_t)(0x80 >> (idx[b] & 7));
                int df; uint32_t icao;
                if (fits(t, df, icao)) { std::memcpy(raw.bytes, t, 14); raw.bits = L; return true; }
            }
        }
    }
    return false;
}

// A reply whose address is only in its parity matches a known aircraft by chance about once in 2^24 / (aircraft heard) tries, and a noisy band gives
// many tries (every candidate that fails is sliced several times). What such a false reply would say has to fit what the aircraft is known to do.
bool AdsbTracker::plausibleReply(const AdsbRaw& raw, int df, int len, uint32_t icao) const {
    auto it = rec_.find(icao);
    if (it == rec_.end()) return false;
    const Rec& r = it->second;
    if ((df == 4 || df == 5 || df == 20 || df == 21) && bitsAt(raw, 6, 3) > 5) return false;      // flight status 6 and 7 are not assigned
    if (std::fabs(raw.levelDbfs - r.a.levelDbfs) > kReplyLevelDb) return false;                    // the same transmitter, so about the same level
    if ((df == 0 || df == 4 || df == 16 || df == 20) && len >= 56 && raw.timeSec - r.altTime <= 30.0) {
        int alt = 0;
        if (decodeAc13(bitsAt(raw, 20, 13), alt) && std::abs(alt - r.trustedAltFt) > kReplyAltFt) return false;   // 5000 ft in 30 s is not a flight
    }
    return true;
}

bool AdsbTracker::accept(AdsbRaw& raw, AdsbFrame& out, bool countBad) {
    const double now = raw.timeSec;
    now_ = std::max(now_, now);
    const uint8_t original[14] = {raw.bytes[0], raw.bytes[1], raw.bytes[2], raw.bytes[3], raw.bytes[4], raw.bytes[5], raw.bytes[6], raw.bytes[7],
                                  raw.bytes[8], raw.bytes[9], raw.bytes[10], raw.bytes[11], raw.bytes[12], raw.bytes[13]};
    int df = (int)bitsAt(raw, 1, 5);
    int len = dfLength(df);
    bool ok = false, trusted = false;
    uint32_t icao = 0;
    int fixed = 0;
    if (len) {
        const uint32_t rem = crc24(raw.bytes, len);
        if (df == 17 || df == 18 || df == 19) {
            if (rem == 0) { ok = trusted = true; icao = bitsAt(raw, 9, 24); }
        } else if (df == 11) {
            icao = bitsAt(raw, 9, 24);
            if (rem == 0) ok = trusted = true;
            else if (rem < 128 && knownAddress(icao, now)) ok = true;      // reply to a ground interrogator: the remainder is its code
        } else {
            icao = rem;                                                    // address / parity: the parity carries the address
            ok = knownAddress(icao, now) && plausibleReply(raw, df, len, icao);
        }
        raw.bits = len;
    }
    if (!ok && tryCorrect(raw, len, 0, now)) {
        ok = trusted = true;
        df = (int)bitsAt(raw, 1, 5);
        len = raw.bits;
        icao = bitsAt(raw, 9, 24);
        for (int i = 0; i < 14; i++) fixed += __builtin_popcount(raw.bytes[i] ^ original[i]);
    }
    if (!ok) { if (countBad) bad_++; return false; }

    out = AdsbFrame();
    std::memcpy(out.bytes, raw.bytes, 14);
    out.bits = len;
    out.df = df;
    out.icao = icao;
    out.corrected = fixed;
    out.levelDbfs = raw.levelDbfs;
    out.snrDb = raw.snrDb;
    out.timeSec = now;
    decodeMsg(out.bytes, len, out.msg);
    out.msg.icao = icao;
    touchedNew_ = false;
    apply(out);
    out.newAircraft = touchedNew_;
    return true;
}

// ---------------------------------------------------------------- the table

void AdsbTracker::apply(const AdsbFrame& f) {
    const Msg& m = f.msg;
    const bool nonIcao = f.df == 18 && (m.ca == 1 || m.ca == 5);
    const uint32_t key = f.icao | (nonIcao ? 0x1000000u : 0u);
    Rec& r = touch(key, f.timeSec);
    if (f.bits == 112 ? (f.df == 17 || f.df == 18 || f.df == 19) : f.df == 11) r.lastTrusted = f.timeSec;
    applyMsg(r, f);

    good_++;
    window_++;
    if (f.corrected) corrected_++;
    dfCount_[f.df & 31]++;
    now_ = std::max(now_, f.timeSec);
    const double snr = f.snrDb;
    if (!snrInit_) { snrEma_ = snr; levelEma_ = f.levelDbfs; snrInit_ = true; }
    else { snrEma_ += 0.05 * (snr - snrEma_); levelEma_ += 0.05 * (f.levelDbfs - levelEma_); }

    AdsbFrameInfo fi;
    fi.hex = toHex(f.bytes, f.bits);
    fi.icao = key;
    fi.df = f.df;
    fi.tc = m.tc;
    fi.corrected = f.corrected;
    fi.timeSec = f.timeSec;
    fi.levelDbfs = f.levelDbfs;
    fi.what = adsbDescribe(m);
    frames_.push_back(std::move(fi));
    if (frames_.size() > kMaxFrames) frames_.pop_front();
}

void AdsbTracker::commitPosition(Rec& r, double lat, double lon, int kind, double now) {
    AdsbAircraft& a = r.a;
    a.hasPos = true; a.lat = lat; a.lon = lon;
    if (kind == 3 && a.posKind == 1) kind = 1;       // a chain of local decodes from an unconfirmed fix stays unconfirmed
    a.posKind = kind;
    r.lastPos = now;
    r.rejects = 0;
    if (r.lastTrack < 0 || now - r.lastTrack >= 5.0) {
        a.track.push_back({(float)lat, (float)lon});
        if (a.track.size() > kMaxTrack) a.track.erase(a.track.begin());
        r.lastTrack = now;
    }
    if (refValid_ && kind >= 2) maxRange_ = std::max(maxRange_, (float)distanceNm(refLat_, refLon_, lat, lon));
}

void AdsbTracker::airbornePosition(Rec& r, const Msg& m, double now) {
    AdsbAircraft& a = r.a;
    const int p = m.cprOdd ? 1 : 0;
    r.cpr[p].lat = m.cprLat; r.cpr[p].lon = m.cprLon; r.cpr[p].t = now; r.cpr[p].valid = true;
    double lat = 0, lon = 0;
    int kind = 0;
    if (r.cpr[1 - p].valid && std::fabs(now - r.cpr[1 - p].t) <= 10.0 &&
        cprGlobalAirborne(r.cpr[0].lat, r.cpr[0].lon, r.cpr[1].lat, r.cpr[1].lon, p == 1, lat, lon)) kind = 2;
    else if (a.hasPos && now - r.lastPos <= 30.0 && cprLocalAirborne(m.cprLat, m.cprLon, p == 1, a.lat, a.lon, lat, lon)) kind = 3;
    else if (!a.hasPos && refValid_ && cprLocalAirborne(m.cprLat, m.cprLon, p == 1, refLat_, refLon_, lat, lon)) kind = 1;
    if (!kind) return;
    // a position that jumps farther than an aircraft can fly is dropped; the third one in a row is believed (the first was the wrong one)
    if (a.hasPos && a.posKind >= 2 && now - r.lastPos < 60.0) {
        const double d = distanceNm(a.lat, a.lon, lat, lon);
        if (d > kMaxSpeedKt * std::max(now - r.lastPos, 0.0) / 3600.0 + 2.0 && ++r.rejects < 3) return;
    }
    commitPosition(r, lat, lon, kind, now);
}

void AdsbTracker::surfacePosition(Rec& r, const Msg& m, double now) {
    AdsbAircraft& a = r.a;
    const int p = m.cprOdd ? 1 : 0;
    r.scpr[p].lat = m.cprLat; r.scpr[p].lon = m.cprLon; r.scpr[p].t = now; r.scpr[p].valid = true;
    // the surface zones repeat every 90 degrees: a position needs a reference, the aircraft's own last fix or the receiver's
    double refLat, refLon;
    if (a.hasPos) { refLat = a.lat; refLon = a.lon; }
    else if (refValid_) { refLat = refLat_; refLon = refLon_; }
    else return;
    double lat = 0, lon = 0;
    int kind = 0;
    if (r.scpr[1 - p].valid && std::fabs(now - r.scpr[1 - p].t) <= 25.0 &&
        cprGlobalSurface(r.scpr[0].lat, r.scpr[0].lon, r.scpr[1].lat, r.scpr[1].lon, p == 1, refLat, refLon, lat, lon)) kind = a.hasPos ? 3 : 1;
    else if (cprLocalSurface(m.cprLat, m.cprLon, p == 1, refLat, refLon, lat, lon)) kind = a.hasPos ? 3 : 1;
    if (!kind) return;
    // the receiver's reference only has to be within about 45 NM for this to be right: the position stays marked as unconfirmed
    commitPosition(r, lat, lon, kind, now);
}

void AdsbTracker::applyMsg(Rec& r, const AdsbFrame& f) {
    const Msg& m = f.msg;
    AdsbAircraft& a = r.a;
    const double now = f.timeSec;
    a.messages++;
    r.lastSeen = now;
    const double p = std::pow(10.0, f.levelDbfs / 10.0);
    r.level = r.level == 0 ? p : r.level + 0.2 * (p - r.level);
    a.levelDbfs = (float)(10.0 * std::log10(std::max(r.level, 1e-12)));
    if (f.df == 18) a.tisb = true;

    // on the ground or in the air
    if (m.tc >= 5 && m.tc <= 8) a.ground = true;
    else if ((m.tc >= 9 && m.tc <= 22 && m.tc != 19)) a.ground = false;
    else if (f.df == 4 || f.df == 5 || f.df == 20 || f.df == 21) { if (m.fs == 0 || m.fs == 2) a.ground = false; else if (m.fs == 1 || m.fs == 3) a.ground = true; }
    else if (f.df == 0 || f.df == 16) a.ground = m.vs == 1;
    else if (f.df == 11 || f.df == 17) { if (m.ca == 4) a.ground = true; else if (m.ca == 5) a.ground = false; }
    if (f.df == 4 || f.df == 5 || f.df == 20 || f.df == 21) { a.alert = m.alert; a.spi = m.spi; }
    else if (m.tc >= 9 && m.tc <= 22) { a.alert = m.alert; a.spi = m.spi; }

    if (m.hasIdent) {
        a.callsign = m.callsign;
        a.category = m.catCode ? categoryCode(m.tc, m.catCode) : "";
    }
    if (m.hasAlt) {
        if (m.altGnss) { a.hasGeoAlt = true; a.geoAltFt = m.altFt; }
        else {
            a.hasAlt = true; a.altFt = m.altFt;
            if (f.df == 17 || f.df == 18) { r.altTime = now; r.trustedAltFt = m.altFt; }   // checked by the CRC itself: the yardstick for replies
        }
    }
    if (m.hasSquawk) {
        // A reply (DF5 / 21) that changes the squawk of an aircraft has to be heard twice within a minute: its address is only in the parity.
        const bool reply = f.df == 5 || f.df == 21;
        if (!reply || !a.hasSquawk || m.squawk == a.squawk || (r.pendingSquawk == m.squawk && now - r.pendingTime <= 60.0)) {
            a.hasSquawk = true; a.squawk = m.squawk;
            r.pendingSquawk = -1;
            r.emergencySq = m.squawk == 7500 ? 5 : m.squawk == 7600 ? 4 : m.squawk == 7700 ? 1 : 0;
        } else { r.pendingSquawk = m.squawk; r.pendingTime = now; }
    }
    if (m.emergency >= 0) r.emergencyTc = m.emergency;
    if (m.hasMove) { a.hasSpeed = true; a.speedKt = (float)m.moveKt; a.speedKind = 0; r.lastVel = now; }
    if (m.hasTrack) { a.hasHeading = true; a.headingDeg = (float)m.trackDeg; a.headingIsTrack = true; }
    if (m.hasVel) { a.hasSpeed = true; a.speedKt = (float)m.speedKt; a.speedKind = m.speedKind; r.lastVel = now; }
    if (m.hasHeading) { a.hasHeading = true; a.headingDeg = (float)m.headingDeg; a.headingIsTrack = m.headingIsTrack; }
    if (m.hasVrate) { a.hasVrate = true; a.vrateFpm = m.vrateFpm; }
    if (m.hasGnssDiff) { a.hasGnssDiff = true; a.gnssDiffFt = m.gnssDiffFt; }
    if (m.hasSelAlt) { a.hasSelAlt = true; a.selAltFt = m.selAltFt; }
    if (m.hasSelHdg) { a.hasSelHdg = true; a.selHdgDeg = (float)m.selHdgDeg; }
    if (m.hasBaro) { a.hasBaroSet = true; a.baroSetMb = (float)m.baroMb; }
    if (m.adsbVersion >= 0) a.adsbVersion = m.adsbVersion;
    if (m.nacp >= 0) a.nacp = m.nacp;
    if (m.hasCpr) {
        if (m.surface) surfacePosition(r, m, now);
        else airbornePosition(r, m, now);
    }
    // Comm-B
    const CommB& c = m.commb;
    if (c.bds == 0x20) a.callsign = c.callsign;
    if (c.bds == 0x40) {
        if (c.hasSelAlt) { a.hasSelAlt = true; a.selAltFt = c.selAltMcpFt; }
        else if (c.hasSelAltFms) { a.hasSelAlt = true; a.selAltFt = c.selAltFmsFt; }
        if (c.hasBaro) { a.hasBaroSet = true; a.baroSetMb = (float)c.baroMb; }
    }
    if (c.bds == 0x50) {
        if (c.hasRoll) { a.hasRoll = true; a.rollDeg = (float)c.rollDeg; }
        if (now - r.lastVel > 10.0) {   // ADS-B velocity, when it is sent, is the better source
            if (c.hasGs) { a.hasSpeed = true; a.speedKt = (float)c.gsKt; a.speedKind = 0; }
            if (c.hasTrack) { a.hasHeading = true; a.headingDeg = (float)c.trackDeg; a.headingIsTrack = true; }
        }
    }
    if (c.bds == 0x60) {
        if (c.hasIas) { a.hasIas = true; a.iasKt = c.iasKt; }
        if (c.hasMach) { a.hasMach = true; a.mach = (float)c.mach; }
        if (c.hasBaroRate && !a.hasVrate) { a.hasVrate = true; a.vrateFpm = c.baroRateFpm; }
    }
}

void AdsbTracker::tick(double now) {
    now_ = std::max(now_, now);
    for (auto it = rec_.begin(); it != rec_.end();) {
        if (now - it->second.lastSeen > opt_.expirySec) it = rec_.erase(it);
        else ++it;
    }
}

void AdsbTracker::snapshot(AdsbTelemetry& t, double now) const {
    t.aircraftCount = (uint32_t)rec_.size();
    t.withPosition = 0;
    std::vector<const Rec*> v;
    v.reserve(rec_.size());
    for (const auto& kv : rec_) {
        v.push_back(&kv.second);
        if (kv.second.a.hasPos) t.withPosition++;
    }
    std::sort(v.begin(), v.end(), [](const Rec* a, const Rec* b) { return a->lastSeen > b->lastSeen; });
    if (v.size() > kMaxListed) v.resize(kMaxListed);
    t.aircraft.clear();
    t.aircraft.reserve(v.size());
    for (const Rec* r : v) {
        AdsbAircraft a = r->a;
        a.ageSec = (float)std::max(0.0, now - r->lastSeen);
        a.posAgeSec = a.hasPos ? (float)std::max(0.0, now - r->lastPos) : 0.f;
        a.emergency = r->emergencyTc ? r->emergencyTc : r->emergencySq;
        if (refValid_ && a.hasPos) {
            a.hasRange = true;
            a.distNm = (float)distanceNm(refLat_, refLon_, a.lat, a.lon);
            a.bearingDeg = (float)bearingDeg(refLat_, refLon_, a.lat, a.lon);
        }
        t.aircraft.push_back(std::move(a));
    }
    t.frames.assign(frames_.begin(), frames_.end());
    t.refValid = refValid_; t.refLat = refLat_; t.refLon = refLon_;
    t.maxRangeNm = maxRange_;
    for (int i = 0; i < 32; i++) t.dfCount[i] = dfCount_[i];
    t.corrected = corrected_;
    t.msgsPerSec = (float)(windows_[0] + windows_[1] + windows_[2] + windows_[3]);   // four windows of 0.25 s
    t.snrDb = (float)snrEma_;
    t.levelDbfs = snrInit_ ? (float)levelEma_ : -120.f;
}

std::string adsbDescribe(const Msg& m) {
    char b[160];
    switch (m.df) {
    case 0: case 16:
        if (m.hasAlt) snprintf(b, sizeof b, "Air-air reply, altitude %d ft", m.altFt); else snprintf(b, sizeof b, "Air-air reply");
        return b;
    case 4:
        if (m.hasAlt) snprintf(b, sizeof b, "Altitude reply %d ft", m.altFt); else snprintf(b, sizeof b, "Altitude reply");
        return b;
    case 5: snprintf(b, sizeof b, "Identity reply %04d", m.squawk); return b;
    case 11: return "All-call reply";
    case 17: case 18: case 19: {
        const char* pre = m.df == 18 ? "TIS-B " : m.df == 19 ? "Military " : "";
        if (m.tc >= 1 && m.tc <= 4) snprintf(b, sizeof b, "%sIdentification %s", pre, m.callsign.c_str());
        else if (m.tc >= 5 && m.tc <= 8) snprintf(b, sizeof b, "%sSurface position", pre);
        else if (m.tc >= 9 && m.tc <= 18) { if (m.hasAlt) snprintf(b, sizeof b, "%sAirborne position %d ft", pre, m.altFt); else snprintf(b, sizeof b, "%sAirborne position", pre); }
        else if (m.tc == 19) { if (m.hasVel) snprintf(b, sizeof b, "%sVelocity %.0f kt %s", pre, m.speedKt, m.speedKind == 0 ? "ground" : m.speedKind == 1 ? "IAS" : "TAS"); else snprintf(b, sizeof b, "%sVelocity", pre); }
        else if (m.tc >= 20 && m.tc <= 22) snprintf(b, sizeof b, "%sAirborne position (GNSS height)", pre);
        else if (m.tc == 0) { if (m.hasAlt) snprintf(b, sizeof b, "%sAltitude %d ft, no position", pre, m.altFt); else snprintf(b, sizeof b, "%sNo position information", pre); }
        else if (m.tc == 28) { if (m.emergency > 0) snprintf(b, sizeof b, "%sStatus: %s", pre, emergencyName(m.emergency)); else snprintf(b, sizeof b, "%sAircraft status", pre); }
        else if (m.tc == 29) snprintf(b, sizeof b, "%sTarget state", pre);
        else if (m.tc == 31) snprintf(b, sizeof b, "%sOperational status", pre);
        else snprintf(b, sizeof b, "%sExtended squitter, type %d", pre, m.tc);
        return b;
    }
    case 20: case 21: {
        std::string s = m.df == 20 ? "Comm-B altitude" : "Comm-B identity";
        if (m.df == 20 && m.hasAlt) { snprintf(b, sizeof b, " %d ft", m.altFt); s += b; }
        if (m.df == 21 && m.hasSquawk) { snprintf(b, sizeof b, " %04d", m.squawk); s += b; }
        if (m.commb.bds) { snprintf(b, sizeof b, ", BDS %X,%X", m.commb.bds >> 4, m.commb.bds & 15); s += b; }
        return s;
    }
    default: return m.df >= 24 ? "Extended length message" : "Mode S message";
    }
}

} // namespace dect2
