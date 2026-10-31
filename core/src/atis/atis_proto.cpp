#include "dect2/atis/atis_proto.h"
#include <cstdio>
#include "dect2/marine/marine_dsc.h"
#include "dect2/marine/marine_mmsi.h"
#include <cctype>

namespace dect2 {
namespace atis {

namespace {
bool allDigits(const std::string& s) { for (char c : s) if (c < '0' || c > '9') return false; return !s.empty(); }
}

std::string callsignFirstLetter(int mid) {
    switch (mid) {
    case 244: case 245: case 246: return "P";   // Netherlands (PA .. PI)
    case 211: case 218: return "D";             // Germany (DA .. DR)
    case 205: return "O";                       // Belgium (ON .. OT)
    case 226: case 227: case 228: return "F";   // France
    case 269: return "H";                       // Switzerland (HB)
    case 253: return "L";                       // Luxembourg (LX)
    case 203: return "O";                       // Austria (OE)
    case 243: return "H";                       // Hungary (HA)
    case 267: return "O";                       // Slovakia (OM)
    case 270: return "O";                       // Czech Republic (OK)
    case 264: return "Y";                       // Romania (YO)
    case 207: return "L";                       // Bulgaria (LZ)
    case 261: return "S";                       // Poland (SP)
    case 219: case 220: return "O";             // Denmark (OZ)
    case 265: case 266: return "S";             // Sweden
    case 257: case 258: case 259: return "L";   // Norway
    case 230: return "O";                       // Finland (OH)
    default: return "";
    }
}

Id idFromDigits(const std::string& ten) {
    Id id;
    if (ten.size() != 10) return id;
    id.digits = ten;
    for (char c : ten) if (c == '~') id.partial = true; else if (c < '0' || c > '9') return id;
    id.valid = true;
    id.standardZ = ten[0] == '9';
    id.mmsi = ten.substr(1);
    const std::string m = ten.substr(1, 3);
    if (allDigits(m)) { id.mid = std::stoi(m); id.country = marine::midCountry(id.mid); }
    // call sign: X1 X2 = the second letter, X3 .. X6 = the number
    const std::string x12 = ten.substr(4, 2), num = ten.substr(6, 4);
    std::string first = callsignFirstLetter(id.mid);
    char second = '?';
    if (allDigits(x12)) { const int v = std::stoi(x12); if (v >= 1 && v <= 26) second = (char)('A' + v - 1); }
    id.callsign = (first.empty() ? std::string("?") : first) + second + num;
    id.firstLetterNote = first.empty() ? "unknown" : "likely";
    return id;
}

Id idFromSymbols(const std::vector<int>& b) {
    Id none;
    if (b.size() < 6 || b[0] != kFormatSpecifier) return none;
    std::string d;
    for (int k = 1; k <= 5; k++) {
        const int s = b[(size_t)k];
        if (s >= 0 && s <= 99) { d += (char)('0' + s / 10); d += (char)('0' + s % 10); }
        else d += "~~";
    }
    Id id = idFromDigits(d);
    return id;
}

std::string idFromCallsign(const std::string& cs, int mid) {
    if (cs.size() != 6 || mid < 100 || mid > 999) return "";
    for (int i = 2; i < 6; i++) if (!isdigit((unsigned char)cs[(size_t)i])) return "";
    const char l = (char)toupper((unsigned char)cs[1]);
    if (l < 'A' || l > 'Z') return "";
    char b[16];
    snprintf(b, sizeof b, "9%03d%02d%s", mid, l - 'A' + 1, cs.substr(2).c_str());
    return b;
}

std::string idFromMmsi(const std::string& m) { return m.size() == 9 && allDigits(m) ? "9" + m : ""; }

std::vector<int> bodyFromDigits(const std::string& ten) {
    if (ten.size() != 10 || !allDigits(ten)) return {};
    std::vector<int> v;
    v.push_back(kFormatSpecifier);
    for (int k = 0; k < 5; k++) v.push_back((ten[(size_t)(2 * k)] - '0') * 10 + (ten[(size_t)(2 * k + 1)] - '0'));
    return v;
}

std::vector<uint8_t> frameBits(const std::string& ten, int dotBits) {
    const auto body = bodyFromDigits(ten);
    if (body.empty()) return {};
    return marine::dscFrameBits(body, kEndOfSequence, dotBits);
}

int checkSymbol(const std::string& ten) {
    const auto body = bodyFromDigits(ten);
    return body.empty() ? -1 : marine::dscEcc(body, kEndOfSequence);
}

} // namespace atis
} // namespace dect2
