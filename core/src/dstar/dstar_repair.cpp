#include "dect2/dstar/dstar_repair.h"
#include <cstring>

namespace dect2::dstar {

namespace {
struct Field { int pos, len; };
constexpr Field kFields[5] = {{3, 8}, {11, 8}, {19, 8}, {27, 8}, {35, 4}};   // RPT2, RPT1, UR, MY, MY suffix

std::string rawField(const uint8_t* p, const Field& f) { return std::string((const char*)p + f.pos, (size_t)f.len); }
std::string padded(const std::string& s, size_t n) { std::string r = s; r.resize(n, ' '); return r; }
}

void headerNoteHeard(const Header& h, voice::HeardIds& heard) {
    const std::string* f[5] = {&h.rpt2, &h.rpt1, &h.ur, &h.my, &h.sfx};
    for (int i = 0; i < 5; i++) {
        const std::string p = padded(*f[i], (size_t)kFields[i].len);
        if (i == 4 && p == "    ") continue;
        heard.noteText(p);
    }
}

bool headerRepair(const uint8_t raw[kHeaderBytes], const voice::HeardIds& heard, Header& out, int* subs) {
    if (heard.textSize() == 0) return false;
    // the options per field: as received, then the heard callsigns within two characters
    std::vector<voice::HeardIds::TextCand> opt[5];
    for (int i = 0; i < 5; i++) {
        opt[i].push_back({rawField(raw, kFields[i]), 0, 0});
        for (auto& c : heard.nearText(rawField(raw, kFields[i]), 2, 3)) opt[i].push_back(c);
    }
    uint8_t buf[kHeaderBytes];
    int bestSubs = 99, found = 0;
    Header best;
    size_t idx[5] = {0, 0, 0, 0, 0};
    for (;;) {
        int total = 0;
        memcpy(buf, raw, kHeaderBytes);
        for (int i = 0; i < 5; i++) { memcpy(buf + kFields[i].pos, opt[i][idx[i]].text.data(), (size_t)kFields[i].len); total += opt[i][idx[i]].subs; }
        if (total > 0 && total <= 6) {
            Header h;
            if (headerUnpack(buf, h)) {
                if (total < bestSubs) { bestSubs = total; best = h; found = 1; }
                else if (total == bestSubs) found++;
            }
        }
        int k = 0;
        while (k < 5 && ++idx[k] >= opt[k].size()) idx[k++] = 0;
        if (k == 5) break;
    }
    if (found != 1) return false;   // none, or two equally good ones: do not guess
    out = best;
    if (subs) *subs = bestSubs;
    return true;
}

} // namespace dect2::dstar
