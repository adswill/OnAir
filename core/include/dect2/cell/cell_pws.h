// Cell towers: the public warning content of SIB10/11/12. Message identifiers, serial number and CB Data pages follow 3GPP TS 23.041
// sec 9.4.1.2.1, 9.4.1.2.2, 9.3.24 and 9.4.2.2.5; the text coding follows TS 23.038 sec 5 and 6.2.1. These are public broadcasts.
#pragma once
#include "cell_asn.h"
#include <map>
#include <string>
#include <vector>

namespace dect2::cell {

// "ETWS earthquake", "CMAS extreme, immediate, observed / EU-Alert level 2", ... etws: the identifier is in the ETWS range. Empty text: not a warning id.
std::string pwsCategory(uint32_t messageId, bool* etws = nullptr);
const char* etwsTypeName(uint32_t warningType);   // from the 7 high bits of the 16-bit Warning-Type

// GSM 7-bit default alphabet, CBS packing, to UTF-8 (with the 0x1B extension table). Trailing CR padding is removed.
std::string gsm7ToUtf8(const uint8_t* d, size_t n);
// The text of a complete Warning Message Contents: CB Data pages when the size says so, a plain block otherwise. lang: ISO 639 code or name from the DCS, "" unknown
bool pwsDecodeText(const std::vector<uint8_t>& content, int dcs, std::string& text, std::string& lang);

struct PwsMessage {
    uint32_t messageId = 0, serial = 0;
    int sib = 12;
    int geoScope = 0, messageCode = 0, updateNumber = 0;   // of the serial number
    bool etws = false, complete = false;
    uint32_t warningType = 0;
    std::string category, text, lang;
    int segments = 0;
};

// Puts segments of one cell together. add() returns true when msg was (re)built: for a new SIB10, or when a segmented message becomes complete
class PwsAssembler {
public:
    bool add(const LteWarnSeg& s, PwsMessage& msg);
private:
    struct Slot { std::map<int, std::vector<uint8_t>> segs; int dcs = -1, last = -1; bool done = false; };
    std::map<uint64_t, Slot> slots_;
};

} // namespace dect2::cell
