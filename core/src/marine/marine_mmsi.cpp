// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/marine/marine_mmsi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace dect2 {
namespace marine {

namespace {
struct Mid { int mid; const char* country; };
// ITU Maritime Identification Digits (List of MIDs, Appendix 43 of the Radio Regulations / ITU Operational Bulletin)
const Mid kMids[] = {
{201,"Albania"},{202,"Andorra"},{203,"Austria"},{204,"Azores (Portugal)"},{205,"Belgium"},{206,"Belarus"},{207,"Bulgaria"},{208,"Vatican"},
{209,"Cyprus"},{210,"Cyprus"},{211,"Germany"},{212,"Cyprus"},{213,"Georgia"},{214,"Moldova"},{215,"Malta"},{216,"Armenia"},{218,"Germany"},
{219,"Denmark"},{220,"Denmark"},{224,"Spain"},{225,"Spain"},{226,"France"},{227,"France"},{228,"France"},{229,"Malta"},{230,"Finland"},
{231,"Faroe Islands (Denmark)"},{232,"United Kingdom"},{233,"United Kingdom"},{234,"United Kingdom"},{235,"United Kingdom"},
{236,"Gibraltar"},{237,"Greece"},{238,"Croatia"},{239,"Greece"},{240,"Greece"},{241,"Greece"},{242,"Morocco"},{243,"Hungary"},
{244,"Netherlands"},{245,"Netherlands"},{246,"Netherlands"},{247,"Italy"},{248,"Malta"},{249,"Malta"},{250,"Ireland"},{251,"Iceland"},
{252,"Liechtenstein"},{253,"Luxembourg"},{254,"Monaco"},{255,"Madeira (Portugal)"},{256,"Malta"},{257,"Norway"},{258,"Norway"},
{259,"Norway"},{261,"Poland"},{262,"Montenegro"},{263,"Portugal"},{264,"Romania"},{265,"Sweden"},{266,"Sweden"},{267,"Slovakia"},
{268,"San Marino"},{269,"Switzerland"},{270,"Czech Republic"},{271,"Turkey"},{272,"Ukraine"},{273,"Russia"},{274,"North Macedonia"},
{275,"Latvia"},{276,"Estonia"},{277,"Lithuania"},{278,"Slovenia"},{279,"Serbia"},
{301,"Anguilla (UK)"},{303,"Alaska (USA)"},{304,"Antigua and Barbuda"},{305,"Antigua and Barbuda"},{306,"Netherlands Antilles"},
{307,"Aruba"},{308,"Bahamas"},{309,"Bahamas"},{310,"Bermuda"},{311,"Bahamas"},{312,"Belize"},{314,"Barbados"},{316,"Canada"},
{319,"Cayman Islands"},{321,"Costa Rica"},{323,"Cuba"},{325,"Dominica"},{327,"Dominican Republic"},{329,"Guadeloupe (France)"},
{330,"Grenada"},{331,"Greenland (Denmark)"},{332,"Guatemala"},{334,"Honduras"},{336,"Haiti"},{338,"United States"},{339,"Jamaica"},
{341,"Saint Kitts and Nevis"},{343,"Saint Lucia"},{345,"Mexico"},{347,"Martinique (France)"},{348,"Montserrat (UK)"},{350,"Nicaragua"},
{351,"Panama"},{352,"Panama"},{353,"Panama"},{354,"Panama"},{355,"Panama"},{356,"Panama"},{357,"Panama"},{358,"Puerto Rico (USA)"},
{359,"El Salvador"},{361,"Saint Pierre and Miquelon (France)"},{362,"Trinidad and Tobago"},{364,"Turks and Caicos Islands (UK)"},
{366,"United States"},{367,"United States"},{368,"United States"},{369,"United States"},{370,"Panama"},{371,"Panama"},{372,"Panama"},
{373,"Panama"},{374,"Panama"},{375,"Saint Vincent and the Grenadines"},{376,"Saint Vincent and the Grenadines"},
{377,"Saint Vincent and the Grenadines"},{378,"British Virgin Islands (UK)"},{379,"US Virgin Islands (USA)"},
{401,"Afghanistan"},{403,"Saudi Arabia"},{405,"Bangladesh"},{408,"Bahrain"},{410,"Bhutan"},{412,"China"},{413,"China"},{414,"China"},
{416,"Taiwan"},{417,"Sri Lanka"},{419,"India"},{422,"Iran"},{423,"Azerbaijan"},{425,"Iraq"},{428,"Israel"},{431,"Japan"},{432,"Japan"},
{434,"Turkmenistan"},{436,"Kazakhstan"},{437,"Uzbekistan"},{438,"Jordan"},{440,"South Korea"},{441,"South Korea"},{443,"Palestine"},
{445,"North Korea"},{447,"Kuwait"},{450,"Lebanon"},{451,"Kyrgyzstan"},{453,"Macao (China)"},{455,"Maldives"},{457,"Mongolia"},
{459,"Nepal"},{461,"Oman"},{463,"Pakistan"},{466,"Qatar"},{468,"Syria"},{470,"United Arab Emirates"},{471,"United Arab Emirates"},
{472,"Tajikistan"},{473,"Yemen"},{475,"Yemen"},{477,"Hong Kong (China)"},{478,"Bosnia and Herzegovina"},
{501,"Adelie Land (France)"},{503,"Australia"},{506,"Myanmar"},{508,"Brunei"},{510,"Micronesia"},{511,"Palau"},{512,"New Zealand"},
{514,"Cambodia"},{515,"Cambodia"},{516,"Christmas Island (Australia)"},{518,"Cook Islands"},{520,"Fiji"},{523,"Cocos Islands (Australia)"},
{525,"Indonesia"},{529,"Kiribati"},{531,"Laos"},{533,"Malaysia"},{536,"Northern Mariana Islands (USA)"},{538,"Marshall Islands"},
{540,"New Caledonia (France)"},{542,"Niue"},{544,"Nauru"},{546,"French Polynesia"},{548,"Philippines"},{553,"Papua New Guinea"},
{555,"Pitcairn Island (UK)"},{557,"Solomon Islands"},{559,"American Samoa (USA)"},{561,"Samoa"},{563,"Singapore"},{564,"Singapore"},
{565,"Singapore"},{566,"Singapore"},{567,"Thailand"},{570,"Tonga"},{572,"Tuvalu"},{574,"Vietnam"},{576,"Vanuatu"},{577,"Vanuatu"},
{578,"Wallis and Futuna (France)"},
{601,"South Africa"},{603,"Angola"},{605,"Algeria"},{607,"Saint Paul and Amsterdam Islands (France)"},{608,"Ascension Island (UK)"},
{609,"Burundi"},{610,"Benin"},{611,"Botswana"},{612,"Central African Republic"},{613,"Cameroon"},{615,"Congo"},{616,"Comoros"},
{617,"Cape Verde"},{618,"Crozet Archipelago (France)"},{619,"Ivory Coast"},{620,"Comoros"},{621,"Djibouti"},{622,"Egypt"},
{624,"Ethiopia"},{625,"Eritrea"},{626,"Gabon"},{627,"Ghana"},{629,"Gambia"},{630,"Guinea-Bissau"},{631,"Equatorial Guinea"},
{632,"Guinea"},{633,"Burkina Faso"},{634,"Kenya"},{635,"Kerguelen Islands (France)"},{636,"Liberia"},{637,"Liberia"},{638,"South Sudan"},
{642,"Libya"},{644,"Lesotho"},{645,"Mauritius"},{647,"Madagascar"},{649,"Mali"},{650,"Mozambique"},{654,"Mauritania"},{655,"Malawi"},
{656,"Niger"},{657,"Nigeria"},{659,"Namibia"},{660,"Reunion (France)"},{661,"Rwanda"},{662,"Sudan"},{663,"Senegal"},{664,"Seychelles"},
{665,"Saint Helena (UK)"},{666,"Somalia"},{667,"Sierra Leone"},{668,"Sao Tome and Principe"},{669,"Eswatini"},{670,"Chad"},{671,"Togo"},
{672,"Tunisia"},{674,"Tanzania"},{675,"Uganda"},{676,"DR Congo"},{677,"Tanzania"},{678,"Zambia"},{679,"Zimbabwe"},
{701,"Argentina"},{710,"Brazil"},{720,"Bolivia"},{725,"Chile"},{730,"Colombia"},{735,"Ecuador"},{740,"Falkland Islands (UK)"},
{745,"Guiana (France)"},{750,"Guyana"},{755,"Paraguay"},{760,"Peru"},{765,"Suriname"},{770,"Uruguay"},{775,"Venezuela"},
};
} // namespace

const char* midCountry(int mid) {
    for (const Mid& m : kMids) if (m.mid == mid) return m.country;
    return "";
}

const char* mmsiKindName(MmsiKind k) {
    switch (k) {
    case MmsiKind::Ship: return "ship";
    case MmsiKind::CoastStation: return "coast station";
    case MmsiKind::Group: return "group";
    case MmsiKind::SarAircraft: return "SAR aircraft";
    case MmsiKind::Handheld: return "handheld VHF";
    case MmsiKind::CraftAssociated: return "craft of a parent ship";
    case MmsiKind::AisSart: return "AIS-SART";
    case MmsiKind::Mob: return "man overboard device";
    case MmsiKind::EpirbAis: return "EPIRB-AIS";
    case MmsiKind::AtoN: return "aid to navigation";
    case MmsiKind::Special: return "special";
    default: return "";
    }
}

MmsiInfo mmsiInfo(const std::string& m) {
    MmsiInfo r;
    if (m.size() != 9) return r;
    for (char c : m) { if (c == '~') r.partial = true; else if (c < '0' || c > '9') return r; }
    auto dig = [&](size_t a, size_t n, int& v) { v = 0; for (size_t i = a; i < a + n; i++) { if (m[i] == '~') return false; v = v * 10 + (m[i] - '0'); } return true; };
    int mid = 0;
    auto setMid = [&](size_t a) { if (dig(a, 3, mid)) { r.mid = mid; r.country = midCountry(mid); } };
    if (m.compare(0, 2, "00") == 0) { r.kind = MmsiKind::CoastStation; setMid(2); }
    else if (m[0] == '0') { r.kind = MmsiKind::Group; setMid(1); }
    else if (m.compare(0, 3, "111") == 0) { r.kind = MmsiKind::SarAircraft; setMid(3); }
    else if (m.compare(0, 2, "99") == 0) { r.kind = MmsiKind::AtoN; setMid(2); }
    else if (m.compare(0, 2, "98") == 0) { r.kind = MmsiKind::CraftAssociated; setMid(2); }
    else if (m.compare(0, 3, "970") == 0) r.kind = MmsiKind::AisSart;
    else if (m.compare(0, 3, "972") == 0) r.kind = MmsiKind::Mob;
    else if (m.compare(0, 3, "974") == 0) r.kind = MmsiKind::EpirbAis;
    else if (m[0] == '8') { r.kind = MmsiKind::Handheld; setMid(1); }
    else if (m[0] >= '2' && m[0] <= '7') { r.kind = MmsiKind::Ship; setMid(0); }
    return r;
}

namespace {
std::string trimS(std::string v) {
    size_t a = 0, b = v.size();
    while (a < b && (v[a] == ' ' || v[a] == '\t' || v[a] == '\r' || v[a] == '"')) a++;
    while (b > a && (v[b - 1] == ' ' || v[b - 1] == '\t' || v[b - 1] == '\r' || v[b - 1] == '"')) b--;
    return v.substr(a, b - a);
}
std::vector<std::string> splitRow(const std::string& line, char d) {
    std::vector<std::string> r;
    std::string cur;
    if (d == ' ') {     // fixed-width text: two or more spaces between the fields
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == ' ' && i + 1 < line.size() && line[i + 1] == ' ') {
                r.push_back(trimS(cur)); cur.clear();
                while (i + 1 < line.size() && line[i + 1] == ' ') i++;
            } else cur += line[i];
        }
        r.push_back(trimS(cur));
        return r;
    }
    bool q = false;
    for (char c : line) {
        if (c == '"') q = !q;
        else if (c == d && !q) { r.push_back(trimS(cur)); cur.clear(); }
        else cur += c;
    }
    r.push_back(trimS(cur));
    return r;
}
std::string lowerS(std::string v) { for (char& c : v) c = (char)tolower((unsigned char)c); return v; }
bool numberOf(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* e = nullptr;
    v = strtod(s.c_str(), &e);
    return e && *e == 0;
}
bool isHemi(char c) { return c == 'N' || c == 'S' || c == 'E' || c == 'W' || c == 'n' || c == 's' || c == 'e' || c == 'w'; }
} // namespace

