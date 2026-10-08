#include "dect2/marine_navtex.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace dect2 {
namespace marine {

namespace {

constexpr uint64_t kHold = 16;     // places a character waits after its retransmission has arrived

// Code words, letter set and figure set. The 35 words with four marks: 32 characters and shifts, plus alpha, beta and the repeat signal.
// Source: ITU-R M.476 / M.625 code table as published by fldigi (src/navtex/navtex.cxx, code_to_ltrs / code_to_figs); where the
// national variants differ (J, S, V, Z in the figure set) the international alphabet no. 2 is used. 0 in the figure column = bell.
struct CodeRow { uint8_t code; char ltr; char fig; };
const CodeRow kRows[] = {
    {0x17, 'J', 0},   {0x1B, 'F', '!'}, {0x1D, 'C', ':'}, {0x1E, 'K', '('},
    {0x27, 'W', '2'}, {0x2B, 'Y', '6'}, {0x2D, 'P', '0'}, {0x2E, 'Q', '1'},
    {0x35, 'G', '&'}, {0x39, 'M', '.'}, {0x3A, 'X', '/'}, {0x3C, 'V', '='},
    {0x47, 'A', '-'}, {0x4B, 'S', '\''}, {0x4D, 'I', '8'}, {0x4E, 'U', '7'},
    {0x53, 'D', '$'}, {0x55, 'R', '4'}, {0x56, 'E', '3'}, {0x59, 'N', ','}, {0x5C, ' ', ' '},
    {0x63, 'Z', '+'}, {0x65, 'L', ')'}, {0x69, 'H', '#'}, {0x6C, '\n', '\n'},
    {0x71, 'O', '9'}, {0x72, 'B', '?'}, {0x74, 'T', '5'}, {0x78, '\r', '\r'},
};

struct Tables {
    char ltr[128] = {}, fig[128] = {};
    Tables() { for (const auto& r : kRows) { ltr[r.code] = r.ltr; fig[r.code] = r.fig; } }
};
const Tables& tables() { static const Tables t; return t; }

int popcount7(unsigned v) { int n = 0; for (int i = 0; i < 7; i++) n += (v >> i) & 1; return n; }
bool isControl(uint8_t c) { return c == kAlpha || c == kBeta || c == kRep || c == kLtrs || c == kFigs || c == kSpare; }

// ZCZC / NNNN with up to one unreadable character
bool matchWord(const std::string& tail, const char* w) {
    if (tail.size() < 4) return false;
    int ok = 0, star = 0;
    for (int i = 0; i < 4; i++) { if (tail[i] == w[i]) ok++; else if (tail[i] == '*') star++; }
    return ok >= 3 && ok + star == 4;
}

} // namespace

bool ccir476Valid(unsigned code) { return code < 128 && popcount7(code) == 4; }

int ccir476Char(uint8_t code, bool figs) {
    if (code >= 128) return 0;
    return figs ? tables().fig[code] : tables().ltr[code];
}

bool ccir476Code(char c, bool figs, uint8_t& code) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (const auto& r : kRows) {
        if (!figs && r.ltr == c && c != 0) { code = r.code; return true; }
        if (figs && r.fig == c && c != 0) { code = r.code; return true; }
    }
    return false;
}

const char* navtexSubjectName(char b2) {
    // ITU-R M.540 / IMO NAVTEX Manual, subject indicator characters
    switch (b2) {
    case 'A': return "Navigational warning";
    case 'B': return "Meteorological warning";
    case 'C': return "Ice report";
    case 'D': return "Search and rescue information, piracy warning";
    case 'E': return "Meteorological forecast";
    case 'F': return "Pilot service message";
    case 'G': return "AIS message";
    case 'H': return "LORAN message";
    case 'I': return "Spare";
    case 'J': return "Satellite navigation system message";
    case 'K': return "Other electronic navaid message";
    case 'L': return "Navigational warning (additional to A)";
    case 'V': case 'W': case 'X': case 'Y': return "Special service";
    case 'Z': return "No messages on hand";
    default: return "Unknown";
    }
}

