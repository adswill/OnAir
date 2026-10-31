// RDS / RBDS data layer, see fm_rds.h.
#include "dect2/fm/fm_rds.h"
#include "dect2/common/text_charset.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace dect2 {

namespace {

// remainder of v (nbits long, bit i = coefficient of x^i) divided by the RDS generator x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1
uint32_t rdsRem(uint32_t v, int nbits) {
    for (int i = nbits - 1; i >= 10; i--) if ((v >> i) & 1) v ^= 0x5B9u << (i - 10);
    return v;
}
constexpr uint32_t kOffsets[5] = {0x0FC, 0x198, 0x168, 0x350, 0x1B4};   // A, B, C, C', D
constexpr uint32_t offsetOfPos(int pos) { return kOffsets[pos == 3 ? 4 : pos]; }   // block positions 0..3 = A, B, C (or C'), D
int posOf(int offIdx) { return offIdx == 3 ? 2 : offIdx == 4 ? 3 : offIdx; }
int matchOffset(uint32_t syn) { for (int i = 0; i < 5; i++) if (syn == kOffsets[i]) return i; return -1; }

// The burst errors of one and two bits (one wrong symbol before the differential decoding), by syndrome. All of them have different
// syndromes (the code corrects every burst up to 5 bits); the longer bursts are left out, they would turn too many bad blocks into
// wrong ones.
struct BurstTable {
    uint32_t syn[51], pat[51];
    int n = 0;
    BurstTable() {
        for (int i = 0; i < 26; i++) { pat[n] = 1u << i; syn[n] = rdsRem(pat[n], 26); n++; }
        for (int i = 0; i < 25; i++) { pat[n] = 3u << i; syn[n] = rdsRem(pat[n], 26); n++; }
    }
    uint32_t find(uint32_t s) const { for (int i = 0; i < n; i++) if (syn[i] == s) return pat[i]; return 0; }
};
const BurstTable& bursts() { static const BurstTable t; return t; }

// The text buffers keep the received bytes; they become UTF-8 only when the text leaves the decoder (rdsText).
char rdsChar(uint8_t c) { return c == 0x0D || (c >= 0x20 && c != 0x7F) ? (char)c : '?'; }

const uint16_t kGb2312[87][94] = {
#include "fm_gb2312.inc"
};

uint32_t gb2312At(uint8_t lead, uint8_t trail) {
    if (lead < 0xA1 || lead > 0xF7 || trail < 0xA1 || trail > 0xFE) return 0;
    return kGb2312[lead - 0xA1][trail - 0xA1];
}

// Bytes 0x20..0x7E as they come (the de-facto ASCII of every station); 0x80..0xFF as the EBU Latin table of IEC 62106 Annex E
// (the same table as DAB charset 0). Many Chinese stations send GB2312 (two bytes a character, both >= 0xA1) instead; 'charset' 1 (auto)
// takes the bytes as GB2312 when every byte >= 0x80 pairs up as a GB2312 character (or most do while a partial text still has gaps), 2 always, 0 never.
std::string rdsText(const char* raw, size_t n, int charset) {
    bool gb = false;
    if (charset != 0) {
        int pairs = 0, stray = 0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t c = (uint8_t)raw[i];
            if (c < 0x80) continue;
            if (i + 1 < n && gb2312At(c, (uint8_t)raw[i + 1])) { pairs++; i++; } else stray++;
        }
        gb = pairs >= 1 && (charset == 2 || stray == 0 || (pairs >= 2 && stray * 2 <= pairs));
    }
    std::string o;
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = (uint8_t)raw[i];
        if (c < 0x80) { o += (char)c; continue; }
        if (gb) {
            const uint32_t cp = i + 1 < n ? gb2312At(c, (uint8_t)raw[i + 1]) : 0;
            if (cp) { text::appendUtf8(o, cp); i++; } else o += '?';
        } else o += text::ebuLatin(&c, 1);
    }
    return o;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return s.substr(i);
}

void putUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
}

double nowSecs() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

} // namespace

const char* rtPlusTypeName(int t) {
    static const char* const n[64] = {
        "Dummy", "Title", "Album", "Track", "Artist", "Composition", "Movement", "Conductor", "Composer", "Band", "Comment", "Genre",
        "News", "Local news", "Stock market", "Sport", "Lottery", "Horoscope", "Daily diversion", "Health", "Event", "Scene", "Cinema",
        "TV", "Date and time", "Weather", "Traffic", "Alarm", "Advertisement", "Website", "Other info",
        "Station (short)", "Station (long)", "Programme now", "Programme next", "Programme part", "Host", "Editorial staff", "Frequency",
        "Programme homepage", "Sub-channel", "Hotline phone", "Studio phone", "Phone", "Studio SMS", "SMS", "Hotline e-mail",
        "Studio e-mail", "E-mail", "MMS", "Chat", "Chat centre", "Vote question", "Vote centre", "Reserved", "Reserved", "Private",
        "Private", "Private", "Place", "Appointment", "Identifier", "Purchase", "Get data"};
    return t >= 0 && t < 64 ? n[t] : "";
}

std::string rdsHexGroup(const uint16_t b[4], const uint8_t err[4]) {
    char s[32];
    std::string out;
    for (int i = 0; i < 4; i++) {
        if (err[i] >= 2) snprintf(s, sizeof s, "----"); else snprintf(s, sizeof s, "%04X", b[i]);
        if (i) out += ' ';
        out += s;
    }
    return out;
}

void RdsDecoder::reset() {
    GroupTap t = std::move(tap_);
    const bool b = ecBurst_, s = ecSoft_;
    *this = RdsDecoder();
    tap_ = std::move(t); ecBurst_ = b; ecSoft_ = s;
}

bool RdsDecoder::correct(uint32_t& reg, uint32_t syn, int pos) const {
    const uint32_t offs[2] = {offsetOfPos(pos), kOffsets[3]};
    const int nOff = pos == 2 ? 2 : 1;
    if (ecBurst_) {
        for (int k = 0; k < nOff; k++) if (const uint32_t e = bursts().find(syn ^ offs[k])) { reg ^= e; return true; }
    }
    if (!ecSoft_) return false;
    // Two wrong symbols: the pairs among the six weakest symbols of the block (27 of them: the one before the first bit flips only the first
    // bit, the last one only the last bit here). A symbol j flips the bits j and j + 1 (bit 0 = the first bit of the block = register bit 25).
    struct Sym { float r; uint32_t mask, syn; };
    Sym s[27];
    for (int j = -1; j < 26; j++) {
        Sym& y = s[j + 1];
        y.r = rel_[(bitNo_ + 6 + (uint64_t)(j + 32)) & 31];   // the bit at bitNo_ - 26 + j carries the reliability of symbol j
        uint32_t m = 0;
        if (j >= 0) m |= 1u << (25 - j);
        if (j + 1 <= 25) m |= 1u << (24 - j);
        y.mask = m; y.syn = rdsRem(m, 26);
    }
    std::partial_sort(s, s + 6, s + 27, [](const Sym& a, const Sym& b) { return a.r < b.r; });
    int best = -1; float bestR = 1e30f; uint32_t bestMask = 0;
    for (int a = 0; a < 6; a++) for (int b = a + 1; b < 6; b++) {
        const uint32_t ps = s[a].syn ^ s[b].syn;
        for (int k = 0; k < nOff; k++) if ((ps ^ syn ^ offs[k]) == 0 && s[a].r + s[b].r < bestR) {
            bestR = s[a].r + s[b].r; best = a; bestMask = s[a].mask ^ s[b].mask;
        }
    }
    if (best < 0) return false;
    reg ^= bestMask;
    return true;
}