std::string normaliseMmsi(const std::string& in) {
    std::string d;
    for (char c : in) {
        if (c >= '0' && c <= '9') d += c;
        else if (c == ' ' || c == '\'' || c == '-' || c == '.' || c == '\t' || c == '"' || c == '\r') continue;      // "232 123 456", a spreadsheet's leading apostrophe
        else return "";
    }
    if (d.size() == 9) return d;
    if (d.size() == 7 || d.size() == 8) {      // a spreadsheet dropped the leading zeros of a coast station (00MID) or group (0MID) number
        const std::string p = std::string(9 - d.size(), '0') + d;
        const MmsiInfo mi = mmsiInfo(p); const MmsiKind k = mi.country.empty() ? MmsiKind::Unknown : mi.kind;
        if (k == MmsiKind::CoastStation || k == MmsiKind::Group) return p;
    }
    return "";
}

bool parseCoordinate(const std::string& s, double& out, char* hemiOut, bool strict) {
    char hemi = 0;
    bool neg = false, seenDigit = false;
    const bool comma = s.find(',') != std::string::npos && s.find('.') == std::string::npos;
    std::vector<std::string> tok;
    std::string cur;
    auto flush = [&] { if (!cur.empty()) { tok.push_back(cur); cur.clear(); } };
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if ((c >= '0' && c <= '9') || c == '.') { cur += (char)c; seenDigit = true; }
        else if (c == ',' && comma && !cur.empty() && i + 1 < s.size() && isdigit((unsigned char)s[i + 1]) && tok.empty()) cur += '.';   // decimal comma
        else if (isHemi((char)c)) {
            const char h = (char)toupper(c);
            if (hemi && hemi != h) return false;
            hemi = h; flush();
        }
        else if (c == '-' || c == '+') { if (!seenDigit && c == '-') neg = true; flush(); }
        else if (c == ' ' || c == '\t' || c == '\'' || c == '"' || c == ':' || c == ',' || c == '`') flush();
        else if (c == 0xC2 && i + 1 < s.size() && ((unsigned char)s[i + 1] == 0xB0 || (unsigned char)s[i + 1] == 0xBA)) { flush(); i++; }     // degree sign
        else if (c == 0xE2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80) { flush(); i += 2; }                                         // prime marks
        else return false;
    }
    flush();
    if (tok.empty() || tok.size() > 3) return false;
    double v[3] = {0, 0, 0};
    for (size_t i = 0; i < tok.size(); i++) if (!numberOf(tok[i], v[i])) return false;
    double deg;
    const bool plainInt = tok.size() == 1 && tok[0].find('.') == std::string::npos;
    if (plainInt && hemi && tok[0].size() >= 4) {          // 5130N, 00312W, 513012N: ddmm[ss] / dddmm[ss]
        const bool lat = hemi == 'N' || hemi == 'S';
        const size_t dl = lat ? 2 : 3;
        const std::string& t = tok[0];
        if (t.size() != dl + 2 && t.size() != dl + 4) return false;
        const double d = atof(t.substr(0, dl).c_str()), m = atof(t.substr(dl, 2).c_str()), sec = t.size() > dl + 2 ? atof(t.substr(dl + 2, 2).c_str()) : 0;
        if (m >= 60 || sec >= 60) return false;
        deg = d + m / 60 + sec / 3600;
    } else if (tok.size() == 1) {
        if (strict && !hemi && tok[0].find('.') == std::string::npos) return false;
        deg = v[0];
    } else {
        if (tok[0].find('.') != std::string::npos || v[1] >= 60 || (tok.size() == 3 && (v[2] >= 60 || tok[1].find('.') != std::string::npos))) return false;
        deg = v[0] + v[1] / 60 + v[2] / 3600;
    }
    if (hemi == 'S' || hemi == 'W') deg = -std::fabs(deg);
    else if (neg) deg = -deg;
    out = deg;
    if (hemiOut) *hemiOut = hemi;
    return true;
}

