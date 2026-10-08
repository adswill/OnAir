// The shared channel database: see channel_db.h.
#include "dect2/channel_db.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace chdb {

namespace {

// the same text as scripts/ingest.py writes: three decimals without trailing zeros, at least one
std::string fmtFreq(double v) {
    char b[32]; snprintf(b, sizeof b, "%.3f", v);
    std::string s = b;
    while (s.size() > 2 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
    return s;
}
std::string fmtSnr(double v) { char b[32]; snprintf(b, sizeof b, "%.1f", v); return b; }
std::string join(const std::vector<std::string>& v, char sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) { if (i) s += sep; s += v[i]; }
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}
// services in the sent text must not contain the separator or line breaks
std::string cleanService(std::string s) {
    for (auto& c : s) if (c == '|' || c == '\n' || c == '\r' || c == '"') c = ' ';
    return trim(s);
}
bool sameChannel(const Row& a, const Row& b) { return std::fabs(a.freqMhz - b.freqMhz) <= 0.05; }

} // namespace

std::string csvQuote(const std::string& f) {
    if (f.find_first_of(",\"\r\n") == std::string::npos) return f;
    std::string o = "\"";
    for (char c : f) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

std::vector<std::vector<std::string>> csvParse(const std::string& t) {
    std::vector<std::vector<std::string>> out;
    std::vector<std::string> rec;
    std::string f;
    bool q = false, any = false;
    auto endRec = [&]() {
        rec.push_back(f); f.clear();
        if (!(rec.size() == 1 && rec[0].empty())) out.push_back(rec);
        rec.clear(); any = false;
    };
    for (size_t i = 0; i < t.size(); i++) {
        const char c = t[i];
        if (q) {
            if (c == '"') { if (i + 1 < t.size() && t[i + 1] == '"') { f += '"'; i++; } else q = false; }
            else f += c;
        } else if (c == '"') { q = true; any = true; }
        else if (c == ',') { rec.push_back(f); f.clear(); any = true; }
        else if (c == '\n') endRec();
        else if (c == '\r') {}
        else { f += c; any = true; }
    }
    if (any || !f.empty() || !rec.empty()) endRec();
    return out;
}

std::vector<std::string> splitServices(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s + std::string(1, sep)) {
        if (c == sep) { cur = trim(cur); if (!cur.empty()) v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    return v;
}

std::vector<Row> parseChannels(const std::string& csv) {
    std::vector<Row> rows;
    bool first = true;
    for (const auto& r : csvParse(csv)) {
        if (first) { first = false; if (!r.empty() && r[0] == "city") continue; }
        if (r.size() < 7) continue;
        Row w;
        w.city = r[0]; w.freqMhz = atof(r[1].c_str()); w.bwMhz = atof(r[2].c_str());
        w.standard = r[3]; w.network = r[4]; w.services = splitServices(r[5], '|'); w.snrDb = atof(r[6].c_str());
        w.reports = r.size() > 7 ? std::max(1, atoi(r[7].c_str())) : 1;
        w.firstSeen = r.size() > 8 ? r[8] : ""; w.lastSeen = r.size() > 9 ? r[9] : "";
        rows.push_back(w);
    }
    return rows;
}

std::string writeChannels(std::vector<Row> rows) {
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.city != b.city ? a.city < b.city : a.freqMhz < b.freqMhz; });
    std::string o = "city,freq_mhz,bw_mhz,standard,network,services,snr_db,reports,first_seen,last_seen\n";
    for (const auto& r : rows)
        o += csvQuote(r.city) + "," + fmtFreq(r.freqMhz) + "," + fmtFreq(r.bwMhz) + "," + csvQuote(r.standard) + "," + csvQuote(r.network) + "," +
             csvQuote(join(r.services, '|')) + "," + fmtSnr(r.snrDb) + "," + std::to_string(r.reports) + "," + r.firstSeen + "," + r.lastSeen + "\n";
    return o;
}

std::vector<City> parseCities(const std::string& csv) {
    std::vector<City> v;
    bool first = true;
    for (const auto& r : csvParse(csv)) {
        if (first) { first = false; if (!r.empty() && r[0] == "name") continue; }
        if (r.size() < 5) continue;
        City c; c.name = r[0]; c.admin1 = r[1]; c.lat = atof(r[2].c_str()); c.lon = atof(r[3].c_str()); c.population = atol(r[4].c_str());
        v.push_back(c);
    }
    return v;
}

std::vector<Country> parseCountries(const std::string& csv) {
    std::vector<Country> v;
    bool first = true;
    for (const auto& r : csvParse(csv)) {
        if (first) { first = false; if (!r.empty() && r[0] == "iso2") continue; }
        if (r.size() < 2) continue;
        v.push_back({r[0], r[1]});
    }
    return v;
}

