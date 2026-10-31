// COSPAS-SARSAT 406 MHz message layer (see sarsat_proto.h).
#include "dect2/sarsat/sarsat_proto.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace dect2::sarsat {

namespace {

constexpr uint32_t kGen1 = 0x26D9E3;   // x^21 ... : BCH(82,61), bits 25-85 protected by 86-106
constexpr uint32_t kGen2 = 0x1539;     // x^12 ... : BCH(38,26), bits 107-132 protected by 133-144

uint64_t get(const std::vector<uint8_t>& b, int first, int last) {   // bits first..last, 1-based, MSB first
    uint64_t v = 0;
    for (int i = first; i <= last; i++) v = (v << 1) | (i - 1 < (int)b.size() ? (b[(size_t)i - 1] & 1u) : 0u);
    return v;
}
void put(std::vector<uint8_t>& b, int first, int last, uint64_t v) {
    for (int i = last; i >= first; i--) { b[(size_t)i - 1] = (uint8_t)(v & 1); v >>= 1; }
}

// syndrome table of a shortened BCH code: error positions (in the code word, 0 = first bit sent) for every pattern of up to t errors
struct Bch {
    uint32_t gen; int deg, n, t;
    std::unordered_map<uint32_t, uint32_t> tab;   // syndrome -> count << 24 | p0 | p1 << 8 | p2 << 16
    Bch(uint32_t g, int d, int nn, int tt) : gen(g), deg(d), n(nn), t(tt) {
        std::vector<uint32_t> single((size_t)n);
        for (int j = 0; j < n; j++) {
            std::vector<uint8_t> w((size_t)n, 0);
            w[(size_t)j] = 1;
            single[(size_t)j] = bchRemainder(w, 1, n, gen, deg);
        }
        for (int a = 0; a < n; a++) {
            tab[single[(size_t)a]] = (1u << 24) | (uint32_t)a;
            if (t < 2) continue;
            for (int b = a + 1; b < n; b++) {
                const uint32_t s2 = single[(size_t)a] ^ single[(size_t)b];
                tab.emplace(s2, (2u << 24) | (uint32_t)a | ((uint32_t)b << 8));
                if (t < 3) continue;
                for (int c = b + 1; c < n; c++) tab.emplace(s2 ^ single[(size_t)c], (3u << 24) | (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16));
            }
        }
    }
    // returns the number of bits repaired, or -1 when the word cannot be repaired
    int correct(std::vector<uint8_t>& bits, int first) const {   // the code word is bits first .. first+n-1
        const uint32_t s = bchRemainder(bits, first, first + n - 1, gen, deg);
        if (s == 0) return 0;
        const auto it = tab.find(s);
        if (it == tab.end()) return -1;
        const int cnt = (int)(it->second >> 24);
        for (int k = 0; k < cnt; k++) {
            const int p = (int)((it->second >> (8 * k)) & 0xFF);
            bits[(size_t)(first - 1 + p)] ^= 1;
        }
        return cnt;
    }
};
const Bch& bch1() { static const Bch b(kGen1, 21, 82, 3); return b; }
const Bch& bch2() { static const Bch b(kGen2, 12, 38, 2); return b; }

struct MidRange { int a, b; const char* name; };
const MidRange kMid[] = {
    {201, 201, "Albania"}, {202, 202, "Andorra"}, {203, 203, "Austria"}, {204, 204, "Portugal (Azores)"}, {205, 205, "Belgium"}, {206, 206, "Belarus"},
    {207, 207, "Bulgaria"}, {208, 208, "Vatican"}, {209, 210, "Cyprus"}, {211, 211, "Germany"}, {212, 212, "Cyprus"}, {213, 213, "Georgia"},
    {214, 214, "Moldova"}, {215, 215, "Malta"}, {216, 216, "Armenia"}, {218, 218, "Germany"}, {219, 220, "Denmark"}, {224, 225, "Spain"},
    {226, 228, "France"}, {229, 229, "Malta"}, {230, 230, "Finland"}, {231, 231, "Faroe Islands"}, {232, 235, "United Kingdom"}, {236, 236, "Gibraltar"},
    {237, 237, "Greece"}, {238, 238, "Croatia"}, {239, 241, "Greece"}, {242, 242, "Morocco"}, {243, 243, "Hungary"}, {244, 246, "Netherlands"},
    {247, 247, "Italy"}, {248, 249, "Malta"}, {250, 250, "Ireland"}, {251, 251, "Iceland"}, {252, 252, "Liechtenstein"}, {253, 253, "Luxembourg"},
    {254, 254, "Monaco"}, {255, 255, "Portugal (Madeira)"}, {256, 256, "Malta"}, {257, 259, "Norway"}, {261, 261, "Poland"}, {262, 262, "Montenegro"},
    {263, 263, "Portugal"}, {264, 264, "Romania"}, {265, 266, "Sweden"}, {267, 267, "Slovakia"}, {268, 268, "San Marino"}, {269, 269, "Switzerland"},
    {270, 270, "Czechia"}, {271, 271, "Turkey"}, {272, 272, "Ukraine"}, {273, 273, "Russia"}, {274, 274, "North Macedonia"}, {275, 275, "Latvia"},
    {276, 276, "Estonia"}, {277, 277, "Lithuania"}, {278, 278, "Slovenia"}, {279, 279, "Serbia"},
    {301, 301, "Anguilla"}, {303, 303, "USA (Alaska)"}, {304, 305, "Antigua and Barbuda"}, {306, 306, "Netherlands Antilles"}, {307, 307, "Aruba"},
    {308, 309, "Bahamas"}, {310, 310, "Bermuda"}, {311, 311, "Bahamas"}, {312, 312, "Belize"}, {314, 314, "Barbados"}, {316, 316, "Canada"}, {319, 319, "Cayman Islands"},
    {321, 321, "Costa Rica"}, {323, 323, "Cuba"}, {325, 325, "Dominica"}, {327, 327, "Dominican Republic"}, {329, 329, "Guadeloupe"}, {330, 330, "Grenada"},
    {331, 331, "Greenland"}, {332, 332, "Guatemala"}, {334, 334, "Honduras"}, {336, 336, "Haiti"}, {338, 338, "USA"}, {339, 339, "Jamaica"},
    {341, 341, "Saint Kitts and Nevis"}, {343, 343, "Saint Lucia"}, {345, 345, "Mexico"}, {347, 347, "Martinique"}, {348, 348, "Montserrat"},
    {350, 350, "Nicaragua"}, {351, 354, "Panama"}, {358, 358, "Puerto Rico"}, {361, 361, "Saint Pierre and Miquelon"}, {362, 362, "Trinidad and Tobago"},
    {364, 364, "Turks and Caicos"}, {366, 369, "USA"}, {370, 373, "Panama"}, {375, 377, "Saint Vincent"}, {378, 378, "British Virgin Islands"},
    {379, 379, "US Virgin Islands"},
    {401, 401, "Afghanistan"}, {403, 403, "Saudi Arabia"}, {405, 405, "Bangladesh"}, {408, 408, "Bahrain"}, {410, 410, "Bhutan"}, {412, 414, "China"},
    {416, 416, "Taiwan"}, {417, 417, "Sri Lanka"}, {419, 419, "India"}, {422, 422, "Iran"}, {423, 423, "Azerbaijan"}, {425, 425, "Iraq"},
    {428, 428, "Israel"}, {431, 432, "Japan"}, {434, 434, "Turkmenistan"}, {436, 436, "Kazakhstan"}, {437, 437, "Uzbekistan"}, {438, 438, "Jordan"},
    {440, 441, "South Korea"}, {443, 443, "Palestine"}, {445, 445, "North Korea"}, {447, 447, "Kuwait"}, {450, 450, "Lebanon"}, {451, 451, "Kyrgyzstan"},
    {453, 453, "Macao"}, {455, 455, "Maldives"}, {457, 457, "Mongolia"}, {459, 459, "Nepal"}, {461, 461, "Oman"}, {463, 463, "Pakistan"},
    {466, 466, "Qatar"}, {468, 468, "Syria"}, {470, 471, "United Arab Emirates"}, {472, 472, "Tajikistan"}, {473, 475, "Yemen"}, {477, 477, "Hong Kong"},
    {478, 478, "Bosnia and Herzegovina"},
    {503, 503, "Australia"}, {506, 506, "Myanmar"}, {508, 508, "Brunei"}, {510, 510, "Micronesia"}, {511, 511, "Palau"}, {512, 512, "New Zealand"},
    {514, 515, "Cambodia"}, {516, 516, "Christmas Island"}, {518, 518, "Cook Islands"}, {520, 520, "Fiji"}, {523, 523, "Cocos Islands"},
    {525, 525, "Indonesia"}, {529, 529, "Kiribati"}, {531, 531, "Laos"}, {533, 533, "Malaysia"}, {536, 536, "Northern Mariana Islands"},
    {538, 538, "Marshall Islands"}, {540, 540, "New Caledonia"}, {542, 542, "Niue"}, {544, 544, "Nauru"}, {546, 546, "French Polynesia"},
    {548, 548, "Philippines"}, {553, 553, "Papua New Guinea"}, {555, 555, "Pitcairn"}, {557, 557, "Solomon Islands"}, {559, 559, "American Samoa"},
    {561, 561, "Samoa"}, {563, 566, "Singapore"}, {567, 567, "Thailand"}, {570, 570, "Tonga"}, {572, 572, "Tuvalu"}, {574, 574, "Vietnam"},
    {576, 577, "Vanuatu"}, {578, 578, "Wallis and Futuna"},
    {601, 601, "South Africa"}, {603, 603, "Angola"}, {605, 605, "Algeria"}, {609, 609, "Burundi"}, {610, 610, "Benin"}, {611, 611, "Botswana"},
    {612, 612, "Central African Republic"}, {613, 613, "Cameroon"}, {615, 615, "Congo"}, {616, 616, "Comoros"}, {617, 617, "Cabo Verde"},
    {619, 619, "Ivory Coast"}, {620, 620, "Comoros"}, {621, 621, "Djibouti"}, {622, 622, "Egypt"}, {624, 624, "Ethiopia"}, {625, 625, "Eritrea"},
    {626, 626, "Gabon"}, {627, 627, "Ghana"}, {629, 629, "Gambia"}, {630, 630, "Guinea-Bissau"}, {631, 631, "Equatorial Guinea"}, {632, 632, "Guinea"},
    {633, 633, "Burkina Faso"}, {634, 634, "Kenya"}, {636, 637, "Liberia"}, {638, 638, "South Sudan"}, {642, 642, "Libya"}, {644, 644, "Lesotho"},
    {645, 645, "Mauritius"}, {647, 647, "Madagascar"}, {649, 649, "Mali"}, {650, 650, "Mozambique"}, {654, 654, "Mauritania"}, {655, 655, "Malawi"},
    {656, 656, "Niger"}, {657, 657, "Nigeria"}, {659, 659, "Namibia"}, {660, 660, "Reunion"}, {661, 661, "Rwanda"}, {662, 662, "Sudan"},
    {663, 663, "Senegal"}, {664, 664, "Seychelles"}, {665, 665, "Saint Helena"}, {666, 666, "Somalia"}, {667, 667, "Sierra Leone"},
    {668, 668, "Sao Tome and Principe"}, {669, 669, "Eswatini"}, {670, 670, "Chad"}, {671, 671, "Togo"}, {672, 672, "Tunisia"}, {674, 675, "Tanzania"},
    {676, 676, "DR Congo"}, {677, 677, "Tanzania"}, {678, 678, "Zambia"}, {679, 679, "Zimbabwe"},
    {701, 701, "Argentina"}, {710, 710, "Brazil"}, {720, 720, "Bolivia"}, {725, 725, "Chile"}, {730, 730, "Colombia"}, {735, 735, "Ecuador"},
    {740, 740, "Falkland Islands"}, {745, 745, "French Guiana"}, {750, 750, "Guyana"}, {755, 755, "Paraguay"}, {760, 760, "Peru"}, {765, 765, "Suriname"},
    {770, 770, "Uruguay"}, {775, 775, "Venezuela"}};

struct BaudotEnt { char c; uint8_t code; };
const BaudotEnt kBaudot[] = {
    {'A', 0x38}, {'B', 0x33}, {'C', 0x2E}, {'D', 0x32}, {'E', 0x30}, {'F', 0x36}, {'G', 0x2B}, {'H', 0x25}, {'I', 0x2C}, {'J', 0x3A},
    {'K', 0x3E}, {'L', 0x29}, {'M', 0x27}, {'N', 0x26}, {'O', 0x23}, {'P', 0x2D}, {'Q', 0x3D}, {'R', 0x2A}, {'S', 0x34}, {'T', 0x21},
    {'U', 0x3C}, {'V', 0x2F}, {'W', 0x39}, {'X', 0x37}, {'Y', 0x35}, {'Z', 0x31}, {'0', 0x0D}, {'1', 0x1D}, {'2', 0x19}, {'3', 0x10},
    {'4', 0x0A}, {'5', 0x01}, {'6', 0x15}, {'7', 0x1C}, {'8', 0x0C}, {'9', 0x03}, {' ', 0x24}, {'-', 0x18}, {'/', 0x17}};

bool standardLocation(int code) { return code == 2 || code == 3 || code == 4 || code == 5 || code == 6 || code == 7 || code == 12 || code == 14 || code == 9; }   // 9: ELT-DT, whose position field is laid out the same way (#407)

}  // namespace