bool parsePosition(const std::string& in, double& lat, double& lon) {
    std::vector<size_t> hi;
    for (size_t i = 0; i < in.size(); i++) {
        if (!isHemi(in[i])) continue;
        const bool lb = i > 0 && isalpha((unsigned char)in[i - 1]), la = i + 1 < in.size() && isalpha((unsigned char)in[i + 1]);
        if (!lb && !la) hi.push_back(i);
    }
    std::string a, b;
    if (hi.size() == 2) {
        const size_t firstDigit = in.find_first_of("0123456789");
        if (firstDigit == std::string::npos) return false;
        const size_t cut = hi[0] < firstDigit ? hi[1] : hi[0] + 1;         // "N 51 30 E 003 12" or "51 30 N 003 12 W"
        a = in.substr(0, cut); b = in.substr(cut);
    } else if (hi.empty()) {
        std::string t = in;
        for (size_t p; (p = t.find(", ")) != std::string::npos;) t.replace(p, 2, "/");
        for (char& c : t) if (c == ';') c = '/';
        const size_t sl = t.find('/');
        if (sl != std::string::npos) { a = t.substr(0, sl); b = t.substr(sl + 1); }
        else {
            const std::string tt = trimS(t);
            const size_t sp = tt.find(' ');
            if (sp == std::string::npos || tt.find(' ', tt.find_first_not_of(' ', sp)) != std::string::npos) return false;
            a = tt.substr(0, sp); b = tt.substr(sp + 1);
        }
    } else return false;
    double x, y; char hx = 0, hy = 0;
    if (!parseCoordinate(a, x, &hx) || !parseCoordinate(b, y, &hy)) return false;
    if (hx == 'E' || hx == 'W' || hy == 'N' || hy == 'S') std::swap(x, y);
    if (std::fabs(x) > 90 || std::fabs(y) > 180) return false;
    lat = x; lon = y;
    return true;
}