std::vector<uint8_t> navtexEncode(const std::string& text) {
    std::vector<uint8_t> out;
    bool figs = false;
    out.push_back(kLtrs);
    auto restate = [&] { out.push_back(figs ? kFigs : kLtrs); };
    for (char c0 : text) {
        char c = c0;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        uint8_t code;
        if (c == '\n') { out.push_back(kCr); out.push_back(kLf); restate(); continue; }
        if (c == '\r') continue;
        if (c == ' ' || c == '\t') { out.push_back(kSpace); continue; }
        if (c >= 'A' && c <= 'Z') {
            if (figs) { out.push_back(kLtrs); figs = false; }
            if (ccir476Code(c, false, code)) out.push_back(code);
            continue;
        }
        if (ccir476Code(c, true, code)) {
            if (!figs) { out.push_back(kFigs); figs = true; }
            out.push_back(code);
        }
    }
    return out;
}

std::vector<uint8_t> sitorBSlots(const std::vector<int>& tok) {
    const size_t n = tok.size() + 2;                     // the last retransmissions come two DX places after the last DX
    std::vector<uint8_t> out(2 * n);
    for (size_t j = 0; j < n; j++) {
        out[2 * j] = j < tok.size() && tok[j] >= 0 ? (uint8_t)tok[j] : kBeta;
        out[2 * j + 1] = (j >= 2 && tok[j - 2] >= 0) ? (uint8_t)tok[j - 2] : kAlpha;
    }
    return out;
}

std::vector<uint8_t> codesToBits(const std::vector<uint8_t>& codes) {
    std::vector<uint8_t> b;
    b.reserve(codes.size() * 7);
    for (uint8_t c : codes) for (int i = 0; i < 7; i++) b.push_back((c >> i) & 1);     // least significant bit first (fldigi navtex.cxx bytes_to_code)
    return b;
}

// ---------------------------------------------------------------------------------------------------------------------------------

NavtexDecoder::NavtexDecoder(bool invert) : invert_(invert) {}

void NavtexDecoder::reset() {
    flush();
    for (int i = 0; i < 7; i++) { validRun_[i] = 0; reg_[i] = 0; regConf_[i] = 0; hpos_[i] = 0; for (int k = 0; k < 32; k++) hvalid_[i][k] = false; }
    bitCount_ = 0; locked_ = false; slots_.clear(); slotBase_ = slotNext_ = 0; recent_ = 0; recentMask_ = 0;
    evid_[0] = evid_[1] = 0; parity_ = -1; nextDx_ = 0; figs_ = false; sinceChar_ = 0;
    tail_.clear();
}