void RdsDecoder::pushBit(int bit, float reliability) {
    reg_ = ((reg_ << 1) | (uint32_t)(bit & 1)) & 0x3FFFFFF;
    rel_[bitNo_ & 31] = reliability;
    bitNo_++;
    if (synced) {
        if (++cnt_ < 26) return;
        cnt_ = 0;
        const uint32_t syn = rdsRem(reg_, 26);
        uint32_t w = reg_;
        uint8_t e = 2;
        if (syn == offsetOfPos(expect_) || (expect_ == 2 && syn == kOffsets[3])) e = 0;
        else if (correct(w, syn, expect_)) e = 1;
        else if (ecVote_ && voteBlock(w, expect_)) { e = 1; info.blocksVoted++; }
        if (e == 0) info.blocksGood++; else if (e == 1) info.blocksCorrected++; else info.blocksBad++;
        okPct += ((e < 2 ? 100.f : 0.f) - okPct) * 0.05f;
        badRun_ = e == 0 ? 0 : e == 1 ? badRun_ : badRun_ + 1;
        block(w, e, expect_, reg_);
        expect_ = (expect_ + 1) & 3;
        if (badRun_ >= 24) { synced = false; okPct = 0; nHits_ = 0; }
        return;
    }
    // Acquisition: a checkword that fits an offset word marks a block end; two of them a whole number of blocks apart (up to six) with the
    // offsets in their order (A B C D) fix the block boundaries. One clean block between bad ones is then enough, not two in a row.
    const int m = matchOffset(rdsRem(reg_, 26));
    if (m < 0) return;
    const int p = posOf(m);
    for (int i = 0; i < nHits_; i++) {
        const uint64_t d = bitNo_ - hits_[i].bit;
        if (d == 0 || d % 26 != 0 || d / 26 > 6) continue;
        if (((hits_[i].pos + (int)(d / 26)) & 3) != p) continue;
        synced = true; cnt_ = 0; badRun_ = 0; okPct = 50; nHits_ = 0;
        for (auto& e : err_) e = 2;
        info.blocksGood++;
        block(reg_, 0, p, reg_);
        expect_ = (p + 1) & 3;
        return;
    }
    // keep the hits of the last six blocks
    int k = 0;
    for (int i = 0; i < nHits_; i++) if (bitNo_ - hits_[i].bit <= 6 * 26) hits_[k++] = hits_[i];
    nHits_ = k;
    if (nHits_ == 8) { std::memmove(hits_, hits_ + 1, sizeof(Hit) * 7); nHits_ = 7; }
    hits_[nHits_++] = {bitNo_, p};
}

// Voting over repeated copies of a bad block (see the header). A copy joins when it is within kClose bits of the new word; at least two
// of them are needed, so the result is the vote of three words, which must also give the checkword of the position. Random bits give
// three close words rarely (about 4% a pair) and a right checkword 1 in 1024, so a wrong block is a one in a million event; the copies
// used are dropped, so one set of words never votes twice.
bool RdsDecoder::voteBlock(uint32_t& w, int pos) {
    constexpr int kClose = 8, kFar = 5;
    VoteCopy cur;
    cur.w = w; cur.at = bitNo_;
    for (int i = 0; i < 26; i++) {
        const float r = std::max(0.01f, rel_[(bitNo_ - 26 + (uint64_t)i) & 31]);
        cur.llr[i] = ((w >> (25 - i)) & 1) ? r : -r;
    }
    VoteCopy* st = vote_[pos];
    int nearIdx[kVote], nn = 0;
    for (int k = 0; k < nVote_[pos]; k++) {
        if (bitNo_ - st[k].at > 26ull * 4 * 40) continue;   // older than about 40 groups (3.5 s): another part of the cycle
        if (__builtin_popcount(st[k].w ^ w) <= kClose) nearIdx[nn++] = k;
    }
    if (nn >= 2) {
        float sum[26];
        for (int i = 0; i < 26; i++) sum[i] = cur.llr[i];
        for (int j = 0; j < nn; j++) for (int i = 0; i < 26; i++) sum[i] += st[nearIdx[j]].llr[i];
        uint32_t v = 0;
        for (int i = 0; i < 26; i++) v = (v << 1) | (sum[i] > 0 ? 1u : 0u);
        const uint32_t syn = rdsRem(v, 26);
        // every copy must lie within kFar bits of the result: a result that needs a copy to be very wrong is a chance hit of the checkword
        bool tight = __builtin_popcount(v ^ w) <= kFar;
        for (int j = 0; j < nn && tight; j++) tight = __builtin_popcount(v ^ st[nearIdx[j]].w) <= kFar;
        if (tight && (syn == offsetOfPos(pos) || (pos == 2 && syn == kOffsets[3]))) {
            for (int j = nn - 1; j >= 0; j--) { st[nearIdx[j]] = st[nVote_[pos] - 1]; nVote_[pos]--; }
            w = v;
            return true;
        }
    }
    // not usable (yet): keep this copy, the oldest goes when the store is full
    if (nVote_[pos] < kVote) st[nVote_[pos]++] = cur;
    else { st[votePos_[pos]] = cur; votePos_[pos] = (votePos_[pos] + 1) % kVote; }
    return false;
}