namespace {
struct Cols { int mmsi = -1, name = -1, call = -1, type = -1, country = -1, station = -1, loc = -1, lat = -1, lon = -1, pos = -1; };

// header words (lower case, letters and digits only) to the role of the column they name
bool headerRole(const std::string& raw, int& role) {
    std::string h;
    for (char c : lowerS(raw)) if (isalnum((unsigned char)c)) h += c;
    static const struct { const char* w; int r; } t[] = {
        {"mmsi", 0}, {"mmsinumber", 0}, {"mmsino", 0}, {"dscid", 0}, {"dscnumber", 0}, {"id", 0}, {"identity", 0},
        {"name", 1}, {"shipname", 1}, {"vessel", 1}, {"vesselname", 1}, {"shipsname", 1}, {"nameofship", 1},
        {"stationname", 5}, {"nameofstation", 5}, {"coaststation", 5}, {"coast", 5}, {"station", 5},
        {"callsign", 2}, {"call", 2}, {"callsigns", 2}, {"callletters", 2}, {"radiocallsign", 2},
        {"type", 3}, {"shiptype", 3}, {"vesseltype", 3}, {"class", 3}, {"category", 3}, {"kind", 3},
        {"country", 4}, {"flag", 4}, {"nation", 4}, {"flagstate", 4}, {"countrycode", 4},
        {"location", 6}, {"place", 6}, {"city", 6}, {"site", 6}, {"town", 6}, {"qth", 6},
        {"lat", 7}, {"latitude", 7}, {"lon", 8}, {"lng", 8}, {"long", 8}, {"longitude", 8},
        {"position", 9}, {"pos", 9}, {"coordinates", 9}, {"coords", 9}, {"latlon", 9}, {"latlong", 9}, {"latitudelongitude", 9}, {"geo", 9}};
    for (const auto& e : t) if (h == e.w) { role = e.r; return true; }
    return false;
}

char detectDelimiter(const std::vector<std::string>& lines) {
    const size_t n = std::min<size_t>(lines.size(), 12);
    const char cand[] = {'\t', ';', '|', ',', ' '};
    for (char d : cand) {
        std::vector<size_t> cnt;
        for (size_t i = 0; i < n; i++) cnt.push_back(splitRow(lines[i], d).size() - 1);
        size_t most = 0; for (size_t c : cnt) most = std::max(most, c);
        if (most == 0) continue;
        size_t good = 0; for (size_t c : cnt) if (c >= 1) good++;      // data rows may miss trailing fields
        if (good * 2 >= n) return d;
    }
    return ',';
}

struct Parsed {
    char delim = ',';
    bool header = false;
    Cols cols;
    std::vector<std::vector<std::string>> rows;     // data rows (header excluded)
    std::vector<std::string> head;
};

Parsed parseTable(const std::string& text) {
    Parsed P;
    std::vector<std::string> lines;
    for (size_t p = 0; p < text.size();) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(p, e - p);
        p = e + 1;
        if (!l.empty() && l[0] == '\xEF' && l.size() >= 3) l.erase(0, 3);       // byte order mark
        if (trimS(l).empty() || trimS(l)[0] == '#') continue;
        lines.push_back(l);
    }
    if (lines.empty()) return P;
    P.delim = detectDelimiter(lines);
    std::vector<std::vector<std::string>> all;
    for (const auto& l : lines) all.push_back(splitRow(l, P.delim));
    // header: no cell of the first row is an MMSI, and at least one names a column we know
    bool anyMmsi = false, anyRole = false;
    Cols c;
    for (size_t i = 0; i < all[0].size(); i++) {
        if (!normaliseMmsi(all[0][i]).empty()) anyMmsi = true;
        int role;
        if (headerRole(all[0][i], role)) {
            anyRole = true;
            int* dst[] = {&c.mmsi, &c.name, &c.call, &c.type, &c.country, &c.station, &c.loc, &c.lat, &c.lon, &c.pos};
            if (*dst[role] < 0) *dst[role] = (int)i;
        }
    }
    if (!anyMmsi && anyRole) { P.header = true; P.head = all[0]; P.cols = c; all.erase(all.begin()); }
    P.rows = all;
    Cols& k = P.cols;
    size_t ncol = P.header ? P.head.size() : 0;
    for (const auto& r : P.rows) ncol = std::max(ncol, r.size());
    const size_t sample = std::min<size_t>(P.rows.size(), 40);
    auto frac = [&](size_t col, int what) {      // share of the sample rows that read as: 0 MMSI, 1 coordinate (strict), 2 position pair
        size_t ok = 0, seen = 0;
        for (size_t i = 0; i < sample; i++) {
            if (col >= P.rows[i].size() || P.rows[i][col].empty()) continue;
            seen++;
            const std::string& s = P.rows[i][col];
            double a, b;
            if (what == 0 && !normaliseMmsi(s).empty()) ok++;
            else if (what == 1 && parseCoordinate(s, a, nullptr, true)) ok++;
            else if (what == 2 && parsePosition(s, a, b)) ok++;
        }
        return seen ? (double)ok / (double)seen : 0.0;
    };
    std::vector<bool> used(ncol, false);
    auto take = [&](int c2) { if (c2 >= 0 && (size_t)c2 < ncol) used[(size_t)c2] = true; };
    if (k.mmsi < 0) {      // find the column that holds the numbers
        double best = 0.6;
        for (size_t i = 0; i < ncol; i++) { const double f = frac(i, 0); if (f > best) { best = f; k.mmsi = (int)i; } }
    }
    if (k.mmsi < 0) return P;
    take(k.mmsi); take(k.name); take(k.call); take(k.type); take(k.country); take(k.station); take(k.loc); take(k.lat); take(k.lon); take(k.pos);
    // a "location" column that holds a position is one
    if (k.loc >= 0 && k.pos < 0 && frac((size_t)k.loc, 2) >= 0.6) { k.pos = k.loc; k.loc = -1; }
    if (k.pos < 0 && (k.lat < 0 || k.lon < 0)) {
        for (size_t i = 0; i < ncol; i++) if (!used[i] && frac(i, 2) >= 0.6 && frac(i, 1) < 0.6) { k.pos = (int)i; take(k.pos); break; }
    }
    if (k.pos < 0 && (k.lat < 0 || k.lon < 0)) {
        std::vector<size_t> cc;
        for (size_t i = 0; i < ncol; i++) if (!used[i] && frac(i, 1) >= 0.6) cc.push_back(i);
        if (cc.size() >= 2) {
            // decide which is which: a letter E/W or a value past 90 makes a longitude, else the first is the latitude
            auto isLon = [&](size_t col) {
                for (size_t i = 0; i < sample; i++) { double v; char h = 0; if (col < P.rows[i].size() && parseCoordinate(P.rows[i][col], v, &h, true) && (h == 'E' || h == 'W' || std::fabs(v) > 90)) return true; }
                return false; };
            size_t a = cc[0], b = cc[1];
            if (isLon(a) && !isLon(b)) std::swap(a, b);
            k.lat = (int)a; k.lon = (int)b; take(k.lat); take(k.lon);
        }
    }
    if (!P.header) {      // headerless: the text columns left, in the order name, call sign, type, country, station, location
        int* order[] = {&k.name, &k.call, &k.type, &k.country, &k.station, &k.loc};
        size_t oi = 0;
        for (size_t i = 0; i < ncol && oi < 6; i++) {
            if (used[i]) continue;
            *order[oi++] = (int)i; used[i] = true;
        }
    }
    return P;
}
} // namespace