uint32_t bchRemainder(const std::vector<uint8_t>& bits, int first, int last, uint32_t gen, int degree) {
    uint32_t rem = 0;
    for (int i = first; i <= last; i++) {
        rem = (rem << 1) | (i - 1 < (int)bits.size() ? (bits[(size_t)i - 1] & 1u) : 0u);
        if (rem >> degree & 1u) rem ^= gen;
    }
    return rem & ((1u << degree) - 1u);
}

std::vector<uint8_t> syncBits(bool selfTest) {
    std::vector<uint8_t> b(kSyncBits, 1);
    put(b, 16, 24, selfTest ? kFrameSyncTest : kFrameSyncNormal);
    return b;
}

std::string countryOfMid(int mid) {
    for (const MidRange& r : kMid) if (mid >= r.a && mid <= r.b) return r.name;
    return "";
}

std::string protocolName(bool user, int code, std::string* type) {
    std::string t = "?", n;
    if (user) {
        switch (code) {
        case 0: n = "Orbitography"; t = "Test"; break;
        case 1: n = "Aviation user"; t = "ELT"; break;
        case 2: n = "Maritime user"; t = "EPIRB"; break;
        case 3: n = "Serial user"; t = "?"; break;
        case 4: n = "National user"; break;
        case 6: n = "Radio call sign user"; t = "EPIRB"; break;
        case 7: n = "Test user"; t = "Test"; break;
        default: n = "User protocol " + std::to_string(code); break;
        }
    } else {
        switch (code) {
        case 2: n = "EPIRB, MMSI and location"; t = "EPIRB"; break;
        case 3: n = "ELT, aircraft address and location"; t = "ELT"; break;
        case 4: n = "ELT, serial and location"; t = "ELT"; break;
        case 5: n = "ELT, aircraft operator and location"; t = "ELT"; break;
        case 6: n = "EPIRB, serial and location"; t = "EPIRB"; break;
        case 7: n = "PLB, serial and location"; t = "PLB"; break;
        case 8: n = "ELT, national location"; t = "ELT"; break;
        case 9: n = "ELT-DT, distress tracking location"; t = "ELT-DT"; break;
        case 10: n = "EPIRB, national location"; t = "EPIRB"; break;
        case 11: n = "PLB, national location"; t = "PLB"; break;
        case 12: n = "Ship security (SSAS)"; t = "Ship security"; break;
        case 14: n = "Location test"; t = "Test"; break;
        case 15: n = "National location test"; t = "Test"; break;
        default: n = "Location protocol " + std::to_string(code); break;
        }
    }
    if (type) *type = t;
    return n;
}

