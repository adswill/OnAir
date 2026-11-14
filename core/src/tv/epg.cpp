// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tv/epg.h"
#include "dect2/tv/dvb_text.h"
#include "dect2/tv/ts_proto.h"
#include "dect2/tv/tvjson.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include "dect2/common/diag.h"

namespace dect2 {

namespace {
bool bcd(uint8_t b, int& v) {
    if ((b >> 4) > 9 || (b & 15) > 9) return false;
    v = (b >> 4) * 10 + (b & 15);
    return true;
}
// days since 1970-01-01 -> civil date (Howard Hinnant's algorithm)
void civilFromDays(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yy + (m <= 2));
}
std::string lower(const std::string& s) { std::string o = s; for (char& c : o) c = (char)std::tolower((unsigned char)c); return o; }
}

bool dvbTime(const uint8_t* p, int64_t& unixSec) {
    if (p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF && p[4] == 0xFF) return false;
    const int mjd = (p[0] << 8) | p[1];
    int h, m, s;
    if (!bcd(p[2], h) || !bcd(p[3], m) || !bcd(p[4], s) || h > 23 || m > 59 || s > 59 || mjd < 40587) return false;
    unixSec = (int64_t)(mjd - 40587) * 86400 + h * 3600 + m * 60 + s;
    return true;
}

// ---- EIT ---------------------------------------------------------------------------------------------------------------------------------------
namespace {
void eventDescriptors(const uint8_t* d, int n, EpgItem& ev, const dvbsi::TextContext& context) {
    std::map<int, std::string> ext;
    std::string extLang;
    int p = 0;
    while (p + 2 <= n) {
        const int tag = d[p], dl = d[p + 1];
        if (p + 2 + dl > n) break;
        const uint8_t* b = d + p + 2;
        switch (tag) {
        case 0x4D:   // short event: language, name, text
            if (dl >= 5) {
                const int nl = b[3];
                if (4 + nl <= dl) {
                    const int tl = b[4 + nl];
                    if (5 + nl + tl <= dl) {
                        ev.lang.assign((const char*)b, 3);
                        const auto eventContext = context.withLanguage(ev.lang);
                        ev.title = dvbsi::dvbText(b + 4, nl, eventContext);
                        ev.text = dvbsi::dvbText(b + 5 + nl, tl, eventContext);
                    }
                }
            }
            break;
        case 0x4E:   // extended event: descriptor_number, language, items, text
            if (dl >= 6) {
                const int num = b[0] >> 4;
                const int li = b[4];
                if (5 + li < dl) {
                    const int tl = b[5 + li];
                    if (6 + li + tl <= dl) {
                        if (extLang.empty()) extLang.assign((const char*)b + 1, 3);
                        ext[num].assign((const char*)b + 6 + li, tl);
                    }
                }
            }
            break;
        case 0x54:   // content
            for (int i = 0; i + 1 < dl; i += 2) if (ev.content.size() < 8) ev.content.push_back(b[i]);
            break;
        case 0x55:   // parental rating
            for (int i = 0; i + 4 <= dl; i += 4) {
                const int r = b[i + 3];
                if (r >= 1 && r <= 15) ev.rating = std::max(ev.rating, r + 3);
            }
            break;
        case 0x76: { // content identifier
            int i = 0;
            while (i < dl) {
                const int type = b[i] >> 2, loc = b[i] & 3;
                i++;
                std::string crid;
                if (loc == 0) {
                    if (i >= dl) break;
                    const int l = b[i++];
                    if (i + l > dl) break;
                    crid.assign((const char*)b + i, l);
                    i += l;
                } else if (loc == 1) { i += 2; } else break;
                if (type == 1 && ev.crid.empty()) ev.crid = dvbsi::dvbText((const uint8_t*)crid.data(), (int)crid.size());
                else if (type == 2 && ev.seriesCrid.empty()) ev.seriesCrid = dvbsi::dvbText((const uint8_t*)crid.data(), (int)crid.size());
            }
            break;
        }
        default: break;
        }
        p += 2 + dl;
    }
    if (!ext.empty()) {
        std::string all;
        for (auto& kv : ext) all += kv.second;
        ev.extended = dvbsi::dvbText((const uint8_t*)all.data(), (int)all.size(), context.withLanguage(extLang));
        if (ev.lang.empty()) ev.lang = extLang;
    }
}
}