ImportPreview ShipDb::preview(const std::string& text, size_t maxRows) {
    ImportPreview pv;
    const Parsed P = parseTable(text);
    pv.delim = P.delim;
    pv.header = P.header;
    if (P.cols.mmsi < 0) return pv;
    pv.ok = true;
    static const char* const names[] = {"mmsi", "name", "callsign", "type", "country", "station", "location", "lat", "lon", "position"};
    const int idx[] = {P.cols.mmsi, P.cols.name, P.cols.call, P.cols.type, P.cols.country, P.cols.station, P.cols.loc, P.cols.lat, P.cols.lon, P.cols.pos};
    size_t ncol = P.head.size();
    for (const auto& r : P.rows) ncol = std::max(ncol, r.size());
    pv.columns.assign(ncol, "");
    for (int i = 0; i < 10; i++) if (idx[i] >= 0 && (size_t)idx[i] < ncol) pv.columns[(size_t)idx[i]] = names[i];
    for (size_t i = 0; i < P.rows.size() && i < maxRows; i++) pv.rows.push_back(P.rows[i]);
    for (const auto& r : P.rows) if ((size_t)P.cols.mmsi < r.size() && !normaliseMmsi(r[(size_t)P.cols.mmsi]).empty()) pv.records++;
    return pv;
}