void RdsDecoder::block(uint32_t w, uint8_t err, int pos, uint32_t raw) {
    if (pos == 0) { for (auto& e : err_) e = 2; rawHash_ = 0; }
    blk_[pos] = (uint16_t)(w >> 10);
    err_[pos] = err;
    if (err == 1) rawHash_ = ((rawHash_ * 0x9E3779B1u) ^ (raw + (uint32_t)pos * 0x4000001u)) | 1u;   // what was received, before the correction
    if (pos == 3) decodeGroup(blk_, err_, nowSecs(), rawHash_);
}

bool RdsDecoder::seenBefore(const uint16_t* b, const uint8_t* err, uint32_t raw) {
    const Sig s{b[1], err[2] < 2 ? b[2] : (uint16_t)0, b[3], (uint8_t)(err[2] < 2), raw};
    // the same B and D, and the same C when both have one; two corrected copies confirm each other only when what was received differs
    // (the same damage twice, from interference that repeats, would give the same wrong correction twice)
    for (int i = 0; i < nSig_; i++) {
        Sig& o = sigs_[i];
        if (o.b != s.b || o.d != s.d || (o.cOk && s.cOk && o.c != s.c)) continue;
        if (err[2] == 1 && !o.cOk) continue;   // a corrected C needs a copy that had a C to compare with
        if (o.raw != 0 && o.raw == s.raw) return false;
        if (s.cOk && !o.cOk) o = s;
        if (s.raw == 0) o.raw = 0;
        return true;
    }
    sigs_[sigPos_] = s; sigPos_ = (sigPos_ + 1) % kSigs; nSig_ = std::min(kSigs, nSig_ + 1);
    return false;
}

void RdsDecoder::onGroup(const uint16_t b[4], const uint8_t err[4], double now) { decodeGroup(b, err, now, 0); }

