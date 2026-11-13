// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB SPI: the data group -> store decoder with the acquisition progress, the XMLTV exporter and the per-ensemble cache. See dab_spi.h.
#include "dect2/dab/dab_spi.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dect2 {

// ---------------------------------------------------------------- decoder
DabSpiDecoder::DabSpiDecoder() : mot_(64, 4u << 20) {
    mot_.setSink([this](const MotObject& o) { note(o); });
}

void DabSpiDecoder::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    resetReq_ = true;   // the MOT reassembly belongs to the thread that feeds it: it is emptied by the next data group
    dirNames_.clear(); haveNames_.clear(); dirCount_ = 0; hashes_.clear();
}

void DabSpiDecoder::clearStore() {
    store_.clear();
    std::lock_guard<std::mutex> lk(mu_);
    images_.clear(); imageOrder_.clear(); imageBytes_ = 0; imageSeq_++;
    haveNames_.clear(); hashes_.clear();
}

void DabSpiDecoder::dataGroup(const uint8_t* d, size_t n) {
    if (n > 65535) { DIAG_DEBUG_EVERY(413669, 60.0, "An SPI data group of %zu bytes is longer than the 65535 a data group can hold, so it is dropped.", n); return; }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (resetReq_) { mot_.reset(); resetReq_ = false; }
    }
    const uint64_t dirs = mot_.stats().directories;
    mot_.dataGroup(d, (int)n);
    if (mot_.stats().directories != dirs) {
        std::lock_guard<std::mutex> lk(mu_);
        std::set<std::string> names;
        for (const MotObject& m : mot_.directory()) if (!m.name.empty()) names.insert(m.name);
        dirCount_ = mot_.directory().size();
        dirEntries_ = mot_.directory();
        dirNames_ = names;
        for (auto it = haveNames_.begin(); it != haveNames_.end();) { if (!names.count(*it)) it = haveNames_.erase(it); else ++it; }
        for (auto it = hashes_.begin(); it != hashes_.end();) { if (!names.count(it->first)) it = hashes_.erase(it); else ++it; }
    }
}

void DabSpiDecoder::motObject(const MotObject& o) { note(o); }

void DabSpiDecoder::note(const MotObject& o) {
    if (tap_) tap_(o);
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : o.body) { h ^= b; h *= 1099511628211ull; }
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = hashes_.find(o.name);
        if (!o.name.empty() && it != hashes_.end() && it->second == h) { haveNames_.insert(o.name); return; }   // a repeat of the carousel
    }
    if (o.contentType == 2) {   // a picture: a logo or programme image the SPI points to by its ContentName
        std::lock_guard<std::mutex> lk(mu_);
        if (o.name.empty() || o.body.size() >= (2u << 20))
            DIAG_DEBUG(172149, "The SPI picture '%s' of %zu bytes has no name or is over 2 MB, so the guide does not keep it.", o.name.c_str(), o.body.size());
        if (!o.name.empty() && o.body.size() < (2u << 20)) {
            auto it = images_.find(o.name);
            if (it != images_.end()) { imageBytes_ -= it->second.data.size(); }
            else imageOrder_.push_back(o.name);
            images_[o.name] = Img{o.subtype, o.body};
            imageBytes_ += o.body.size();
            while (imageBytes_ > (8u << 20) && imageOrder_.size() > 1) {
                auto old = images_.find(imageOrder_.front());
                if (old != images_.end()) { imageBytes_ -= old->second.data.size(); images_.erase(old); }
                imageOrder_.erase(imageOrder_.begin());
            }
            hashes_[o.name] = h; haveNames_.insert(o.name);
            imageSeq_++; changes_++;
        }
        return;
    }
    const bool ok = store_.addObject(o);
    std::lock_guard<std::mutex> lk(mu_);
    if (!o.name.empty()) { hashes_[o.name] = h; haveNames_.insert(o.name); }
    if (ok) changes_++;
}

bool DabSpiDecoder::image(const std::string& name, int& subtype, std::vector<uint8_t>& data) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = images_.find(name);
    if (it == images_.end()) return false;
    subtype = it->second.subtype; data = it->second.data;
    return true;
}

DabSpiDecoder::Progress DabSpiDecoder::progress() const {
    std::lock_guard<std::mutex> lk(mu_);
    Progress p;
    p.groups = mot_.stats().groups;
    p.changeCount = changes_;
    p.expected = (int)dirCount_;
    int have = 0;
    for (const std::string& n : dirNames_) if (haveNames_.count(n)) have++;
    p.have = have;
    p.percent = dirCount_ == 0 ? -1 : std::min(100.0, 100.0 * have / (double)dirCount_);
    return p;
}