int ShipDb::importText(const std::string& text) { return importDetailed(text).read; }

ImportStats ShipDb::importDetailed(const std::string& text) {
    ImportStats st;
    const Parsed P = parseTable(text);
    const Cols& k = P.cols;
    if (k.mmsi < 0) return st;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& c : P.rows) {
        auto at = [&](int i) { return i >= 0 && (size_t)i < c.size() ? c[(size_t)i] : std::string(); };
        const std::string mmsi = normaliseMmsi(at(k.mmsi));
        if (mmsi.empty()) { st.skipped++; continue; }
        ShipRec r;
        auto it = m_.find(mmsi);
        const bool existed = it != m_.end();
        if (existed) r = it->second;
        const ShipRec before = r;
        r.mmsi = mmsi;
        auto set = [&](std::string& dst, int i) { const std::string v = at(i); if (!v.empty()) dst = v.substr(0, 60); };     // an empty cell never erases what is known
        set(r.name, k.name); set(r.callsign, k.call); set(r.type, k.type); set(r.country, k.country); set(r.station, k.station); set(r.location, k.loc);
        double la = 0, lo = 0;
        bool gotPos = false;
        if (k.pos >= 0) gotPos = parsePosition(at(k.pos), la, lo);
        if (!gotPos && k.lat >= 0 && k.lon >= 0) gotPos = parseCoordinate(at(k.lat), la) && parseCoordinate(at(k.lon), lo) && std::fabs(la) <= 90 && std::fabs(lo) <= 180;
        if (gotPos && !(la == 0 && lo == 0)) { r.hasPos = true; r.lat = la; r.lon = lo; }
        if (r.name.empty() && r.callsign.empty() && r.type.empty() && r.country.empty() && r.station.empty() && r.location.empty() && !r.hasPos) { st.skipped++; continue; }   // an MMSI alone carries nothing
        r.source = 1;
        const bool changed = !existed || before.name != r.name || before.callsign != r.callsign || before.type != r.type || before.country != r.country || before.station != r.station ||
                             before.location != r.location || before.hasPos != r.hasPos || before.lat != r.lat || before.lon != r.lon;
        m_[mmsi] = r;
        st.read++;
        if (!existed) st.added++; else if (changed) st.updated++; else st.unchanged++;
        if (r.hasPos) st.withPos++;
        const MmsiKind kd = mmsiInfo(mmsi).kind;
        if (kd == MmsiKind::CoastStation) st.coast++; else if (kd == MmsiKind::Ship) st.ships++;
    }
    if (st.read) { ver_++; dirty_ = true; }
    return st;
}