void RdsDecoder::decodeGroup(const uint16_t b[4], const uint8_t err[4], double now, uint32_t raw) {
    if (tap_) tap_(b, err);
    if (err[1] >= 2) return;                       // block B holds the group type: nothing without it
    const int type = b[1] >> 12, verB = (b[1] >> 11) & 1, code = type * 2 + verB;
    bool cOk = err[2] < 2;
    const bool dOk = err[3] < 2;
    const bool corrected = err[1] == 1 || (cOk && err[2] == 1) || (dOk && err[3] == 1);
    // the PI: from block A, or from C' in a version B group; a corrected one only when it came the same before
    if (err[0] == 0) pi = b[0];
    if (verB && err[2] == 0) pi = b[2];
    if (!dOk) return;
    // every group is remembered; one with a corrected block is used only when it came the same before (clean or corrected)
    const bool repeat = seenBefore(b, err, corrected ? (raw ? raw : 1u) : 0u);
    if (corrected && !repeat) {
        if (err[1] == 1 || err[3] == 1) return;
        cOk = false;   // only C corrected: the group is used without it (the name in D of a 0A group does not wait for the AF in C)
    }
    if (err[0] == 1 && repeat) pi = b[0];
    groups++;
    tp = (b[1] >> 10) & 1;
    pty = (b[1] >> 5) & 31;
    const int low = b[1] & 31;

    if (code == rtPlusGroup_ && cOk) { rtPlusGroup(b[1], b[2], b[3]); return; }
    if (code == ertGroup_ && cOk) { ertGroup(b[1], b[2], b[3]); return; }

    if (type == 0) {
        ta = (low >> 4) & 1; ms = (low >> 3) & 1;
        const int seg = low & 3;
        // each segment is kept as it comes (a group lost to noise no longer throws the others away, #46); a segment that comes again
        // with other characters means the name is changing (a scrolling name), and the collection starts over from it, so two
        // versions of the name are never mixed
        const char c0 = rdsChar(b[3] >> 8), c1 = rdsChar(b[3] & 0xFF);
        if ((psSeen_ >> seg & 1) && (psBuf_[seg * 2] != c0 || psBuf_[seg * 2 + 1] != c1)) psSeen_ = 0;
        psBuf_[seg * 2] = c0; psBuf_[seg * 2 + 1] = c1;
        psSeen_ |= (uint8_t)(1 << seg);
        psAnyBuf_[seg * 2] = c0; psAnyBuf_[seg * 2 + 1] = c1; psAnySeen_ |= (uint8_t)(1 << seg);
        psSince_++;
        if (psSeen_ == 15) { ps = rdsText(psBuf_, 8, charset); psSeen_ = 0; psAnySeen_ = 0; psSince_ = 0; info.psPartial = ps; }
        else if (psAnySeen_ == 15 && psSince_ >= 40) {
            // a name that scrolls changes between its segments, so the strict collection above never completes (Chinese stations in particular send a
            // moving name of 4 characters): after about 40 groups the latest segments are shown as one name, as a scrolling display would
            ps = rdsText(psAnyBuf_, 8, charset); psSeen_ = 0; psAnySeen_ = 0; psSince_ = 0; info.psPartial = ps;
        } else {
            char p[9] = "________";
            for (int s = 0; s < 4; s++) if (psSeen_ >> s & 1) { p[s * 2] = psBuf_[s * 2]; p[s * 2 + 1] = psBuf_[s * 2 + 1]; }
            if (ps.empty()) info.psPartial = rdsText(p, 8, charset);
        }
        if (!verB && cOk) {   // alternative frequencies (method A or B lists alike: the frequencies are collected)
            for (int k = 0; k < 2; k++) {
                const int af = k == 0 ? b[2] >> 8 : b[2] & 0xFF;
                if (af < 1 || af > 204) continue;
                const double mhz = 87.5 + 0.1 * af;
                auto& v = info.afMhz;
                if (std::none_of(v.begin(), v.end(), [&](double x) { return std::fabs(x - mhz) < 0.01; }) && v.size() < 32) {
                    v.push_back(mhz); std::sort(v.begin(), v.end());
                }
            }
        }
    } else if (type == 1) {
        if (!verB && cOk) {
            const int variant = (b[2] >> 12) & 7;
            if (variant == 0) info.ecc = b[2] & 0xFF;
            else if (variant == 3) info.language = b[2] & 0xFFF;
        }
        const int day = b[3] >> 11;
        if (day) { info.pinDay = day; info.pinHour = (b[3] >> 6) & 31; info.pinMinute = b[3] & 63; }
    } else if (type == 2) {
        const int ab = (low >> 4) & 1, seg = low & 15;
        if (ab != rtAb_ || (bool)verB != rt2B_) {
            std::memset(rtBuf_, ' ', 64); rtAb_ = ab; rt2B_ = verB; rt.clear(); rtSeen_ = 0;
            rtPlusRaw_[0].type = rtPlusRaw_[1].type = 0;   // the tags point into the old text
        }
        if (!verB && cOk) {
            rtBuf_[seg * 4] = rdsChar(b[2] >> 8); rtBuf_[seg * 4 + 1] = rdsChar(b[2] & 0xFF);
            rtBuf_[seg * 4 + 2] = rdsChar(b[3] >> 8); rtBuf_[seg * 4 + 3] = rdsChar(b[3] & 0xFF);
            rtSeen_ |= (uint16_t)(1u << seg);
        } else if (verB) {
            rtBuf_[seg * 2] = rdsChar(b[3] >> 8); rtBuf_[seg * 2 + 1] = rdsChar(b[3] & 0xFF);
            rtSeen_ |= (uint16_t)(1u << seg);
        }
        {   // complete: the segments up to the one holding the carriage return (2A: 4 characters each, 2B: 2), or all 16
            const int per = verB ? 2 : 4;
            const char* end = (const char*)std::memchr(rtBuf_, '\r', (size_t)(16 * per));
            const int last = end ? (int)(end - rtBuf_) / per : 15;
            const uint16_t need = (uint16_t)((2u << last) - 1);
            rtComplete = (rtSeen_ & need) == need;
        }
        std::string s(rtBuf_, verB ? 32 : 64);
        const size_t cr = s.find('\r');
        if (cr != std::string::npos) s.resize(cr);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        rt = rdsText(s.data(), s.size(), charset);
        rtPlusRefresh();
    } else if (type == 3 && !verB && cOk) {
        const int app = low, aid = b[3];
        auto& v = info.oda;
        auto it = std::find_if(v.begin(), v.end(), [&](const std::pair<int, int>& x) { return x.first == aid; });
        if (it == v.end()) { if (v.size() < 16) v.emplace_back(aid, app); } else it->second = app;
        if (aid == 0x4BD7 && app != 0) rtPlusGroup_ = app;        // RadioText+
        if (aid == 0x6552 && app != 0) { ertGroup_ = app; ertUtf8_ = b[2] & 1; }   // enhanced RadioText
        tmc.onGroup3A((uint16_t)low, b[2], b[3]);
    } else if (type == 4 && !verB && cOk) {
        if (corrected) return;   // the time changes every minute: a corrected one can never be confirmed
        const int mjd = ((b[1] & 3) << 15) | (b[2] >> 1);
        const int hour = ((b[2] & 1) << 4) | (b[3] >> 12), minute = (b[3] >> 6) & 63;
        const int off = (b[3] & 31) * 30 * (((b[3] >> 5) & 1) ? -1 : 1);
        if (hour < 24 && minute < 60 && mjd > 50000 && mjd < 80000 && std::abs(off) <= 15 * 60) {
            info.clockUtc = ((int64_t)mjd - 40587) * 86400 + hour * 3600 + minute * 60;
            info.clockOffsetMin = off;
            info.clockAt = now;
        }
    } else if (type == 8 && !verB && cOk) {
        tmc.onGroup8A((uint16_t)low, b[2], b[3], now);
    } else if (type == 10 && !verB && cOk) {
        const int ab = (low >> 4) & 1, seg = low & 1;
        if (ab != ptynAb_) { std::memset(ptynBuf_, ' ', 8); ptynSeen_ = 0; ptynAb_ = ab; }
        ptynBuf_[seg * 4] = rdsChar(b[2] >> 8); ptynBuf_[seg * 4 + 1] = rdsChar(b[2] & 0xFF);
        ptynBuf_[seg * 4 + 2] = rdsChar(b[3] >> 8); ptynBuf_[seg * 4 + 3] = rdsChar(b[3] & 0xFF);
        ptynSeen_ |= (uint8_t)(1 << seg);
        if (ptynSeen_ == 3) info.ptyn = rdsText(ptynBuf_, 8, charset);
    } else if (type == 14 && !verB && cOk) {
        const int variant = low & 15, on = b[3];
        if (variant <= 3 && on != 0 && on != pi) {
            auto it = std::find_if(eonBuf_.begin(), eonBuf_.end(), [&](const EonBuf& e) { return e.pi == on; });
            if (it == eonBuf_.end()) {
                if (eonBuf_.size() >= 16) return;
                EonBuf e{}; e.pi = on; std::memset(e.ps, ' ', 8); e.ps[8] = 0;
                eonBuf_.push_back(e); it = eonBuf_.end() - 1;
            }
            it->ps[variant * 2] = rdsChar(b[2] >> 8); it->ps[variant * 2 + 1] = rdsChar(b[2] & 0xFF);
            it->seen |= (uint8_t)(1 << variant);
            info.eon.clear();
            for (const auto& e : eonBuf_) info.eon.push_back({e.pi, e.seen == 15 ? rdsText(e.ps, 8, charset) : std::string()});
        }
    }
}