// ---------------------------------------------------------------- XMLTV
namespace {
std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: if ((unsigned char)c >= 0x20 || c == '\n' || c == '\t') o += c;
        }
    }
    return o;
}
std::string xmltvTime(int64_t utc, int offMin) {
    const std::string iso = spiFormatTime(utc, offMin);   // 2026-10-17T20:45:00+02:00 or ...Z
    std::string o;
    o += iso.substr(0, 4) + iso.substr(5, 2) + iso.substr(8, 2) + iso.substr(11, 2) + iso.substr(14, 2) + iso.substr(17, 2);
    const int a = offMin < 0 ? -offMin : offMin;
    char b[16];
    snprintf(b, sizeof b, " %c%02d%02d", offMin < 0 ? '-' : '+', a / 60, a % 60);
    return o + b;
}
std::string chanId(uint32_t sid) {
    char b[24];
    snprintf(b, sizeof b, "%x.dab", (unsigned)sid);
    return b;
}
}  // namespace

std::string SpiStore::toXmltv(int64_t from, int64_t to) const {
    const SpiSnapshot s = snapshot();
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE tv SYSTEM \"xmltv.dtd\">\n<tv generator-info-name=\"OnAir\">\n";
    std::set<uint32_t> used;
    for (const SpiEvent& e : s.events) used.insert(e.sid);
    for (uint32_t sid : used) {
        auto it = s.services.find(sid);
        std::string name = it != s.services.end() ? it->second.display() : "";
        if (name.empty()) { char b[16]; snprintf(b, sizeof b, "%04X", (unsigned)sid); name = b; }
        o += "<channel id=\"" + chanId(sid) + "\">\n  <display-name>" + esc(name) + "</display-name>\n";
        if (it != s.services.end() && it->second.longName.size() && it->second.longName != name) o += "  <display-name>" + esc(it->second.longName) + "</display-name>\n";
        if (it != s.services.end()) for (const SpiMedia& m : it->second.media) if (m.type.compare(0, 4, "logo") == 0 && m.url.compare(0, 4, "http") == 0) { o += "  <icon src=\"" + esc(m.url) + "\"/>\n"; break; }
        o += "</channel>\n";
    }
    for (const SpiEvent& e : s.events) {
        const int64_t end = e.duration > 0 ? e.start + e.duration : e.start;
        if (to > from && (e.start >= to || end < from)) continue;
        if (to <= from && from > 0 && end < from) continue;
        auto pit = s.programmes.find(e.progKey);
        if (pit == s.programmes.end()) {
            DIAG_DEBUG_EVERY(903286, 60.0, "A guide event of service %04X at %lld refers to programme %s, which is not stored, so the XMLTV export leaves it out.",
                             (unsigned)e.sid, (long long)e.start, e.progKey.c_str());
            continue;
        }

        const SpiProgramme& p = pit->second;
        o += "<programme start=\"" + xmltvTime(e.start, e.offMin) + "\"";
        if (e.duration > 0) o += " stop=\"" + xmltvTime(e.start + e.duration, e.offMin) + "\"";
        o += " channel=\"" + chanId(e.sid) + "\">\n";
        const std::string lang = p.lang.empty() ? "" : " lang=\"" + esc(p.lang) + "\"";
        const std::string title = p.title();
        o += "  <title" + lang + ">" + esc(title.empty() ? "?" : title) + "</title>\n";
        if (!p.longName.empty() && p.longName != title) o += "  <sub-title" + lang + ">" + esc(p.longName) + "</sub-title>\n";
        const std::string desc = p.description();
        if (!desc.empty()) o += "  <desc" + lang + ">" + esc(desc) + "</desc>\n";
        for (const std::string& g : p.genres) o += "  <category" + lang + ">" + esc(g) + "</category>\n";
        for (const SpiMedia& m : p.media) if (m.url.compare(0, 4, "http") == 0) { o += "  <icon src=\"" + esc(m.url) + "\"/>\n"; break; }
        for (const std::string& l : p.links) if (l.compare(0, 4, "http") == 0) { o += "  <url>" + esc(l) + "</url>\n"; break; }
        o += "</programme>\n";
    }
    o += "</tv>\n";
    return o;
}