// ---- guide language ---------------------------------------------------------------------------------------------------------------------------
namespace {
std::mutex gLangMu;
std::string gLang;
std::string lang3(const std::string& l) { std::string o = lower(l); if (o.size() > 3) o.resize(3); return o; }
}

// ISO 639-2 has a bibliographic and a terminology code for some languages (ger / deu); broadcasters use both
std::string epgLanguageKey(const std::string& l) {
    static const char* const kAlias[][2] = {{"ger", "deu"}, {"fre", "fra"}, {"dut", "nld"}, {"cze", "ces"}, {"gre", "ell"}, {"rum", "ron"}, {"slo", "slk"}, {"chi", "zho"},
                                            {"ice", "isl"}, {"arm", "hye"}, {"baq", "eus"}, {"mac", "mkd"}, {"alb", "sqi"}, {"bur", "mya"}, {"geo", "kat"}, {"per", "fas"},
                                            {"tib", "bod"}, {"wel", "cym"}, {"may", "msa"}, {"mao", "mri"}, {"nob", "nor"}, {"nno", "nor"}};
    const std::string o = lang3(l);
    for (auto& a : kAlias) if (o == a[0]) return a[1];
    return o;
}

void epgSetPreferredLanguage(const std::string& iso639) { std::lock_guard<std::mutex> lk(gLangMu); gLang = epgLanguageKey(iso639); }
std::string epgPreferredLanguage() { std::lock_guard<std::mutex> lk(gLangMu); return gLang; }

bool epgPickLanguage(const uint8_t* s, int len, const std::string& wantIn, std::map<int, EpgLangPick>& out) {
    out.clear();
    const std::string want = epgLanguageKey(wantIn);
    if (want.size() != 3 || len < 18) return false;
    const int total = std::min(len, 3 + (((s[1] & 0x0F) << 8) | s[2]));
    const int end = total - 4;
    int p = 14;
    while (p + 12 <= end) {
        const int id = (s[p] << 8) | s[p + 1];
        const int dl = ((s[p + 10] & 0x0F) << 8) | s[p + 11];
        if (p + 12 + dl > end) break;
        EpgLangPick pk;
        int q = p + 12;
        const int dend = q + dl;
        bool seen = false;
        while (q + 2 <= dend) {
            const int tag = s[q], l = s[q + 1];
            if (q + 2 + l > dend) break;
            const uint8_t* b = s + q + 2;
            const int off = q + 2;
            if (tag == 0x4D && l >= 5 && epgLanguageKey(std::string((const char*)b, 3)) == want) {
                const int nl = b[3];
                if (4 + nl < l) {
                    const int tl = b[4 + nl];
                    if (5 + nl + tl <= l && !pk.hasShort) {
                        pk.hasShort = true; seen = true; pk.lang.assign((const char*)b, 3);
                        pk.titleOff = off + 4; pk.titleLen = nl;
                        pk.textOff = off + 5 + nl; pk.textLen = tl;
                    }
                }
            } else if (tag == 0x4E && l >= 6 && epgLanguageKey(std::string((const char*)b + 1, 3)) == want) {
                const int li = b[4];
                if (5 + li < l) {
                    const int tl = b[5 + li];
                    if (6 + li + tl <= l) { pk.ext.push_back({off + 6 + li, tl}); seen = true; if (pk.lang.empty()) pk.lang.assign((const char*)b + 1, 3); }
                }
            }
            q += 2 + l;
        }
        if (seen) out[id] = std::move(pk);
        p += 12 + dl;
    }
    return !out.empty();
}