void merge(std::vector<Row>& db, const std::string& city, const std::vector<Row>& in, const std::string& date) {
    std::vector<Row> uniq;   // one entry per channel of this submission, the best SNR
    for (const auto& r : in) {
        bool dup = false;
        for (auto& u : uniq) if (sameChannel(u, r)) {
            dup = true;
            if (r.snrDb > u.snrDb) u.snrDb = r.snrDb;
            for (const auto& s : r.services) if (std::find(u.services.begin(), u.services.end(), s) == u.services.end()) u.services.push_back(s);
        }
        if (!dup) uniq.push_back(r);
    }
    for (const auto& r : uniq) {
        Row* hit = nullptr;
        for (auto& d : db) if (d.city == city && sameChannel(d, r)) { hit = &d; break; }
        if (hit) {
            hit->reports++; hit->lastSeen = date;
            if (r.snrDb > hit->snrDb) hit->snrDb = r.snrDb;
            for (const auto& s : r.services) if (std::find(hit->services.begin(), hit->services.end(), s) == hit->services.end()) hit->services.push_back(s);
            if (hit->standard.empty()) hit->standard = r.standard;
            if (hit->network.empty()) hit->network = r.network;
            if (hit->bwMhz <= 0) hit->bwMhz = r.bwMhz;
        } else {
            Row n = r; n.city = city; n.reports = 1; n.firstSeen = date; n.lastSeen = date;
            db.push_back(n);
        }
    }
}

std::string issueBody(const Submission& s, bool withServices) {
    std::string o = "mode: " + s.mode + "\ncountry: " + s.country + "\ncity: " + s.city + "\n\n```csv\nfreq_mhz,bw_mhz,standard,network,services,snr_db\n";
    for (const auto& r : s.rows) {
        std::vector<std::string> sv;
        if (withServices) for (const auto& x : r.services) { const std::string c = cleanService(x); if (!c.empty()) sv.push_back(c); }
        o += fmtFreq(r.freqMhz) + "," + fmtFreq(r.bwMhz) + "," + csvQuote(r.standard) + "," + csvQuote(cleanService(r.network)) + "," + csvQuote(join(sv, '|')) + "," + fmtSnr(r.snrDb) + "\n";
    }
    return o + "```\n";
}

bool parseIssueBody(const std::string& body, Submission& out, std::string& err) {
    out = Submission();
    std::string csv;
    bool inBlock = false, haveBlock = false;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t e = body.find('\n', pos);
        if (e == std::string::npos) e = body.size();
        const std::string line = trim(body.substr(pos, e - pos));
        pos = e + 1;
        if (line.compare(0, 3, "```") == 0) {
            if (inBlock) { inBlock = false; haveBlock = true; break; }
            inBlock = true; continue;
        }
        if (inBlock) { csv += line + "\n"; continue; }
        auto key = [&](const char* k, std::string& dst) {
            const std::string p = std::string(k) + ":";
            if (dst.empty() && line.compare(0, p.size(), p) == 0) dst = trim(line.substr(p.size()));
        };
        key("mode", out.mode); key("country", out.country); key("city", out.city);
    }
    if (!haveBlock) { err = "no csv block found"; return false; }
    bool first = true;
    for (const auto& r : csvParse(csv)) {
        if (first) { first = false; if (!r.empty() && r[0] == "freq_mhz") continue; }
        if (r.size() < 6) { err = "a row has fewer than 6 fields"; return false; }
        Row w;
        w.freqMhz = atof(r[0].c_str()); w.bwMhz = atof(r[1].c_str()); w.standard = r[2]; w.network = r[3]; w.services = splitServices(r[4], '|'); w.snrDb = atof(r[5].c_str());
        out.rows.push_back(w);
    }
    if (out.mode.empty() || out.country.empty() || out.city.empty()) { err = "mode, country or city is missing"; return false; }
    return true;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string submissionTitle(const Submission& s) { return "[submit] " + s.mode + " " + s.country + " " + s.city; }

namespace {
size_t linkLen(const Submission& x, bool sv, const std::string& repo) { return submissionUrl(x, sv, repo).size(); }
}

std::string submissionUrl(const Submission& s, bool withServices, const std::string& repo) {
    return "https://github.com/" + repo + "/issues/new?template=submit.yml&title=" + urlEncode(submissionTitle(s)) + "&data=" + urlEncode(issueBody(s, withServices));
}

std::vector<Submission> submissionParts(const Submission& s, bool& withServices, size_t maxLen, const std::string& repo) {
    withServices = true;
    if (linkLen(s, true, repo) <= maxLen) return {s};
    withServices = false;
    if (linkLen(s, false, repo) <= maxLen) return {s};
    // split: as many rows as fit (without services) in each issue
    std::vector<Submission> out;
    Submission part = s; part.rows.clear();
    for (const auto& r : s.rows) {
        part.rows.push_back(r);
        if (linkLen(part, false, repo) > maxLen && part.rows.size() > 1) {
            part.rows.pop_back();
            out.push_back(part);
            part.rows.clear(); part.rows.push_back(r);
        }
    }
    if (!part.rows.empty()) out.push_back(part);
    return out;
}

std::vector<std::string> submissionUrls(const Submission& s, size_t maxLen, const std::string& repo) {
    bool sv = true;
    std::vector<std::string> out;
    for (const auto& p : submissionParts(s, sv, maxLen, repo)) out.push_back(submissionUrl(p, sv, repo));
    return out;
}

} // namespace chdb
} // namespace dect2