std::string bitsToHex(const std::vector<uint8_t>& bits, int first, int last) {
    std::string s;
    static const char* hx = "0123456789ABCDEF";
    for (int i = first; i <= last; i += 4) {
        unsigned v = 0;
        for (int k = 0; k < 4; k++) v = (v << 1) | (i + k <= last && i + k - 1 < (int)bits.size() ? (bits[(size_t)(i + k) - 1] & 1u) : 0u);
        s.push_back(hx[v]);
    }
    return s;
}

uint64_t baudotPack(const std::string& s, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        const char c = i < (int)s.size() ? (char)std::toupper((unsigned char)s[(size_t)i]) : ' ';
        uint8_t code = 0x24;
        for (const BaudotEnt& e : kBaudot) if (e.c == c) code = e.code;
        v = (v << 6) | code;
    }
    return v;
}
std::string baudotUnpack(uint64_t v, int n) {
    std::string s;
    for (int i = n - 1; i >= 0; i--) {
        const uint8_t code = (uint8_t)((v >> (6 * i)) & 0x3F);
        char c = '?';
        for (const BaudotEnt& e : kBaudot) if (e.code == code) c = e.c;
        s.push_back(c);
    }
    return s;
}

namespace {
bool decodeRaw(std::vector<uint8_t>& bits, Beacon& o);
}