void NavtexDecoder::pushBit(float soft) {
    int bit = soft > 0 ? 1 : 0;
    if (invert_) bit ^= 1;
    float conf = std::fabs(soft);
    if (conf > 1.f) conf = 1.f;
    for (int i = 0; i < 6; i++) { reg_[i] = reg_[i + 1]; regConf_[i] = regConf_[i + 1]; }
    reg_[6] = (uint8_t)bit; regConf_[6] = conf;
    bitCount_++;
    const int ph = (int)(bitCount_ % 7);
    unsigned code = 0;
    for (int i = 0; i < 7; i++) code |= (unsigned)reg_[i] << i;      // the oldest bit is the least significant
    const bool valid = bitCount_ >= 7 && popcount7(code) == 4;
    validRun_[ph] = valid ? validRun_[ph] + 1 : 0;
    {
        float cm = 0; for (int i = 0; i < 7; i++) cm += regConf_[i];
        hconf_[ph][hpos_[ph] & 31] = cm / 7.f;
    }
    hcode_[ph][hpos_[ph] & 31] = (uint8_t)code; hvalid_[ph][hpos_[ph] & 31] = valid; hpos_[ph]++;
    if (!locked_) {
        if (++sinceChar_ > 1500) { sinceChar_ = 0; flush(); }      // 15 s without a lock ends a message
        if (phaseGood(ph)) {
            locked_ = true; lockPhase_ = ph; slots_.clear(); slotBase_ = slotNext_ = 0;
            recentMask_ = 0; recent_ = 0; evid_[0] = evid_[1] = 0; parity_ = -1; nextDx_ = 0; sinceChar_ = 0;
        }
        return;
    }
    if (ph != lockPhase_) {
        // a bit slip: another place in the 7-bit cycle suddenly gives a long run of good words while ours does not
        if (recent_ >= 6 && validRun_[ph] >= 9 && phaseScore(ph) >= phaseScore(lockPhase_) + 12) {
            // keep the numbering of the places (the damaged ones stay in it as unreadable): the retransmissions after the slip then
            // repair the characters lost in it. The parity is kept; the new evidence decides if it was lost with a place.
            // The new phase's words since the slip replace the places they belong to: the first old place that ends at or after the end of a new word
            // (a loss of s bits moves the boundary s bits earlier, and the count of places goes on as before).
            const int m = std::min(validRun_[ph], 24);
            for (int a = m - 1; a >= 0; a--) {
                const int hi = (hpos_[ph] - 1 - a) & 31;
                Slot ns;
                ns.code = hcode_[ph][hi]; ns.valid = hvalid_[ph][hi]; ns.endBit = bitCount_ - 7 * (uint64_t)a;
                for (int i = 0; i < 7; i++) { ns.bit[i] = (uint8_t)((ns.code >> i) & 1); ns.conf[i] = hconf_[ph][hi]; }
                long j = -1;
                for (size_t i = slots_.size(); i-- > 0;) { if (slots_[i].endBit >= ns.endBit) j = (long)i; else break; }
                if (j >= 0) { if (ns.valid) slots_[(size_t)j] = ns; }
                else { slots_.push_back(ns); slotNext_++; if (slots_.size() > 256) { slots_.pop_front(); slotBase_++; } }
            }
            lockPhase_ = ph; recentMask_ = 0; recent_ = 0;
            evid_[0] = evid_[1] = 0;
        }
        return;
    }
    Slot s;
    s.code = (uint8_t)code; s.valid = valid; s.endBit = bitCount_;
    for (int i = 0; i < 7; i++) { s.bit[i] = reg_[i]; s.conf[i] = regConf_[i]; }
    onChar(s);
}

// How much a bit phase looks like the character boundary of a transmission: valid words, the idle signals (alpha, beta, repeat) and text
// whose repetition 5 places later is the same word. The idle pattern alone is not enough: shifted by one bit it also forms valid words.
int NavtexDecoder::phaseScore(int ph, int* validRun) const {
    auto at = [&](int back, uint8_t& code, bool& v) { const int i = (hpos_[ph] - 1 - back) & 31; code = hcode_[ph][i]; v = hvalid_[ph][i]; };
    int ctrl = 0, match = 0;
    for (int j = 0; j < 16; j++) { uint8_t c; bool v; at(j, c, v); if (v && (c == kAlpha || c == kBeta || c == kRep)) ctrl++; }
    for (int j = 0; j < 20; j++) {
        uint8_t a, b; bool va, vb; at(j, a, va); at(j + 5, b, vb);
        if (va && vb && a == b && !isControl(a)) match++;
    }
    if (validRun) *validRun = validRun_[ph];
    return validRun_[ph] + 4 * ctrl + 4 * match;
}