// ---------------------------------------------------------------- persistence
namespace {
std::string pesc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\t') o += "\\t";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}
std::string punesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            i++;
            o += s[i] == 't' ? '\t' : s[i] == 'n' ? '\n' : s[i] == 'r' ? '\r' : s[i];
        } else o += s[i];
    }
    return o;
}
std::vector<std::string> split(const std::string& l) {
    std::vector<std::string> f;
    size_t st = 0;
    for (size_t i = 0; i <= l.size(); i++) if (i == l.size() || l[i] == '\t') { f.push_back(punesc(l.substr(st, i - st))); st = i + 1; }
    return f;
}
void line(std::string& o, char tag, std::initializer_list<std::string> f) {
    o += tag;
    for (const std::string& x : f) { o += '\t'; o += pesc(x); }
    o += '\n';
}
}  // namespace

std::string SpiStore::serialize() const {
    const SpiSnapshot s = snapshot();
    std::string o = "ONAIRSPI 1\n";
    line(o, 'E', {std::to_string(s.ecc), std::to_string(s.eid)});
    for (const auto& kv : s.services) {
        const SpiService& v = kv.second;
        line(o, 'S', {std::to_string(v.sid), std::to_string(v.ecc), std::to_string(v.eid), v.shortName, v.mediumName, v.longName, v.description, v.genre});
        for (const SpiMedia& m : v.media) line(o, 'm', {m.type, m.url, m.mime, std::to_string(m.w), std::to_string(m.h)});
    }
    for (const auto& kv : s.programmes) {
        const SpiProgramme& p = kv.second;
        line(o, 'P', {p.key, p.crid, std::to_string(p.shortId), std::to_string(p.version), p.lang, p.shortName, p.mediumName, p.longName, p.shortDesc, p.longDesc, p.recommended ? "1" : "0"});
        for (const std::string& g : p.genres) line(o, 'g', {g});
        for (const std::string& g : p.links) line(o, 'l', {g});
        for (const std::string& g : p.groups) line(o, 'r', {g});
        for (const SpiMedia& m : p.media) line(o, 'i', {m.type, m.url, m.mime, std::to_string(m.w), std::to_string(m.h)});
    }
    for (const SpiEvent& e : s.events) line(o, 'V', {std::to_string(e.sid), std::to_string(e.start), std::to_string(e.duration), std::to_string(e.offMin), e.progKey});
    for (const SpiGroup& g : s.groups) line(o, 'Q', {g.crid, g.name, g.desc, g.type, std::to_string(g.numOfItems)});
    return o;
}