bool parseEitSection(const uint8_t* s, int len, EitSection& out, const dvbsi::TextContext& context) {
    out = EitSection();
    if (len < 18 || s[0] < 0x4E || s[0] > 0x6F) return false;
    const int total = 3 + (((s[1] & 0x0F) << 8) | s[2]);
    if (total < 18 || total > len) return false;
    if (tsp::crc32(s, total) != 0) { DIAG_DEBUG_EVERY(752979, 10.0, "An EIT section for service 0x%04X failed its CRC check, so its programme events are ignored.", (unsigned)((s[3] << 8) | s[4])); return false; }
    out.tableId = s[0];
    out.sid = (s[3] << 8) | s[4];
    out.version = (s[5] >> 1) & 0x1F;
    out.current = s[5] & 1;
    out.secNum = s[6];
    out.lastSec = s[7];
    out.tsid = (s[8] << 8) | s[9];
    out.onid = (s[10] << 8) | s[11];
    const bool other = s[0] == 0x4F || (s[0] >= 0x60);
    const int end = total - 4;
    int p = 14;
    while (p + 12 <= end) {
        EpgItem ev;
        ev.eventId = (s[p] << 8) | s[p + 1];
        int64_t st = 0;
        const bool okStart = dvbTime(s + p + 2, st);
        int dh, dm, ds;
        const bool okDur = bcd(s[p + 7], dh) && bcd(s[p + 8], dm) && bcd(s[p + 9], ds) && dm < 60 && ds < 60;
        ev.running = s[p + 10] >> 5;
        ev.freeCa = (s[p + 10] >> 4) & 1;
        const int dl = ((s[p + 10] & 0x0F) << 8) | s[p + 11];
        if (p + 12 + dl > end) break;   // damaged loop: keep what was decoded
        if (okStart && okDur) {
            ev.start = st;
            ev.duration = dh * 3600 + dm * 60 + ds;
            ev.other = other;
            eventDescriptors(s + p + 12, dl, ev, context);
            out.events.push_back(std::move(ev));
        }
        p += 12 + dl;
    }
    const std::string want = epgPreferredLanguage();
    std::map<int, EpgLangPick> picks;
    if (!want.empty() && epgPickLanguage(s, total, want, picks)) {
        for (EpgItem& ev : out.events) {
            auto it = picks.find(ev.eventId);
            if (it == picks.end()) continue;
            const EpgLangPick& pk = it->second;
            ev.lang = pk.lang;
            const auto selectedContext = context.withLanguage(pk.lang);
            if (pk.hasShort) { ev.title = dvbsi::dvbText(s + pk.titleOff, pk.titleLen, selectedContext); ev.text = dvbsi::dvbText(s + pk.textOff, pk.textLen, selectedContext); }
            if (!pk.ext.empty()) {
                std::string all;
                for (auto& e : pk.ext) all.append((const char*)s + e.first, (size_t)e.second);
                ev.extended = dvbsi::dvbText((const uint8_t*)all.data(), (int)all.size(), selectedContext);
            }
        }
    }
    return true;
}

// ---- time --------------------------------------------------------------------------------------------------------------------------------------
bool parseTimeSection(const uint8_t* s, int len, TotInfo& out) {
    out = TotInfo();
    if (len < 8) return false;
    if (s[0] == 0x70) {
        return dvbTime(s + 3, out.utc);
    }
    if (s[0] != 0x73 || len < 14) return false;
    const int total = 3 + (((s[1] & 0x0F) << 8) | s[2]);
    if (total < 14 || total > len || tsp::crc32(s, total) != 0) { DIAG_DEBUG_EVERY(196026, 10.0, "A TOT or TDT section failed its length or CRC check, so the broadcast time is not updated from it."); return false; }
    if (!dvbTime(s + 3, out.utc)) return false;
    const int dl = ((s[8] & 0x0F) << 8) | s[9];
    int p = 10;
    const int end = std::min(total - 4, 10 + dl);
    while (p + 2 <= end) {
        const int tag = s[p], l = s[p + 1];
        if (p + 2 + l > end) break;
        if (tag == 0x58) {
            for (int i = 0; i + 13 <= l; i += 13) {
                const uint8_t* b = s + p + 2 + i;
                LocalTimeOffset z;
                z.country.assign((const char*)b, 3);
                z.region = b[3] >> 2;
                const int neg = b[3] & 1;
                int h, m, nh, nm;
                if (!bcd(b[4], h) || !bcd(b[5], m) || !bcd(b[11], nh) || !bcd(b[12], nm)) continue;
                z.offsetMin = (neg ? -1 : 1) * (h * 60 + m);
                z.nextOffsetMin = (neg ? -1 : 1) * (nh * 60 + nm);
                if (!dvbTime(b + 6, z.changeUtc)) z.changeUtc = 0;
                out.offsets.push_back(z);
            }
            out.hasOffsets = !out.offsets.empty();
        }
        p += 2 + l;
    }
    return true;
}