bool NavtexDecoder::phaseGood(int ph) const {
    if (validRun_[ph] < 16) return false;
    int ctrl = 0, match = 0;
    auto at = [&](int back, uint8_t& code, bool& v) { const int i = (hpos_[ph] - 1 - back) & 31; code = hcode_[ph][i]; v = hvalid_[ph][i]; };
    for (int j = 0; j < 16; j++) { uint8_t c; bool v; at(j, c, v); if (v && (c == kAlpha || c == kBeta || c == kRep)) ctrl++; }
    if (ctrl >= 12) return true;
    for (int j = 0; j < 20; j++) {
        uint8_t a, b; bool va, vb; at(j, a, va); at(j + 5, b, vb);
        if (va && vb && a == b && !isControl(a)) match++;
    }
    return match >= 6;
}

void NavtexDecoder::onChar(const Slot& s) {
    slots_.push_back(s);
    slotNext_++;
    if (slots_.size() > 256) { slots_.pop_front(); slotBase_++; }
    const uint32_t bad = s.valid ? 0u : 1u;
    recentMask_ = ((recentMask_ << 1) | bad) & 0xFFFFFFu;
    recent_ = __builtin_popcount(recentMask_);
    if (s.valid) validChars_++; else badChars_++;
    // evidence for the DX/RX parity: the retransmission equals the character 5 places earlier
    const uint64_t k = slotNext_ - 1;
    if (k >= slotBase_ + 5 && s.valid && !isControl(s.code)) {
        const Slot& a = slots_[(size_t)(k - 5 - slotBase_)];
        if (a.valid && a.code == s.code) evid_[(k - 5) & 1]++;
    }
    if (parity_ < 0) {
        for (int p = 0; p < 2; p++)
            if (evid_[p] >= 3 && evid_[p] >= 3 * evid_[1 - p] + 1) { parity_ = p; nextDx_ = slotBase_ + ((slotBase_ & 1) == (uint64_t)p ? 0 : 1); }
    } else if (evid_[1 - parity_] >= 4 && evid_[parity_] == 0) {
        parity_ = 1 - parity_;
        nextDx_ = std::max<uint64_t>(nextDx_, slotBase_);
        if ((nextDx_ & 1) != (uint64_t)parity_) nextDx_++;
    }
    if (parity_ >= 0) process(false);
    if (recent_ >= 14) {                     // lost the signal
        process(true);
        locked_ = false; sinceChar_ = 0;
        for (int i = 0; i < 7; i++) { validRun_[i] = 0; hpos_[i] = 0; for (int k = 0; k < 32; k++) hvalid_[i][k] = false; }
    }
}

bool NavtexDecoder::combine(const Slot& a, const Slot& b, uint8_t& code) const {
    if (a.valid && b.valid) {
        if (a.code == b.code) { code = a.code; return true; }
        float ca = 0, cb = 0;
        for (int i = 0; i < 7; i++) { ca += a.conf[i]; cb += b.conf[i]; }
        code = ca >= cb ? a.code : b.code;
        return true;
    }
    if (a.valid) { code = a.code; return true; }
    if (b.valid) { code = b.code; return true; }
    // neither copy has four marks: add the soft values of both and fix the least sure bit when one flip is enough
    float llr[7];
    unsigned c = 0;
    for (int i = 0; i < 7; i++) {
        llr[i] = (a.bit[i] ? 1.f : -1.f) * a.conf[i] + (b.bit[i] ? 1.f : -1.f) * b.conf[i];
        c |= (llr[i] > 0 ? 1u : 0u) << i;
    }
    const int n = popcount7(c);
    if (n != 3 && n != 5) return false;
    int best = -1; float bc = 1e9f;
    for (int i = 0; i < 7; i++) {
        const unsigned bitv = (c >> i) & 1;
        if (n == 3 && bitv == 0 && std::fabs(llr[i]) < bc) { bc = std::fabs(llr[i]); best = i; }
        if (n == 5 && bitv == 1 && std::fabs(llr[i]) < bc) { bc = std::fabs(llr[i]); best = i; }
    }
    if (best < 0) return false;
    c ^= 1u << best;
    code = (uint8_t)c;
    return popcount7(c) == 4;
}