void RdsDecoder::rtPlusGroup(uint16_t b, uint16_t c, uint16_t d) {
    const int toggle = (b >> 4) & 1;
    info.rtPlusRunning = (b >> 3) & 1;
    if (toggle != rtPlusToggle_) { info.rtPlus.clear(); rtPlusToggle_ = toggle; }
    rtPlusRaw_[0] = {((b & 7) << 3) | (c >> 13), (c >> 7) & 63, (c >> 1) & 63};
    rtPlusRaw_[1] = {((c & 1) << 5) | (d >> 11), (d >> 5) & 63, d & 31};
    rtPlusRefresh();
}

void RdsDecoder::rtPlusRefresh() {
    const int per = rt2B_ ? 2 : 4, len = rt2B_ ? 32 : 64;
    for (const RawTag& t : rtPlusRaw_) {
        if (t.type <= 0 || t.start + t.len >= len) continue;
        bool all = true;   // every segment the tag covers has come
        for (int s = t.start / per; s <= (t.start + t.len) / per; s++) if (!(rtSeen_ >> s & 1)) all = false;
        if (!all) continue;
        const std::string text = trimmed(rdsText(rtBuf_ + t.start, (size_t)t.len + 1, charset));
        if (text.empty() || text.find('\r') != std::string::npos) continue;
        auto it = std::find_if(info.rtPlus.begin(), info.rtPlus.end(), [&](const RtPlusTag& x) { return x.type == t.type; });
        if (it == info.rtPlus.end()) { info.rtPlus.push_back({t.type, rtPlusTypeName(t.type), text}); }
        else it->text = text;
    }
}