int zoneOffsetAt(const LocalTimeOffset& z, int64_t utc) {
    return ((z.changeUtc > 0 && utc >= z.changeUtc) ? z.nextOffsetMin : z.offsetMin) * 60;
}

std::string formatInZone(int64_t utc, const LocalTimeOffset* z) {
    const int64_t t = utc + (z ? zoneOffsetAt(*z, utc) : 0);
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    int y, m, d;
    civilFromDays(days, y, m, d);
    char b[40];
    snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", y, m, d, (int)(rem / 3600), (int)(rem / 60 % 60));
    return b;
}

// ---- database ----------------------------------------------------------------------------------------------------------------------------------
void EpgDb::setLimits(const EpgLimits& l) { std::lock_guard<std::mutex> lk(mu_); lim_ = l; }
EpgLimits EpgDb::limits() const { std::lock_guard<std::mutex> lk(mu_); return lim_; }

void EpgDb::trimLocked(std::vector<EpgItem>& v) const {
    if (v.size() > lim_.maxPerService) v.resize(lim_.maxPerService);   // sorted by start: the farthest future goes
}

bool EpgDb::ingest(const ServiceKey& k, const EpgItem& evIn, int64_t now) {
    if (evIn.duration <= 0 || evIn.start <= 0 || evIn.duration > 24 * 3600 * 2) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (now > 0) {
        if (evIn.end() < now - lim_.keepPastSec) return false;
        if (evIn.start > now + lim_.maxFutureSec) return false;
    }
    auto it = db_.find(k);
    if (it == db_.end()) {
        if (db_.size() >= lim_.maxServices) { DIAG_DEBUG_EVERY(383206, 30.0, "The programme guide already holds %zu services, so events of a further service are dropped.", db_.size()); return false; }
        it = db_.emplace(k, std::vector<EpgItem>()).first;
    }
    auto& v = it->second;
    EpgItem ev = evIn;
    bool changed = true;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i].eventId != ev.eventId) continue;
        const EpgItem& o = v[i];
        if (ev.extended.empty() && ev.title == o.title) ev.extended = o.extended;
        if (ev.content.empty()) ev.content = o.content;
        if (!ev.rating) ev.rating = o.rating;
        if (ev.crid.empty()) ev.crid = o.crid;
        if (ev.seriesCrid.empty()) ev.seriesCrid = o.seriesCrid;
        if (ev.text.empty() && ev.title == o.title) ev.text = o.text;
        if (ev.running == 0 && o.running != 0 && o.start == ev.start) ev.running = o.running;
        changed = !(o.start == ev.start && o.duration == ev.duration && o.title == ev.title && o.text == ev.text && o.extended == ev.extended &&
                    o.running == ev.running && o.content == ev.content && o.rating == ev.rating && o.crid == ev.crid && o.seriesCrid == ev.seriesCrid && o.freeCa == ev.freeCa);
        v.erase(v.begin() + i);
        break;
    }
    // the broadcaster moved things around: events of other ids that overlap this one are stale
    for (size_t i = 0; i < v.size();) {
        if (v[i].start < ev.end() && ev.start < v[i].end()) { v.erase(v.begin() + i); changed = true; } else i++;
    }
    auto pos = std::upper_bound(v.begin(), v.end(), ev, [](const EpgItem& a, const EpgItem& b) { return a.start < b.start; });
    v.insert(pos, ev);
    trimLocked(v);
    return changed;
}