std::string ShipDb::exportTsv() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<const ShipRec*> v;
    for (const auto& kv : m_) v.push_back(&kv.second);
    std::sort(v.begin(), v.end(), [](const ShipRec* a, const ShipRec* b) { return a->mmsi < b->mmsi; });
    std::string o = "mmsi\tname\tcallsign\ttype\tcountry\tstation\tlocation\tlat\tlon\n";
    char b[48];
    for (const ShipRec* r : v) {
        std::string la, lo;
        if (r->hasPos) { snprintf(b, sizeof b, "%.5f", r->lat); la = b; snprintf(b, sizeof b, "%.5f", r->lon); lo = b; }
        auto clean = [](std::string s) { for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; };
        o += r->mmsi + "\t" + clean(r->name) + "\t" + clean(r->callsign) + "\t" + clean(r->type) + "\t" + clean(r->country) + "\t" + clean(r->station) + "\t" + clean(r->location) + "\t" + la + "\t" + lo + "\n";
    }
    return o;
}

void ShipDb::learn(const std::string& mmsi, const std::string& name, const std::string& callsign, const std::string& type) {
    if (mmsi.size() != 9 || normaliseMmsi(mmsi) != mmsi || (name.empty() && callsign.empty() && type.empty())) return;
    std::lock_guard<std::mutex> lk(mu_);
    ShipRec& r = m_[mmsi];
    const ShipRec before = r;
    r.mmsi = mmsi;
    auto upd = [&](std::string& dst, const std::string& v) { if (!v.empty() && (r.source == 0 || dst.empty())) dst = v; };
    upd(r.name, name); upd(r.callsign, callsign); upd(r.type, type);
    if (r.country.empty()) r.country = mmsiInfo(mmsi).country;
    if (before.name != r.name || before.callsign != r.callsign || before.type != r.type || before.country != r.country) { ver_++; dirty_ = true; }
}

