// Pagers receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// The speeds the receiver tells apart. Index into PagerTelemetry::speeds.
enum PagerSpeed { kPocsag512, kPocsag1200, kPocsag2400, kFlex1600_2, kFlex3200_2, kFlex3200_4, kFlex6400_4, kPagerSpeeds };
inline const char* pagerSpeedName(int s) {
    static const char* const n[kPagerSpeeds] = {"POCSAG 512", "POCSAG 1200", "POCSAG 2400", "FLEX 1600/2", "FLEX 3200/2", "FLEX 3200/4", "FLEX 6400/4"};
    return s >= 0 && s < kPagerSpeeds ? n[s] : "?";
}

enum PagerMsgType { kPagerTone, kPagerNumeric, kPagerAlpha, kPagerSecure, kPagerBinary };
inline const char* pagerTypeName(int t) {
    static const char* const n[] = {"tone", "numeric", "alpha", "secure", "binary"};
    return t >= 0 && t <= kPagerBinary ? n[t] : "?";
}

// One page. Strings are plain text; a character that is not printable is dropped.
struct PagerMessage {
    uint64_t serial = 0;         // grows by one per message, never restarts: a list row keeps its identity
    double timeSec = 0;          // seconds of signal time when it was decoded
    int64_t wallTime = 0;        // wall clock (seconds since 1970)
    int speed = 0;               // PagerSpeed
    bool flex = false;
    uint32_t address = 0;        // POCSAG: the 21-bit RIC; FLEX: the capcode
    int function = -1;           // POCSAG function bits 0 to 3; -1 for FLEX
    int type = kPagerTone;       // PagerMsgType
    std::string text;            // numeric digits or text; "secure, not shown" for secure pages
    int fixed = 0;               // bits that the BCH code corrected in the code words of this page
    bool damaged = false;        // a code word of the message could not be repaired: the text is incomplete
    int copies = 1;              // transmissions of this message that were combined (multi-copy combining, #20); 1 = a single copy
    bool combined = false;       // code words were rebuilt from other copies of the message
    // POCSAG pages with a message: both readings, so the UI can switch between them; empty for FLEX and tone pages
    std::string numText, alphaText;
    bool looksAlpha = false;     // the alphanumeric reading looks like real text (pager::pocsagLooksAlpha)
    int readHow = 0;             // PocsagReadHow: why type was picked
    // whole-span decoding (wide_modes.h): the carrier that sent it; carrierId 0 = the single receiver. carrierHz: relative to the radio's centre.
    int carrierId = 0;
    double carrierHz = 0;
};

// How a POCSAG message is read. Networks often ignore the standard (function 0 = numeric): Australian emergency paging sends text with function 0.
enum PocsagReadMode { kPocsagReadAuto, kPocsagReadByFunction, kPocsagReadAlpha, kPocsagReadNumeric, kPocsagReadModes };
inline const char* pocsagReadModeName(int m) {
    static const char* const n[] = {"Auto", "By function code", "Force alphanumeric", "Force numeric"};
    return m >= 0 && m < kPocsagReadModes ? n[m] : "?";
}
enum PocsagReadHow { kPocsagHowFunction, kPocsagHowAuto, kPocsagHowForced, kPocsagHowAddress };
inline const char* pocsagReadHowName(int h) {
    static const char* const n[] = {"function", "auto", "forced", "address"};
    return h >= 0 && h <= kPocsagHowAddress ? n[h] : "?";
}
// National character sets for the 7-bit alphanumeric text. German (DIN 66003, used by German POCSAG networks): [ \\ ] { | } ~ are Ä Ö Ü ä ö ü ß.
enum PagerCharset { kPagerCharAscii = 0, kPagerCharGerman = 1, kPagerCharsets = 2 };
inline const char* pagerCharsetName(int c) { return c == kPagerCharGerman ? "German (ä ö ü ß)" : "ASCII"; }
inline std::string pagerApplyCharset(const std::string& in, int cs) {
    if (cs != kPagerCharGerman) return in;
    std::string o;
    o.reserve(in.size() + 4);
    for (char c : in) {
        switch (c) {
        case '[': o += "\xC3\x84"; break;    // Ä
        case '\\': o += "\xC3\x96"; break;   // Ö
        case ']': o += "\xC3\x9C"; break;    // Ü
        case '{': o += "\xC3\xA4"; break;    // ä
        case '|': o += "\xC3\xB6"; break;    // ö
        case '}': o += "\xC3\xBC"; break;    // ü
        case '~': o += "\xC3\x9F"; break;    // ß
        default: o += c;
        }
    }
    return o;
}