void EpgDb::ingestSection(const EitSection& s, int64_t now) {
    if (!s.current) return;
    const ServiceKey k{s.onid, s.tsid, s.sid};
    for (const auto& e : s.events) ingest(k, e, now);
}

size_t EpgDb::prune(int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t removed = 0;
    for (auto it = db_.begin(); it != db_.end();) {
        auto& v = it->second;
        const size_t before = v.size();
        v.erase(std::remove_if(v.begin(), v.end(), [&](const EpgItem& e) { return e.end() < now - lim_.keepPastSec; }), v.end());
        removed += before - v.size();
        if (v.empty()) it = db_.erase(it); else ++it;
    }
    return removed;
}

void EpgDb::clear() { std::lock_guard<std::mutex> lk(mu_); db_.clear(); }

std::vector<ServiceKey> EpgDb::services() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ServiceKey> r;
    for (auto& kv : db_) r.push_back(kv.first);
    return r;
}
std::vector<EpgItem> EpgDb::events(const ServiceKey& k) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = db_.find(k);
    return it == db_.end() ? std::vector<EpgItem>() : it->second;
}
std::vector<EpgItem> EpgDb::eventsBetween(const ServiceKey& k, int64_t from, int64_t to) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<EpgItem> r;
    auto it = db_.find(k);
    if (it == db_.end()) return r;
    for (auto& e : it->second) if (e.start < to && e.end() > from) r.push_back(e);
    return r;
}
bool EpgDb::nowNext(const ServiceKey& k, int64_t now, EpgItem* cur, EpgItem* next) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = db_.find(k);
    if (it == db_.end()) return false;
    bool haveCur = false, haveNext = false;
    for (auto& e : it->second) {
        if (e.start <= now && now < e.end()) { if (cur) *cur = e; haveCur = true; }
        else if (e.start > now) { if (next && !haveNext) *next = e; haveNext = true; break; }
    }
    return haveCur;
}
bool EpgDb::find(const ServiceKey& k, int eventId, EpgItem& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = db_.find(k);
    if (it == db_.end()) return false;
    for (auto& e : it->second) if (e.eventId == eventId) { out = e; return true; }
    return false;
}

std::vector<EpgHit> EpgDb::search(const std::string& query, size_t maxHits) const {
    std::vector<std::string> words;
    {
        std::string w;
        for (char c : lower(query)) { if (c == ' ') { if (!w.empty()) words.push_back(w); w.clear(); } else w += c; }
        if (!w.empty()) words.push_back(w);
    }
    std::vector<EpgHit> r;
    if (words.empty()) return r;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : db_) for (auto& e : kv.second) {
        const std::string hay = lower(e.title + " " + e.text + " " + e.extended);
        bool all = true;
        for (auto& w : words) if (hay.find(w) == std::string::npos) { all = false; break; }
        if (all) { r.push_back({kv.first, e}); }
    }
    std::sort(r.begin(), r.end(), [](const EpgHit& a, const EpgHit& b) { return a.ev.start < b.ev.start; });
    if (r.size() > maxHits) r.resize(maxHits);
    return r;
}

std::vector<EpgHit> EpgDb::seriesEvents(const std::string& crid, int64_t from) const {
    std::vector<EpgHit> r;
    if (crid.empty()) return r;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : db_) for (auto& e : kv.second) if (e.seriesCrid == crid && e.end() > from) r.push_back({kv.first, e});
    std::sort(r.begin(), r.end(), [](const EpgHit& a, const EpgHit& b) { return a.ev.start < b.ev.start; });
    return r;
}

size_t EpgDb::count() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (auto& kv : db_) n += kv.second.size();
    return n;
}

tvjson::Value epgItemToValue(const EpgItem& e) {
    using namespace tvjson;
    Value x = Value::object();
    x.set("id", e.eventId).set("s", e.start).set("d", e.duration);
    if (e.running) x.set("r", e.running);
    if (e.freeCa) x.set("ca", true);
    if (!e.lang.empty()) x.set("l", e.lang);
    if (!e.title.empty()) x.set("t", e.title);
    if (!e.text.empty()) x.set("x", e.text);
    if (!e.extended.empty()) x.set("e", e.extended);
    if (!e.content.empty()) { Value c = Value::array(); for (uint8_t b : e.content) c.push(Value::number(b)); x.set("c", c); }
    if (e.rating) x.set("age", e.rating);
    if (!e.crid.empty()) x.set("crid", e.crid);
    if (!e.seriesCrid.empty()) x.set("scrid", e.seriesCrid);
    if (e.other) x.set("o", true);
    return x;
}