bool SpiStore::deserialize(const std::string& text) {
    std::istringstream in(text);
    std::string l;
    if (!std::getline(in, l) || l != "ONAIRSPI 1") {
        DIAG_DEBUG(560905, "The cached programme guide starts with '%.40s' instead of ONAIRSPI 1, so it is not loaded.", l.c_str());
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    SpiService* curS = nullptr;
    SpiProgramme* curP = nullptr;
    size_t lines = 0;
    while (std::getline(in, l) && ++lines < 400000) {
        if (l.size() < 2) continue;
        const std::vector<std::string> f = split(l.substr(2));
        auto num = [&](size_t i) -> long long { return i < f.size() ? atoll(f[i].c_str()) : 0; };
        auto str = [&](size_t i) -> std::string { return i < f.size() ? f[i] : std::string(); };
        switch (l[0]) {
        case 'E': s_.ecc = (int)num(0); s_.eid = (int)num(1); break;
        case 'S': {
            SpiService& v = s_.services[(uint32_t)num(0)];
            v = SpiService();
            v.sid = (uint32_t)num(0); v.ecc = (int)num(1); v.eid = (int)num(2); v.shortName = str(3); v.mediumName = str(4); v.longName = str(5); v.description = str(6); v.genre = str(7);
            curS = &v; curP = nullptr;
            break;
        }
        case 'm': if (curS && f.size() >= 5) curS->media.push_back({str(0), str(1), str(2), (int)num(3), (int)num(4)}); break;
        case 'P': {
            if (s_.programmes.size() >= 20000) { curP = nullptr; break; }
            SpiProgramme& p = s_.programmes[str(0)];
            p = SpiProgramme();
            p.key = str(0); p.crid = str(1); p.shortId = (uint32_t)num(2); p.version = (int)num(3); p.lang = str(4); p.shortName = str(5); p.mediumName = str(6); p.longName = str(7);
            p.shortDesc = str(8); p.longDesc = str(9); p.recommended = str(10) == "1";
            curP = &p; curS = nullptr;
            break;
        }
        case 'g': if (curP) curP->genres.push_back(str(0)); break;
        case 'l': if (curP) curP->links.push_back(str(0)); break;
        case 'r': if (curP) curP->groups.push_back(str(0)); break;
        case 'i': if (curP && f.size() >= 5) curP->media.push_back({str(0), str(1), str(2), (int)num(3), (int)num(4)}); break;
        case 'V': {
            if (ev_.size() >= 60000) break;
            SpiEvent e;
            e.sid = (uint32_t)num(0); e.start = num(1); e.duration = (int)num(2); e.offMin = (int)num(3); e.progKey = str(4);
            ev_.insert(e);
            break;
        }
        case 'Q': if (s_.groups.size() < 5000) s_.groups.push_back({str(0), str(1), str(2), str(3), (int)num(4)}); break;
        default: break;
        }
    }
    if (lines >= 400000) DIAG_DEBUG(860967, "The cached programme guide has more than 400000 lines, so only the first ones were loaded.");
    s_.events.assign(ev_.begin(), ev_.end());
    st_.events = ev_.size();
    seq_++;
    return true;
}

namespace {
std::string cachePath(const std::string& dir, uint16_t eid) {
    char b[32];
    snprintf(b, sizeof b, "/spi-%04x.txt", (unsigned)eid);
    return dir + b;
}
}  // namespace

bool spiSave(const SpiStore& st, const std::string& dir, uint16_t eid) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string path = cachePath(dir, eid), tmp = path + ".tmp";
    const std::string txt = st.serialize();
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { DIAG_DEBUG(304514, "The programme guide cache %s could not be created, so the guide of this ensemble is not kept for next time.", tmp.c_str()); return false; }
        f.write(txt.data(), (std::streamsize)txt.size());
        if (!f) { DIAG_DEBUG(338999, "Writing %zu bytes to the programme guide cache %s failed, so the old cache stays.", txt.size(), tmp.c_str()); return false; }
    }
    fs::rename(tmp, path, ec);
    if (ec) DIAG_DEBUG(550079, "The programme guide cache %s could not replace %s (%s), so the old cache stays.", tmp.c_str(), path.c_str(), ec.message().c_str());
    return !ec;
}

bool spiLoad(SpiStore& st, const std::string& dir, uint16_t eid, int64_t utcNow, int maxAgeDays) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string path = cachePath(dir, eid);
    const auto mt = fs::last_write_time(path, ec);
    if (ec) return false;
    const auto age = std::chrono::duration_cast<std::chrono::hours>(fs::file_time_type::clock::now() - mt).count();
    if (age > (long long)maxAgeDays * 24) {
        DIAG_DEBUG(126843, "The programme guide cache %s is %lld hours old, more than %d days, so it is not loaded.", path.c_str(), (long long)age, maxAgeDays);
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) { DIAG_DEBUG(862612, "The programme guide cache %s exists but could not be opened, so the guide starts empty.", path.c_str()); return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    if (ss.str().size() > (48u << 20)) { DIAG_DEBUG(171883, "The programme guide cache %s is over 48 MB, so it is not loaded.", path.c_str()); return false; }
    if (!st.deserialize(ss.str())) return false;
    st.prune(utcNow);
    return true;
}

void spiCleanCache(const std::string& dir, int64_t, int maxAgeDays, size_t maxFiles) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    fs::directory_iterator dirIt(dir, ec);
    if (ec) DIAG_DEBUG(134748, "Reading the programme guide cache folder %s failed (%s), so no old cache files are removed.", dir.c_str(), ec.message().c_str());
    for (const auto& de : dirIt) {
        const std::string n = de.path().filename().string();
        if (n.compare(0, 4, "spi-") != 0 || n.size() < 8) continue;
        files.push_back({de.last_write_time(ec), de.path()});
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const auto now = fs::file_time_type::clock::now();
    for (size_t i = 0; i < files.size(); i++) {
        const auto age = std::chrono::duration_cast<std::chrono::hours>(now - files[i].first).count();
        if (i >= maxFiles || age > (long long)maxAgeDays * 24) {
            fs::remove(files[i].second, ec);
            if (ec) DIAG_DEBUG_EVERY(777298, 60.0, "The old programme guide cache file %s could not be removed (%s), so it stays on disk.", files[i].second.string().c_str(), ec.message().c_str());
        }

    }
}

}  // namespace dect2