// #404: BCH(82,61) repairs 3 bits, so about 4 in 100 random 112 bit blocks pass it. A message that needed 2 or 3 repairs must therefore also make sense
// (a country that exists, a protocol that has a name); a real beacon with that much damage still does, noise rarely.
bool decode(std::vector<uint8_t>& bits, Beacon& o) {
    const std::vector<uint8_t> keep = bits;
    if (!decodeRaw(bits, o)) return false;
    if (o.corrected >= 2 && o.valid) {
        const bool named = o.protoName.rfind("Location protocol ", 0) != 0 && o.protoName.rfind("User protocol ", 0) != 0;
        bool odd = false;
        if (o.userProto) odd = o.idText.find('?') != std::string::npos;   // Baudot text with a code that is no character
        else if (standardLocation(o.protoCode) && !o.hasPos) odd = !(get(bits, 66, 74) == 511 && get(bits, 76, 85) == 1023);   // neither a position nor "no position"
        if (o.country.empty() || !named || odd) { bits = keep; o = Beacon(); return false; }
    }
    return true;
}

namespace {
bool decodeRaw(std::vector<uint8_t>& bits, Beacon& o) {
    o = Beacon();
    if (bits.size() < (size_t)kShortBits) return false;
    const int c1 = bch1().correct(bits, 25);
    if (c1 < 0) return false;
    o.corrected = c1;
    o.valid = true;
    o.longMsg = bits[24] != 0;
    if (o.longMsg && bits.size() < (size_t)kLongBits) { o.valid = false; return false; }
    o.selfTest = get(bits, 16, 24) == kFrameSyncTest;
    o.userProto = bits[25] != 0;
    o.mid = (int)get(bits, 27, 36);
    o.country = countryOfMid(o.mid);
    {   // the 15 hex ID: bits 26-85, with the position of a standard location protocol at its default value (C/S T.001 3.3)
        std::vector<uint8_t> id(bits.begin(), bits.begin() + 85);
        if (!o.userProto && standardLocation((int)get(bits, 37, 40))) { put(id, 65, 85, 0); put(id, 66, 74, 511); put(id, 76, 85, 1023); }
        // ELT-DT (location protocol 9) has no fixed beacon identity: bits 41-64 change from burst to burst (activation, altitude, age of the fix),
        // so they stay out of the 15 hex ID or one transmitter shows up as a new beacon every burst (#407)
        if (!o.userProto && get(bits, 37, 40) == 9) put(id, 41, 64, 0);
        o.hexId = bitsToHex(id, 26, 85);
    }
    o.homing = bits[110] != 0;
    if (o.longMsg) {
        o.bch2Present = true;
        const int c2 = bch2().correct(bits, 107);
        o.bch2Ok = c2 >= 0;
        if (c2 > 0) o.corrected += c2;
    }
    char b[160];
    if (o.userProto) {
        o.protoCode = (int)get(bits, 37, 39);
        o.protoName = protocolName(true, o.protoCode, &o.beaconType);
        switch (o.protoCode) {
        case 1: o.idText = "aircraft marking " + baudotUnpack(get(bits, 40, 81), 7); break;
        case 2: o.idText = "call sign / MMSI digits " + baudotUnpack(get(bits, 40, 75), 6) + ", beacon " + baudotUnpack(get(bits, 76, 81), 1); break;
        case 3: {
            const int ty = (int)get(bits, 40, 42);
            // bits 40-42: 000 ELT, 001 ELT with operator designator, 011 ELT with 24-bit address, 010 float-free EPIRB, 100 EPIRB not float-free, 110 PLB
            o.beaconType = (ty == 0 || ty == 1 || ty == 3) ? "ELT" : (ty == 2 || ty == 4) ? "EPIRB" : ty == 6 ? "PLB" : "?";
            snprintf(b, sizeof b, "%s, serial field %s", ty == 0 ? "ELT" : ty == 1 ? "ELT (aircraft operator)" : ty == 3 ? "ELT (24-bit address)" : ty == 2 ? "float-free EPIRB" : ty == 4 ? "EPIRB (not float-free)" : ty == 6 ? "PLB" : "type unknown", bitsToHex(bits, 44, 73).c_str());
            o.idText = b;
            break;
        }
        default: o.idText = "identification field " + bitsToHex(bits, 40, 85); break;
        }
        return true;
    }
    o.protoCode = (int)get(bits, 37, 40);
    o.protoName = protocolName(false, o.protoCode, &o.beaconType);
    switch (o.protoCode) {
    case 2: {
        const unsigned long m = (unsigned long)get(bits, 41, 60);
        snprintf(b, sizeof b, "MMSI %03d%06lu, beacon %d", o.mid, m, (int)get(bits, 61, 64));
        o.idText = b;
        break;
    }
    case 4: case 6: case 7:
        snprintf(b, sizeof b, "certificate %d, serial %d", (int)get(bits, 41, 50), (int)get(bits, 51, 64));
        o.idText = b;
        break;
    case 5: {
        const uint64_t v = get(bits, 41, 55);
        std::string op;
        for (int k = 2; k >= 0; k--) { const int ch = (int)((v >> (5 * k)) & 31); op.push_back(ch >= 1 && ch <= 26 ? (char)('A' + ch - 1) : '?'); }
        snprintf(b, sizeof b, "operator %s, serial %d", op.c_str(), (int)get(bits, 56, 64));
        o.idText = b;
        break;
    }
    case 3:
        snprintf(b, sizeof b, "aircraft address %06X", (unsigned)get(bits, 41, 64));
        o.idText = b;
        break;
    case 9:
        o.idText = "no fixed identity, status field " + bitsToHex(bits, 41, 64);
        break;
    default: o.idText = "identification field " + bitsToHex(bits, 41, 64); break;
    }
    if (standardLocation(o.protoCode)) {
        const int latU = (int)get(bits, 66, 74), lonU = (int)get(bits, 76, 85);
        if (latU <= 360 && lonU <= 720) {
            o.hasPos = true;
            o.lat = (bits[64] ? -1.0 : 1.0) * latU * 0.25;
            o.lon = (bits[74] ? -1.0 : 1.0) * lonU * 0.25;
            o.posPrecisionDeg = 0.125;
            if (o.longMsg && o.bch2Ok) {
                const int lm = (int)get(bits, 114, 117), ls = (int)get(bits, 118, 122), om = (int)get(bits, 124, 127), os = (int)get(bits, 128, 132);
                if (lm <= 14 && ls <= 14 && om <= 14 && os <= 14) {
                    const double dl = lm / 60.0 + ls * 4.0 / 3600.0, dn = om / 60.0 + os * 4.0 / 3600.0;
                    o.lat += bits[112] ? dl : -dl;
                    o.lon += bits[122] ? dn : -dn;
                    o.fine = true;
                    o.posPrecisionDeg = 2.0 / 3600.0;
                }
            }
        }
    }
    return true;
}

}  // namespace