EpgItem epgItemFromValue(const tvjson::Value& x) {
    using namespace tvjson;
    EpgItem e;
    e.eventId = (int)x.getInt("id"); e.start = x.getInt("s"); e.duration = (int)x.getInt("d");
    e.running = (int)x.getInt("r"); e.freeCa = x.getBool("ca");
    e.lang = x.getStr("l"); e.title = x.getStr("t"); e.text = x.getStr("x"); e.extended = x.getStr("e");
    if (const Value* c = x.get("c")) if (c->type == Value::Arr) for (auto& b : c->a) if (b.type == Value::Num && e.content.size() < 8) e.content.push_back((uint8_t)b.n);
    e.rating = (int)x.getInt("age"); e.crid = x.getStr("crid"); e.seriesCrid = x.getStr("scrid"); e.other = x.getBool("o");
    return e;
}

std::string EpgDb::toJson() const {
    using namespace tvjson;
    Value root = Value::object();
    root.set("version", 1);
    Value svcs = Value::array();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : db_) {
            Value s = Value::object();
            s.set("onid", kv.first.onid).set("tsid", kv.first.tsid).set("sid", kv.first.sid);
            Value evs = Value::array();
            for (auto& e : kv.second) evs.push(epgItemToValue(e));
            s.set("events", evs);
            svcs.push(s);
        }
    }
    root.set("services", svcs);
    return dump(root);
}

bool EpgDb::fromJson(const std::string& json) {
    using namespace tvjson;
    Value root;
    if (!parse(json, root) || root.type != Value::Obj) { DIAG_DEBUG(643112, "The saved programme guide is not valid JSON, so it is not loaded."); return false; }
    const Value* svcs = root.get("services");
    if (!svcs || svcs->type != Value::Arr) return false;
    std::map<ServiceKey, std::vector<EpgItem>> nd;
    for (auto& s : svcs->a) {
        if (s.type != Value::Obj) continue;
        ServiceKey k{(int)s.getInt("onid"), (int)s.getInt("tsid"), (int)s.getInt("sid")};
        const Value* evs = s.get("events");
        if (!evs || evs->type != Value::Arr) continue;
        auto& v = nd[k];
        for (auto& x : evs->a) {
            if (x.type != Value::Obj) continue;
            EpgItem e = epgItemFromValue(x);
            if (e.duration > 0 && e.start > 0) v.push_back(std::move(e));
        }
        std::sort(v.begin(), v.end(), [](const EpgItem& a, const EpgItem& b) { return a.start < b.start; });
    }
    std::lock_guard<std::mutex> lk(mu_);
    db_ = std::move(nd);
    return true;
}

bool EpgDb::save(const std::string& path) const {
    const std::string tmp = path + ".tmp";
    FILE* f = u8fopen(tmp, "wb");
    if (!f) { DIAG_ERROR(645984, "The programme guide file %s could not be created, so the guide is not saved.", tmp.c_str()); return false; }
    const std::string j = toJson();
    const bool ok = fwrite(j.data(), 1, j.size(), f) == j.size();
    fclose(f);
    std::error_code ec;
    if (!ok) { DIAG_ERROR(792533, "Writing the programme guide file %s failed (the disk may be full), so the guide is not saved.", tmp.c_str()); std::filesystem::remove(u8file(tmp), ec); return false; }
    std::filesystem::rename(u8file(tmp), u8file(path), ec);
    if (ec) DIAG_ERROR(319122, "The programme guide file %s could not be put in place (%s), so the guide is not saved.", path.c_str(), ec.message().c_str());
    return !ec;
}