void NavtexDecoder::process(bool force) {
    if (parity_ < 0) return;
    while (nextDx_ < slotNext_) {
        if (!force && nextDx_ + 5 + kHold >= slotNext_) break;       // wait a little more: a bit slip found later repairs the places
        if (nextDx_ < slotBase_) { nextDx_ += 2; continue; }
        const Slot& a = slots_[(size_t)(nextDx_ - slotBase_)];
        Slot none;
        const Slot& b = (nextDx_ + 5 < slotNext_) ? slots_[(size_t)(nextDx_ + 5 - slotBase_)] : none;
        uint8_t code = 0;
        if (combine(a, b, code)) {
            if (code == kLtrs) figs_ = false;
            else if (code == kFigs) figs_ = true;
            else if (!isControl(code)) { const int ch = ccir476Char(code, figs_); if (ch) emitChar(ch, false); }
        } else emitChar('*', true);
        nextDx_ += 2;
    }
}

void NavtexDecoder::emitChar(int ch, bool err) {
    charsOut_++;
    tail_.push_back((char)ch);
    if (tail_.size() > 4) tail_.erase(tail_.begin());
    if (mstate_ != 0 && matchWord(tail_, "ZCZC")) {      // a new message starts before the old one ended
        finish(false);
        mstate_ = 1; hdr_.clear(); body_.clear(); tail_.clear();
        return;
    }
    if (mstate_ == 0) {
        if (matchWord(tail_, "ZCZC")) { mstate_ = 1; hdr_.clear(); body_.clear(); tail_.clear(); }
        return;
    }
    if (mstate_ == 1) {
        if (hdr_.empty() && (ch == ' ' || ch == '\r' || ch == '\n')) return;       // "ZCZC" then a space
        if (ch == '\r' || ch == '\n') { if (hdr_.size() >= 4) mstate_ = 2; return; }
        hdr_.push_back((char)ch);
        if (hdr_.size() == 4) mstate_ = 2;
        return;
    }
    // body
    if (ch == '\r') return;
    body_.push_back((char)ch);
    if (matchWord(tail_, "NNNN")) {
        body_.resize(body_.size() >= 4 ? body_.size() - 4 : 0);
        finish(true);
        return;
    }
    if (body_.size() > 20000) finish(false);
}

void NavtexDecoder::finish(bool complete) {
    if (mstate_ == 0) return;
    const bool had = mstate_ == 2 || !hdr_.empty();
    mstate_ = 0;
    if (!had) { tail_.clear(); return; }
    NavtexMessage m;
    // trim blank lines at both ends
    size_t a = 0, b = body_.size();
    while (a < b && (body_[a] == '\n' || body_[a] == ' ')) a++;
    while (b > a && (body_[b - 1] == '\n' || body_[b - 1] == ' ')) b--;
    m.text = body_.substr(a, b - a);
    m.header = hdr_;
    m.station = hdr_.size() > 0 && hdr_[0] != '*' ? hdr_[0] : '?';
    m.subject = hdr_.size() > 1 && hdr_[1] != '*' ? hdr_[1] : '?';
    if (hdr_.size() >= 4 && isdigit((unsigned char)hdr_[2]) && isdigit((unsigned char)hdr_[3])) m.number = (hdr_[2] - '0') * 10 + (hdr_[3] - '0');
    m.subjectName = navtexSubjectName(m.subject);
    m.chars = (uint32_t)m.text.size();
    for (char c : m.text) if (c == '*') m.errors++;
    for (char c : hdr_) if (c == '*') m.errors++;
    m.cer = m.chars + 4 ? (float)m.errors / (float)(m.chars + 4) : 0.f;
    m.complete = complete;
    body_.clear(); hdr_.clear(); tail_.clear();
    if (cb_) cb_(m);
}

void NavtexDecoder::flush() {
    process(true);
    if (mstate_ != 0) finish(false);
}

} // namespace marine
} // namespace dect2
