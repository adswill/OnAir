// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
#include "dect2/ham/ham_flag.h"
#include <cstring>
#include <unordered_map>

namespace dect2::ham {
namespace {
struct NameIso { const char* name; const char* iso; };
// every entity of the built-in table (ham_dxcc_data.inc); a name that is a part of a country maps to that country
const NameIso kIso[] = {
    {"Canada","CA"},{"United States","US"},{"Alaska","US"},{"Hawaii","US"},{"Puerto Rico","PR"},{"US Virgin Islands","VI"},{"Guam","GU"},{"American Samoa","AS"},
    {"Mexico","MX"},{"Cuba","CU"},{"Dominican Republic","DO"},{"Haiti","HT"},{"Jamaica","JM"},{"Bahamas","BS"},{"Bermuda","BM"},{"Cayman Islands","KY"},
    {"Costa Rica","CR"},{"Panama","PA"},{"Guatemala","GT"},{"Honduras","HN"},{"El Salvador","SV"},{"Nicaragua","NI"},{"Belize","BZ"},{"Trinidad and Tobago","TT"},
    {"Barbados","BB"},{"Aruba","AW"},{"Curacao","CW"},{"Bonaire","BQ"},{"Guadeloupe","GP"},{"Martinique","MQ"},{"St. Lucia","LC"},{"Grenada","GD"},
    {"St. Vincent","VC"},{"Antigua and Barbuda","AG"},{"St. Kitts and Nevis","KN"},{"British Virgin Islands","VG"},{"Turks and Caicos","TC"},{"Greenland","GL"},
    {"St. Pierre and Miquelon","PM"},{"Argentina","AR"},{"Brazil","BR"},{"Chile","CL"},{"Easter Island","CL"},{"Colombia","CO"},{"Ecuador","EC"},{"Galapagos Islands","EC"},
    {"Peru","PE"},{"Venezuela","VE"},{"Uruguay","UY"},{"Paraguay","PY"},{"Bolivia","BO"},{"Guyana","GY"},{"Suriname","SR"},{"French Guiana","GF"},{"Falkland Islands","FK"},
    {"England","GB"},{"Scotland","GB"},{"Wales","GB"},{"Northern Ireland","GB"},{"Isle of Man","IM"},{"Guernsey","GG"},{"Jersey","JE"},{"Ireland","IE"},{"France","FR"},{"Corsica","FR"},
    {"Germany","DE"},{"Belgium","BE"},{"Netherlands","NL"},{"Luxembourg","LU"},{"Switzerland","CH"},{"Liechtenstein","LI"},{"Austria","AT"},{"Italy","IT"},{"Sardinia","IT"},
    {"Malta","MT"},{"San Marino","SM"},{"Vatican City","VA"},{"Monaco","MC"},{"Andorra","AD"},{"Spain","ES"},{"Balearic Islands","ES"},{"Canary Islands","ES"},{"Portugal","PT"},
    {"Azores","PT"},{"Madeira Islands","PT"},{"Gibraltar","GI"},{"Czech Republic","CZ"},{"Slovak Republic","SK"},{"Hungary","HU"},{"Poland","PL"},{"Romania","RO"},{"Bulgaria","BG"},
    {"Greece","GR"},{"Crete","GR"},{"Turkey","TR"},{"Cyprus","CY"},{"Serbia","RS"},{"Croatia","HR"},{"Slovenia","SI"},{"Bosnia-Herzegovina","BA"},{"North Macedonia","MK"},
    {"Montenegro","ME"},{"Kosovo","XK"},{"Albania","AL"},{"Moldova","MD"},{"Ukraine","UA"},{"Belarus","BY"},{"Lithuania","LT"},{"Latvia","LV"},{"Estonia","EE"},
    {"European Russia","RU"},{"Kaliningrad","RU"},{"Asiatic Russia","RU"},{"Finland","FI"},{"Aland Islands","AX"},{"Sweden","SE"},{"Norway","NO"},{"Svalbard","SJ"},
    {"Denmark","DK"},{"Faroe Islands","FO"},{"Iceland","IS"},{"Armenia","AM"},{"Azerbaijan","AZ"},{"Georgia","GE"},{"Kazakhstan","KZ"},{"Uzbekistan","UZ"},{"Tajikistan","TJ"},
    {"Turkmenistan","TM"},{"Kyrgyzstan","KG"},{"Japan","JP"},{"China","CN"},{"Taiwan","TW"},{"Hong Kong","HK"},{"Macao","MO"},{"Republic of Korea","KR"},{"DPR of Korea","KP"},
    {"Mongolia","MN"},{"India","IN"},{"Pakistan","PK"},{"Bangladesh","BD"},{"Sri Lanka","LK"},{"Nepal","NP"},{"Afghanistan","AF"},{"Maldives","MV"},{"Iran","IR"},{"Iraq","IQ"},
    {"Israel","IL"},{"Jordan","JO"},{"Lebanon","LB"},{"Syria","SY"},{"Saudi Arabia","SA"},{"United Arab Emirates","AE"},{"Qatar","QA"},{"Bahrain","BH"},{"Kuwait","KW"},
    {"Oman","OM"},{"Yemen","YE"},{"Thailand","TH"},{"Vietnam","VN"},{"Cambodia","KH"},{"Laos","LA"},{"Myanmar","MM"},{"Bhutan","BT"},{"West Malaysia","MY"},{"East Malaysia","MY"},
    {"Singapore","SG"},{"Indonesia","ID"},{"Philippines","PH"},{"Timor-Leste","TL"},{"Australia","AU"},{"New Zealand","NZ"},{"Fiji","FJ"},{"Tonga","TO"},{"Samoa","WS"},
    {"Vanuatu","VU"},{"Papua New Guinea","PG"},{"Solomon Islands","SB"},{"New Caledonia","NC"},{"French Polynesia","PF"},{"Western Kiribati","KI"},{"Marshall Islands","MH"},
    {"Micronesia","FM"},{"Palau","PW"},{"South Africa","ZA"},{"Namibia","NA"},{"Botswana","BW"},{"Zimbabwe","ZW"},{"Zambia","ZM"},{"Malawi","MW"},{"Mozambique","MZ"},
    {"Madagascar","MG"},{"Mauritius","MU"},{"Reunion Island","RE"},{"Seychelles","SC"},{"Kenya","KE"},{"Tanzania","TZ"},{"Uganda","UG"},{"Ethiopia","ET"},{"Sudan","SD"},
    {"Egypt","EG"},{"Libya","LY"},{"Tunisia","TN"},{"Algeria","DZ"},{"Morocco","MA"},{"Senegal","SN"},{"Cape Verde","CV"},{"Mali","ML"},{"Niger","NE"},{"Cote d'Ivoire","CI"},
    {"Ghana","GH"},{"Nigeria","NG"},{"Cameroon","CM"},{"Dem. Rep. of the Congo","CD"},{"Angola","AO"},
    // names a user's cty.dat uses for the same places
    {"Fed. Rep. of Germany","DE"},{"Czech Rep.","CZ"},{"Slovak Rep.","SK"},{"Republic of Korea","KR"},{"Russia","RU"},{"Bosnia and Herzegovina","BA"},
    {"Cote d Ivoire","CI"},{"Republic of the Congo","CG"},{"Ivory Coast","CI"},
};
}  // namespace

std::string entityIso(const std::string& name) {
    static const std::unordered_map<std::string, const char*> m = [] { std::unordered_map<std::string, const char*> r; for (const auto& e : kIso) r[e.name] = e.iso; return r; }();
    const auto it = m.find(name);
    return it == m.end() ? std::string() : std::string(it->second);
}

std::string callIso(const std::string& call, const DxccDb& db) {
    if (call.empty()) return "";
    const DxccHit h = db.lookup(call);
    if (!h.e) return "";
    std::string iso = entityIso(h.e->name);
    if (iso.empty() && &db != &DxccDb::builtin()) {   // a user file with another spelling of the name: the built-in table knows the same call
        const DxccHit b = DxccDb::builtin().lookup(call);
        if (b.e) iso = entityIso(b.e->name);
    }
    return iso;
}

std::string isoFlagEmoji(const std::string& iso) {
    if (iso.size() != 2 || iso[0] < 'A' || iso[0] > 'Z' || iso[1] < 'A' || iso[1] > 'Z') return "";
    std::string out;
    for (char c : iso) {
        const unsigned cp = 0x1F1E6u + (unsigned)(c - 'A');   // regional indicator A..Z: U+1F1E6 .. U+1F1FF, four bytes in UTF-8
        out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    }
    return out;
}

}  // namespace dect2::ham