bool EpgDb::load(const std::string& path) {
    FILE* f = u8fopen(path, "rb");
    if (!f) { DIAG_DEBUG(622671, "The saved programme guide %s could not be opened, so the guide starts empty.", path.c_str()); return false; }
    std::string j;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && j.size() < (64u << 20)) j.append(buf, n);
    fclose(f);
    return fromJson(j);
}

// ---- collector ---------------------------------------------------------------------------------------------------------------------------------
void EpgCollector::feed(const uint8_t* pkt) {
    if (pkt[0] != 0x47) return;
    const int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    if (pid != 0x11 && pid != 0x12 && pid != 0x14) return;
    std::lock_guard<std::mutex> lk(mu_);
    asm_[pid].feed(pkt, [&](const uint8_t* s, int len) { section(pid, s, len); });
}

int64_t EpgCollector::nowLocked() const {
    if (fixedNow_) return fixedNow_;
    if (tdt_) return tdt_ + (int64_t)sinceTdt_;
    return (int64_t)std::time(nullptr);
}

void EpgCollector::section(int pid, const uint8_t* s, int len) {
    dvbsi::TextContext context{textEncoding_};
    if (!zones_.empty()) {
        context.country = zones_.front().country;
        for (const auto& zone : zones_) if (zone.country != context.country) { context.country.clear(); break; }
    }
    if (pid == 0x12) {
        EitSection e;
        if (!parseEitSection(s, len, e, context)) { bad_++; return; }
        ok_++;
        if (db_) db_->ingestSection(e, nowLocked());
    } else if (pid == 0x14) {
        TotInfo t;
        if (!parseTimeSection(s, len, t)) return;
        tdt_ = t.utc;
        sinceTdt_ = 0;
        if (t.hasOffsets) zones_ = t.offsets;
    } else if (pid == 0x11) {
        if ((s[0] != 0x42 && s[0] != 0x46) || len < 11 || tsp::crc32(s, 3 + (((s[1] & 0x0F) << 8) | s[2])) != 0) return;
        int tsid = 0, onid = 0;
        std::vector<tsp::SdtService> v;
        if (!tsp::parseSdt(s, len, tsid, onid, v)) return;
        if (s[0] == 0x42) {
            if ((onid_ && onid_ != onid) || (tsid_ && tsid_ != tsid)) { zones_.clear(); context.country.clear(); }
            onid_ = onid; tsid_ = tsid;
        }
        for (auto& sv : v) {
            if (!sv.hasName || sv.name.off + sv.name.len > (uint32_t)len) continue;
            names_[ServiceKey{onid, tsid, sv.sid}] = dvbsi::dvbText(s + sv.name.off, (int)sv.name.len, context);
        }
    }
}

void EpgCollector::advance(double dt) { std::lock_guard<std::mutex> lk(mu_); sinceTdt_ += dt; }
int64_t EpgCollector::utcNow() const { std::lock_guard<std::mutex> lk(mu_); return nowLocked(); }
bool EpgCollector::haveStreamTime() const { std::lock_guard<std::mutex> lk(mu_); return tdt_ != 0; }
ServiceKey EpgCollector::currentTs() const { std::lock_guard<std::mutex> lk(mu_); return ServiceKey{onid_, tsid_, 0}; }
std::string EpgCollector::serviceName(const ServiceKey& k) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = names_.find(k);
    return it == names_.end() ? std::string() : it->second;
}
std::vector<LocalTimeOffset> EpgCollector::zones() const { std::lock_guard<std::mutex> lk(mu_); return zones_; }
uint64_t EpgCollector::sections() const { std::lock_guard<std::mutex> lk(mu_); return ok_; }
uint64_t EpgCollector::badSections() const { std::lock_guard<std::mutex> lk(mu_); return bad_; }
void EpgCollector::setFixedNow(int64_t t) { std::lock_guard<std::mutex> lk(mu_); fixedNow_ = t; }
void EpgCollector::setTextEncoding(dvbsi::TextEncoding encoding) {
    std::lock_guard<std::mutex> lk(mu_);
    if (textEncoding_ != encoding) { textEncoding_ = encoding; zones_.clear(); }
}

} // namespace dect2
