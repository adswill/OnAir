// ADS-B frame validation and the aircraft table.
#include "dect2/adsb/adsb_track.h"
#include "dect2/results/results_io.h"
#include "dect2/net/feeds.h"
#include "dect2/automation/engine.h"
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
static const size_t kMaxTrack = 80;   // a few minutes of 5 s samples: enough of a trail to read the path on the map
static const double kMaxSpeedKt = 1800;     // faster than this between two positions is a jump, not a flight
static const double kGlobalCprPairSec = 15.0; // even/odd airborne pair window (matches cprHave snapshot age)
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
    stats_ = AdsbRangeStats();
    now_ = 0;
    snrEma_ = levelEma_ = 0; snrInit_ = false;
}

void AdsbTracker::shiftTime(double dt) {
    for (auto& kv : rec_) {
        Rec& r = kv.second;
        for (double* t : {&r.firstSeen, &r.lastSeen, &r.lastTrusted, &r.lastPos, &r.lastTrack, &r.lastVel, &r.altTime, &r.baroAltTime, &r.pendingTime,
                          &r.t50, &r.t60, &r.tWind, &r.tSat, &r.tWindRep, &r.tSatRep, &r.tRa, &r.tMet}) *t += dt;
        for (int i = 0; i < 2; i++) { r.cpr[i].t += dt; r.scpr[i].t += dt; }
    }
    for (auto& f : frames_) f.timeSec += dt;
    now_ += dt;
}

std::string AdsbTracker::saveResults() const { return results::pack(now_, rec_, frames_, stats_); }