void RdsDecoder::ertGroup(uint16_t b, uint16_t c, uint16_t d) {
    const int seg = b & 31;
    ertBuf_[seg * 4] = (uint8_t)(c >> 8); ertBuf_[seg * 4 + 1] = (uint8_t)c;
    ertBuf_[seg * 4 + 2] = (uint8_t)(d >> 8); ertBuf_[seg * 4 + 3] = (uint8_t)d;
    ertSeen_ |= 1u << seg;
    std::string s;
    if (ertUtf8_) {
        for (int i = 0; i < 128 && ertBuf_[i] != 0x0D; i++) if (ertBuf_[i]) s += (char)ertBuf_[i];
        // cut a multi-byte character that has not fully come yet
        size_t k = s.size();
        while (k > 0 && ((uint8_t)s[k - 1] & 0xC0) == 0x80) k--;
        if (k > 0 && ((uint8_t)s[k - 1] & 0x80)) {
            const uint8_t lead = (uint8_t)s[k - 1];
            const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if (s.size() - (k - 1) < need) s.resize(k - 1);
        }
    } else {
        for (int i = 0; i + 1 < 128; i += 2) {
            const uint32_t cp = (uint32_t)ertBuf_[i] << 8 | ertBuf_[i + 1];
            if (cp == 0x0D) break;
            if (cp) putUtf8(s, cp);
        }
    }
    info.ert = trimmed(s);
}

} // namespace dect2