// Picks the reading of a POCSAG message (type, text, readHow) for a mode; byAddress marks a per-address override. FLEX and tone pages are left alone.
inline void pocsagApplyRead(PagerMessage& m, int mode, bool byAddress = false) {
    if (m.flex || (m.type != kPagerNumeric && m.type != kPagerAlpha)) return;
    bool alpha;
    if (mode == kPocsagReadAlpha || mode == kPocsagReadNumeric) { alpha = mode == kPocsagReadAlpha; m.readHow = byAddress ? kPocsagHowAddress : kPocsagHowForced; }
    else if (mode == kPocsagReadByFunction) { alpha = m.function != 0; m.readHow = kPocsagHowFunction; }
    else {
        // text-like wins; otherwise numeric, except that a page sent as alphanumeric (function 1 to 3) stays so unless its numeric reading is plain digits
        bool plain = !m.numText.empty();
        for (char c : m.numText) plain = plain && ((c >= '0' && c <= '9') || c == ' ' || c == '-');
        alpha = m.looksAlpha || (m.function != 0 && !plain);
        m.readHow = kPocsagHowAuto;
    }
    m.type = alpha ? kPagerAlpha : kPagerNumeric;
    m.text = alpha ? m.alphaText : m.numText;
}

// Code words of one speed: good as received, repaired by the BCH code, lost.
struct PagerSpeedStat {
    uint64_t ok = 0, fixed = 0, failed = 0;
    uint64_t messages = 0;
    uint64_t transmissions = 0;  // batches (POCSAG) or frames (FLEX) that were locked and had at least one good code word
};

struct PagerTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 searching, 1 receiving (locked on a transmission now, or one ended less than 2 s ago)
    double cfoHz = 0;            // carrier error, from the last sync
    float snrDb = 0;             // channel power over the noise floor, dB
    float levelDb = -200;        // power of the whole input, dBFS (-200 = nothing measured yet)
    bool dataValid = false;      // a page has been decoded
    uint64_t blocksOk = 0;       // code words: good (also the repaired ones) and lost
    uint64_t blocksBad = 0;
    // set-up
    double timeSec = 0;          // signal seconds since the start (now)
    double inputRate = 0;
    // ---- the rest is for the pager screens
    bool signal = false;         // the channel has a carrier now
    bool sync = false;           // locked on a transmission now
    int lastSpeed = -1;          // PagerSpeed of the last lock, -1 = none yet
    double lastPocsagSec = -1e9; // signal time of the last good POCSAG / FLEX code word
    double lastFlexSec = -1e9;
    uint64_t messagesTotal = 0;
    PagerSpeedStat speeds[kPagerSpeeds];
    std::shared_ptr<const std::vector<PagerMessage>> messages;   // newest first, at most 2000; null = none
};

inline std::string pagerSummary(const PagerTelemetry& t) {
    char b[160];
    if (t.state == 0 && !t.dataValid) {
        snprintf(b, sizeof b, "Pagers: searching, level %.1f dBFS", t.levelDb);
        return b;
    }
    snprintf(b, sizeof b, "Pagers: %s  %llu pages  %llu good / %llu lost code words", t.lastSpeed >= 0 ? pagerSpeedName(t.lastSpeed) : "no sync",
             (unsigned long long)t.messagesTotal, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    return b;
}

} // namespace dect2