bool AdsbTracker::loadResults(const std::string& blob, double now) {
    double t0 = 0;
    std::unordered_map<uint32_t, Rec> rec;
    std::deque<AdsbFrameInfo> frames;
    AdsbRangeStats st;
    if (!results::unpack(blob, t0, rec, frames, st)) return false;
    stats_ = std::move(st);
    rec_ = std::move(rec);
    frames_ = std::move(frames);
    const double keep = now_;
    now_ = t0;
    shiftTime(now - t0);
    now_ = std::max(keep, now);
    return true;
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
            // the least certain bits (the slicer's confidence per bit): every pair among the 16 least certain, every triple among the 12 least certain.
            // The remainder of the damaged frame is the XOR of the single-bit syndromes of the flipped positions, so each combination is one XOR and a
            // compare; the address (known, unless fixUnknown) and the format still have to fit, which keeps the false repairs out.
            int idx[112];
            std::iota(idx, idx + L, 0);
            const int K = 16;
            std::partial_sort(idx, idx + K, idx + L, [&](int a, int b) { return raw.conf[a] < raw.conf[b]; });
            uint32_t syn[K];
            for (int k = 0; k < K; k++) syn[k] = singleBitSyndrome(L, idx[k]);
            auto tryFlip = [&](int n, const int* sel) {
                uint8_t t[14];
                std::memcpy(t, raw.bytes, 14);
                for (int i = 0; i < n; i++) t[idx[sel[i]] >> 3] ^= (uint8_t)(0x80 >> (idx[sel[i]] & 7));
                int df; uint32_t icao;
                if (fits(t, df, icao)) { std::memcpy(raw.bytes, t, 14); raw.bits = L; return true; }
                return false;
            };
            for (int a = 0; a < K; a++) for (int b = a + 1; b < K; b++) {
                if ((syn[a] ^ syn[b]) != rem) continue;
                const int sel[2] = {a, b};
                if (tryFlip(2, sel)) return true;
            }
            for (int a = 0; a < 12; a++) for (int b = a + 1; b < 12; b++) {
                const uint32_t ab = syn[a] ^ syn[b] ^ rem;
                for (int c = b + 1; c < 12; c++) {
                    if (syn[c] != ab) continue;
                    const int sel[3] = {a, b, c};
                    if (tryFlip(3, sel)) return true;
                }
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

void AdsbTracker::resetStats() { stats_ = AdsbRangeStats(); stats_.nextSample = now_ + 10; maxRange_ = 0; }

uint32_t AdsbTracker::aircraftFrom(uint8_t src) const {
    uint32_t n = 0;
    for (const auto& kv : rec_) if (kv.second.a.srcId == src) n++;
    return n;
}

// range statistics: called for every confirmed position
void AdsbTracker::noteStats(const Rec& r) {
    const AdsbAircraft& a = r.a;
    if (a.hasAlt && (!stats_.hasMaxAlt || a.altFt > stats_.maxAltFt)) { stats_.hasMaxAlt = true; stats_.maxAltFt = a.altFt; }
    if (!refValid_ || !a.hasPos || a.posKind < 2) return;
    const double d = distanceNm(refLat_, refLon_, a.lat, a.lon);
    double b = bearingDeg(refLat_, refLon_, a.lat, a.lon);
    b = std::fmod(std::fmod(b, 360.0) + 360.0, 360.0);
    const int sec = std::min(kAdsbSectors - 1, (int)(b / 10.0));
    stats_.sectorMaxNm[sec] = std::max(stats_.sectorMaxNm[sec], (float)d);
    const int band = adsbAltBand(a.hasAlt, a.altFt, a.ground);
    if (band >= 0) stats_.sectorBandMaxNm[band][sec] = std::max(stats_.sectorBandMaxNm[band][sec], (float)d);
    stats_.rangeHist[std::min(kAdsbRangeBins - 1, (int)(d / 10.0))]++;
    stats_.positions++;
    stats_.maxRangeNm = std::max(stats_.maxRangeNm, (float)d);
}

void AdsbTracker::applyExternal(const AdsbExtUpdate& u, uint8_t src, double now) {
    Rec& r = touch(u.icao & 0xFFFFFF, now);
    AdsbAircraft& a = r.a;
    a.srcId = src;
    a.messages++;
    r.lastSeen = now;
    r.lastTrusted = now;
    now_ = std::max(now_, now);
    if (!u.callsign.empty()) a.callsign = u.callsign;
    if (u.hasAlt) { a.hasAlt = true; a.altFt = u.altFt; r.baroAltTime = r.altTime = now; r.trustedAltFt = u.altFt; }
    if (u.hasSpeed) { a.hasSpeed = true; a.speedKt = (float)u.speedKt; a.speedKind = 0; r.lastVel = now; }
    if (u.hasHeading) { a.hasHeading = true; a.headingDeg = (float)u.headingDeg; a.headingIsTrack = true; }
    if (u.hasVrate) { a.hasVrate = true; a.vrateFpm = u.vrateFpm; }
    if (u.hasSquawk) {
        a.hasSquawk = true; a.squawk = u.squawk;
        r.emergencySq = u.squawk == 7500 ? 5 : u.squawk == 7600 ? 4 : u.squawk == 7700 ? 1 : 0;
    }
    if (u.hasGround) a.ground = u.ground;
    if (u.hasPos && std::fabs(u.lat) <= 90 && std::fabs(u.lon) <= 180 && (u.lat != 0 || u.lon != 0)) {
        commitPosition(r, u.lat, u.lon, 2, now);
        noteStats(r);
    }
}

void AdsbTracker::apply(const AdsbFrame& f, uint8_t src) {
    const Msg& m = f.msg;
    curSrc_ = src;
    const bool nonIcao = f.df == 18 && (m.ca == 1 || m.ca == 5);
    const uint32_t key = f.icao | (nonIcao ? 0x1000000u : 0u);
    Rec& r = touch(key, f.timeSec);
    const double posBefore = r.lastPos;
    if (f.bits == 112 ? (f.df == 17 || f.df == 18 || f.df == 19) : f.df == 11) r.lastTrusted = f.timeSec;
    applyMsg(r, f);
    r.a.srcId = src;
    if (r.lastPos != posBefore) noteStats(r);
    if (src) { now_ = std::max(now_, f.timeSec); return; }   // another receiver's frame: no local counters, no frame list, no data feed
    if (Feeds::get().wantAdsb()) {   // the data feeds out (feeds.h), off unless the user turned them on
        FeedAdsbFrame ff;
        memcpy(ff.bytes, f.bytes, sizeof ff.bytes);
        ff.bits = f.bits; ff.timeSec = f.timeSec; ff.levelDbfs = f.levelDbfs;
        ff.icao = f.icao; ff.nonIcao = nonIcao; ff.msg = m;
        ff.hasPos = r.lastPos != posBefore && r.a.hasPos;
        ff.lat = r.a.lat; ff.lon = r.a.lon;
        Feeds::get().adsb(ff);
    }
    if (automation::wants(automation::FAdsb)) {   // the automations: a new aircraft, a new squawk, otherwise an update every 2 s
        const bool isNew = !r.autoNew;
        const int sq = r.a.hasSquawk ? r.a.squawk : -1;
        if (isNew || sq != r.autoSq || f.timeSec - r.autoEmit >= 2.0) {
            auto mk = [&](const char* type) {
                automation::Event ev(type);
                const AdsbAircraft& a = r.a;
                ev.at(automation::wallNow(), "ADS-B", 1090e6);
                char hex[8];
                snprintf(hex, sizeof hex, "%06X", key & 0xFFFFFF);
                ev.str("hex", hex).str("callsign", a.callsign).str("category", a.category).num("messages", a.messages).num("emergency", a.emergency).flag("ground", a.ground);
                if (a.hasSquawk) ev.num("squawk", a.squawk);
                if (a.hasAlt) ev.num("alt_ft", a.altFt);
                if (a.hasSpeed) ev.num("speed_kt", a.speedKt);
                if (a.hasHeading) ev.num("heading_deg", a.headingDeg);
                if (a.hasVrate) ev.num("vrate_fpm", a.vrateFpm);
                if (a.hasPos) ev.num("lat", a.lat).num("lon", a.lon);
                if (a.hasRange) ev.num("distance_km", a.distNm * 1.852);
                return ev;
            };
            if (isNew) automation::emit(mk("adsb.new"));
            automation::emit(mk("adsb.aircraft"));
            r.autoNew = true; r.autoSq = sq; r.autoEmit = f.timeSec;
        }
    }

    good_++;
    window_++;
    if (f.corrected) corrected_++;
    if (f.levelDbfs > kAdsbLoudDbfs) loud_++;
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
        a.track.push_back({(float)lat, (float)lon, (int16_t)std::max(-3000, std::min(3000, a.altFt / 100)), a.hasAlt, a.ground});
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
    if (!a.hasPos && refValid_ && cprLocalAirborne(m.cprLat, m.cprLon, p == 1, refLat_, refLon_, lat, lon)
             && distanceNm(refLat_, refLon_, lat, lon) <= 180.0) kind = 1;
    else if (r.cpr[1 - p].valid && std::fabs(now - r.cpr[1 - p].t) <= kGlobalCprPairSec &&
        cprGlobalAirborne(r.cpr[0].lat, r.cpr[0].lon, r.cpr[1].lat, r.cpr[1].lon, p == 1, lat, lon)) kind = 2;
    else if (a.hasPos && now - r.lastPos <= 30.0 && cprLocalAirborne(m.cprLat, m.cprLon, p == 1, a.lat, a.lon, lat, lon)) kind = 3;
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
            a.hasAlt = true; a.altFt = m.altFt; r.baroAltTime = now;
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
    if (m.sil >= 0) a.sil = m.sil;
    if (m.hasCpr && !m.surface) {   // containment from the type code (DO-260B table, supplement bits ignored)
        static const int8_t nicOfTc[] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, 11, 10, 9, 7, 8, 5, 4, 3, 2, 0};   // TC 9 - 18
        if (m.tc >= 9 && m.tc <= 18) a.nic = nicOfTc[m.tc];
    }
    if (m.hasCpr) {
        if (m.surface) surfacePosition(r, m, now);
        else airbornePosition(r, m, now);
    }
    // Comm-B
    // An identity reply (DF21) has no altitude of its own: the weather registers are tried with the aircraft's last barometric altitude
    // when it is at most 30 s old, and refused without one
    CommB cb21;
    const bool alt21 = f.df == 21 && a.hasAlt && now - r.baroAltTime <= 30.0;
    if (alt21) cb21 = inferCommB(f.bytes + 4, a.altFt);
    const CommB& c = alt21 ? cb21 : m.commb;
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
    if (c.bds == 0x50) { r.c50 = c; r.t50 = now; }
    if (c.bds == 0x60) { r.c60 = c; r.t60 = now; }
    if ((c.bds == 0x50 || c.bds == 0x60) && std::fabs(r.t50 - r.t60) <= 4.0) {
        const CommB& p = r.c50; const CommB& q = r.c60;
        const bool steady = (!p.hasRoll || std::fabs(p.rollDeg) <= 5.0) && (!p.hasTrackRate || std::fabs(p.trackRateDps) <= 0.5);   // a turn makes heading and track disagree in time
        if (steady && p.hasTrack && p.hasGs && p.hasTas && q.hasHeading && p.tasKt >= 100) {
            const double lat = a.hasPos ? a.lat : refValid_ ? refLat_ : 0.0, lon = a.hasPos ? a.lon : refValid_ ? refLon_ : 0.0;
            const double hdg = q.headingDeg + ((a.hasPos || refValid_) ? magDeclinationDeg(lat, lon) : 0.0);
            double from = 0, kt = 0;
            windTriangle(p.trackDeg, p.gsKt, hdg, p.tasKt, from, kt);
            if (kt <= 250.0) {
                const double rr = M_PI / 180.0, we = -kt * std::sin(from * rr), wn = -kt * std::cos(from * rr);
                if (now - r.tWind > 60.0) { r.windE = we; r.windN = wn; }
                else { r.windE += 0.3 * (we - r.windE); r.windN += 0.3 * (wn - r.windN); }
                r.tWind = now;
                a.hasWindDer = true;
                a.windDerKt = (float)std::hypot(r.windE, r.windN);
                double d = std::atan2(-r.windE, -r.windN) / rr; if (d < 0) d += 360.0;
                a.windDerDir = (float)d;
            }
        }
        if (p.hasTas && q.hasMach && q.mach >= 0.4 && p.tasKt >= 150) {   // below Mach 0.4 the 0.004 step alone is worth several degrees
            const double t = satFromTasMach(p.tasKt, q.mach);
            if (t >= -80.0 && t <= 50.0) {
                r.satEma = now - r.tSat > 60.0 ? t : r.satEma + 0.3 * (t - r.satEma);
                r.tSat = now;
                if (now - r.tSatRep > 60.0) { a.hasSat = true; a.satC = (float)r.satEma; a.satDerived = true; }
            }
        }
    }
    if (c.bds == 0x44 || c.bds == 0x45) {
        r.tMet = now;
        if (c.hasWind) { a.hasWindRep = true; a.windRepKt = (float)c.windKt; a.windRepDir = (float)c.windDirDeg; r.tWindRep = now; }
        if (c.hasSat) { a.hasSat = true; a.satC = (float)c.satC; a.satDerived = false; r.tSatRep = now; }
        if (c.hasPressure) { a.hasPressure = true; a.pressureHpa = c.pressureHpa; }
        if (c.hasHumidity) { a.hasHumidity = true; a.humidityPct = (float)c.humidityPct; }
        if (c.turbulence >= 0) a.turbulence = c.turbulence;
        if (c.bds == 0x45) { a.windShear = c.windShear; a.microburst = c.microburst; a.icing = c.icing; a.wakeVortex = c.wakeVortex; }
    }
    if (c.bds == 0x30) {
        r.tRa = now;
        a.hasRa = true; a.raAra = c.ara; a.raRac = c.rac; a.raTerminated = c.raTerminated; a.raThreat = c.tti == 1 ? c.threatIcao : 0;
    }
    if (c.bds == 0x10) { a.subnetVersion = c.subnetVersion; a.acasRa = c.acasOperating; }
    if (c.bds == 0x17) a.gicbMask = c.gicbMask;
}

void AdsbTracker::tick(double now) {
    now_ = std::max(now_, now);
    while (now >= stats_.nextSample && stats_.rateSeries.size() < 2000) {   // a point every 10 s: message rate and aircraft in the table
        stats_.rateSeries.push_back((float)(windows_[0] + windows_[1] + windows_[2] + windows_[3]));
        stats_.countSeries.push_back((uint16_t)std::min<size_t>(rec_.size(), 65535));
        stats_.nextSample += 10;
    }
    if (now >= stats_.nextSample) stats_.nextSample = now + 10;
    for (auto it = rec_.begin(); it != rec_.end();) {
        if (now - it->second.lastSeen > (opt_.dropSec > 0 ? opt_.dropSec : opt_.expirySec)) it = rec_.erase(it);
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
        a.cprHave = 0;
        if (r->cpr[0].valid && now - r->cpr[0].t <= 15.0) a.cprHave |= 1;
        if (r->cpr[1].valid && now - r->cpr[1].t <= 15.0) a.cprHave |= 2;
        a.emergency = r->emergencyTc ? r->emergencyTc : r->emergencySq;
        if (now - r->tWind > 300.0) a.hasWindDer = false;      // weather goes stale after five minutes, an advisory after one
        if (now - r->tWindRep > 300.0) a.hasWindRep = false;
        if (now - std::max(r->tSat, r->tSatRep) > 300.0) a.hasSat = false;
        if (now - r->tRa > 60.0) a.hasRa = false;
        a.raAgeSec = a.hasRa ? (float)std::max(0.0, now - r->tRa) : 0.f;
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
    t.loudMsgs = loud_;
    t.stats = stats_;
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
        const CommB& c = m.commb;
        if (c.bds) { snprintf(b, sizeof b, ", BDS %X,%X", c.bds >> 4, c.bds & 15); s += b; }
        if (c.bds == 0x10) { snprintf(b, sizeof b, " capability: subnetwork v%d%s", c.subnetVersion, c.acasOperating ? ", ACAS RA" : ""); s += b; }
        if (c.bds == 0x17) { snprintf(b, sizeof b, " GICB registers %06X", (unsigned)c.gicbMask); s += b; }
        if (c.bds == 0x30) {
            s += " ACAS RA: " + raText(c.ara, c.rac, c.raTerminated);
            if (c.tti == 1) { snprintf(b, sizeof b, ", threat %06X", (unsigned)c.threatIcao); s += b; }
        }
        if (c.bds == 0x44) { snprintf(b, sizeof b, " wind %03.0f/%d kt, SAT %.1f C", c.windDirDeg, c.windKt, c.satC); s += b; if (c.hasPressure) { snprintf(b, sizeof b, ", %d hPa", c.pressureHpa); s += b; } }
        if (c.bds == 0x45) {
            snprintf(b, sizeof b, " hazard: SAT %.1f C", c.satC); s += b;
            const char* nm[5] = {"turbulence", "wind shear", "microburst", "icing", "wake"}; const int lv[5] = {c.turbulence, c.windShear, c.microburst, c.icing, c.wakeVortex};
            for (int i = 0; i < 5; i++) if (lv[i] > 0) { snprintf(b, sizeof b, ", %s %d", nm[i], lv[i]); s += b; }
        }
        return s;
    }
    default: return m.df >= 24 ? "Extended length message" : "Mode S message";
    }
}

} // namespace dect2