std::vector<uint8_t> encode(const Spec& s) {
    std::vector<uint8_t> b((size_t)(s.longMsg ? kLongBits : kShortBits), 0);
    const std::vector<uint8_t> sy = syncBits(s.selfTest);
    std::copy(sy.begin(), sy.end(), b.begin());
    b[24] = s.longMsg ? 1 : 0;
    b[25] = s.userProto ? 1 : 0;
    put(b, 27, 36, (uint64_t)s.mid);
    if (s.userProto) {
        put(b, 37, 39, (uint64_t)s.protoCode);
        put(b, 40, 85, s.userBits);
    } else {
        put(b, 37, 40, (uint64_t)s.protoCode);
        put(b, 41, 64, s.ident);
        put(b, 65, 85, 0);
        put(b, 66, 74, 511); put(b, 76, 85, 1023);   // no position
        if (s.hasPos && standardLocation(s.protoCode)) {
            const double la = std::max(-90.0, std::min(90.0, s.lat)), lo = std::max(-180.0, std::min(180.0, s.lon));
            const int cu = (int)std::lround(std::fabs(la) / 0.25), ou = (int)std::lround(std::fabs(lo) / 0.25);
            const double cl = (la < 0 ? -1.0 : 1.0) * cu * 0.25, co = (lo < 0 ? -1.0 : 1.0) * ou * 0.25;
            b[64] = la < 0 ? 1 : 0; put(b, 66, 74, (uint64_t)cu);
            b[74] = lo < 0 ? 1 : 0; put(b, 76, 85, (uint64_t)ou);
            if (s.longMsg && s.fine) {
                const double dl = la - cl, dn = lo - co;
                const int tl = (int)std::lround(std::fabs(dl) * 3600.0 / 4.0), tn = (int)std::lround(std::fabs(dn) * 3600.0 / 4.0);
                b[112] = dl >= 0 ? 1 : 0; put(b, 114, 117, (uint64_t)(tl / 15)); put(b, 118, 122, (uint64_t)(tl % 15));
                b[122] = dn >= 0 ? 1 : 0; put(b, 124, 127, (uint64_t)(tn / 15)); put(b, 128, 132, (uint64_t)(tn % 15));
            } else if (s.longMsg) {
                put(b, 113, 132, 0xFFFFF);
            }
        } else if (s.longMsg) {
            put(b, 113, 132, 0xFFFFF);
        }
    }
    b[110] = s.homing ? 1 : 0;
    {   // parity = remainder of the data followed by 21 zeros
        std::vector<uint8_t> w(b.begin(), b.begin() + 85);
        w.resize(106, 0);
        put(b, 86, 106, bchRemainder(w, 25, 106, kGen1, 21));
    }
    if (s.longMsg) {
        if (s.userProto) put(b, 113, 132, 0);
        std::vector<uint8_t> w(b.begin(), b.begin() + 132);
        w.resize(144, 0);
        put(b, 133, 144, bchRemainder(w, 107, 144, kGen2, 12));
    }
    return b;
}

} // namespace dect2::sarsat