bool ShipDb::find(const std::string& mmsi, ShipRec& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = m_.find(mmsi);
    if (it == m_.end()) return false;
    out = it->second;
    return true;
}

std::string ShipDb::name(const std::string& mmsi) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = m_.find(mmsi);
    if (it == m_.end()) return "";
    const ShipRec& r = it->second;
    std::string n = !r.name.empty() ? r.name : r.station;
    if (!r.location.empty() && !n.empty() && n.find(r.location) == std::string::npos) n += ", " + r.location;
    return n;
}

std::vector<ShipRec> ShipDb::positioned() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ShipRec> v;
    for (const auto& kv : m_) if (kv.second.hasPos) v.push_back(kv.second);
    std::sort(v.begin(), v.end(), [](const ShipRec& a, const ShipRec& b) { return a.mmsi < b.mmsi; });
    return v;
}

size_t ShipDb::size() const { std::lock_guard<std::mutex> lk(mu_); return m_.size(); }
uint64_t ShipDb::version() const { std::lock_guard<std::mutex> lk(mu_); return ver_; }
bool ShipDb::takeDirty() { std::lock_guard<std::mutex> lk(mu_); const bool d = dirty_; dirty_ = false; return d; }
void ShipDb::clear() { std::lock_guard<std::mutex> lk(mu_); m_.clear(); ver_++; dirty_ = true; }
ShipDb& ShipDb::global() { static ShipDb g; return g; }

} // namespace marine
} // namespace dect2
