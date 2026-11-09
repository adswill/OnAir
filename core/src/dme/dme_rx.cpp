// DME receiver: envelope pulses -> pulse pairs (X 12 us, Y 36 us) -> pair rate, channel and the Morse ident. See dme_rx.h.
#include "dect2/dme/dme_rx.h"

#include <algorithm>
#include <cmath>

namespace dect2::dme {

namespace {
constexpr double kBinSec = 0.025;        // housekeeping step: letters are closed and the rate updated every 25 ms
constexpr double kDashSec = 0.2;         // a key-down longer than this is a dash (dot 0.1 - 0.16 s, dash three dots)
constexpr double kLetterGapSec = 0.2;    // a key-up this long ends a letter (between elements one dot, between letters three dots)
constexpr double kWordGapSec = 0.5;      // a key-up this long ends a word
constexpr int kKeyRun = 8;               // regular intervals in a row that make a key-down (random traffic does that about once in 10^12 pairs)
constexpr double kMinKeySec = 0.05;      // a key-down shorter than this is not an element
constexpr size_t kIdentMax = 40;
} // namespace

const char* channelName(Channel c) {
    switch (c) {
    case Channel::X: return "X";
    case Channel::Y: return "Y";
    default: return "none";
    }
}

char morseLetter(const std::string& code) {
    static const struct { const char* code; char ch; } kTable[] = {
        {".-", 'A'},     {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'},   {".", 'E'},     {"..-.", 'F'}, {"--.", 'G'},
        {"....", 'H'},   {"..", 'I'},   {".---", 'J'}, {"-.-", 'K'},   {".-..", 'L'},  {"--", 'M'},    {"-.", 'N'},
        {"---", 'O'},    {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'},   {"...", 'S'},   {"-", 'T'},     {"..-", 'U'},
        {"...-", 'V'},   {".--", 'W'},  {"-..-", 'X'}, {"-.--", 'Y'},  {"--..", 'Z'},  {"-----", '0'}, {".----", '1'},
        {"..---", '2'},  {"...--", '3'}, {"....-", '4'}, {".....", '5'}, {"-....", '6'}, {"--...", '7'}, {"---..", '8'},
        {"----.", '9'},
    };
    for (const auto& e : kTable)
        if (code == e.code) return e.ch;
    return '?';
}

void Receiver::configure(double sampleRate) {
    fs_ = sampleRate;
    decay_ = fs_ > 0 ? std::exp(-1.0 / (0.2 * fs_)) : 0.0;    // peak follower, time constant 0.2 s
    binSamples_ = fs_ > 0 ? static_cast<uint64_t>(std::llround(kBinSec * fs_)) : 0;
    reset();
}

void Receiver::reset() {
    peak_ = 0.0;
    inPulse_ = false;
    startSample_ = 0;
    sampleCount_ = 0;
    havePrev_ = false;
    prevStartSec_ = 0.0;
    pairs_.clear();
    binEnd_ = binSamples_;
    binPairs_ = 0;
    keyed_ = false;
    havePair_ = false;
    lastPairSec_ = keyStartSec_ = lastRegSec_ = offStartSec_ = 0.0;
    regular_ = 0;
    intervalSum_ = 0.0;
    letterFlushed_ = false;
    wordFlushed_ = false;
    code_.clear();
    t_ = Telemetry();
    tacan_.reset();
    pulsePeak_ = 0.f;
    tInPulse_ = false;
    tStart_ = 0;
}

void Receiver::feed(const float* env, size_t n) {
    if (fs_ <= 0.0 || binSamples_ == 0) return;
    for (size_t k = 0; k < n; k++) {
        const double x = env[k];
        peak_ = std::max(x, peak_ * decay_);
        const bool above = peak_ > 0.0 && x > 0.5 * peak_;     // a pulse is the envelope above half its recent peak
        if (!inPulse_ && above) {
            inPulse_ = true;
            startSample_ = sampleCount_;
        } else if (inPulse_ && !above) {
            inPulse_ = false;
            const double widthUs = (double)(sampleCount_ - startSample_) * 1e6 / fs_;
            if (std::fabs(widthUs - kPulseWidthUs) <= kPulseTolUs) onPulse(startSample_ / fs_);
        }
        // TACAN: a lower threshold than the DME pulse rule, or the bursts and replies are lost in the troughs of the 15 and 135 Hz modulation
        {
            const bool tAbove = peak_ > 0.0 && x > 0.3 * peak_;
            if (!tInPulse_ && tAbove) { tInPulse_ = true; tStart_ = sampleCount_; pulsePeak_ = 0.f; }
            if (tInPulse_ && (float)x > pulsePeak_) pulsePeak_ = (float)x;
            if (tInPulse_ && !tAbove) {
                tInPulse_ = false;
                const double us = (double)(sampleCount_ - tStart_) * 1e6 / fs_;
                if (us > 2.0 && us < 8.0) tacan_.pulse(tStart_ / fs_, pulsePeak_);
            }
        }
        sampleCount_++;
        if (sampleCount_ >= binEnd_) {
            closeBin();
            binEnd_ += binSamples_;
        }
    }
    t_.levelDb = 20.0 * std::log10(peak_ + 1e-12);
}

void Receiver::onPulse(double startSec) {
    if (havePrev_) {
        const double dtUs = (startSec - prevStartSec_) * 1e6;
        Channel ch = Channel::None;
        bool interrogation = false;
        if (std::fabs(dtUs - kSpacingXUs) <= kSpacingTolUs) ch = Channel::X;
        else if (std::fabs(dtUs - kSpacingYUs) <= kSpacingTolUs) ch = Channel::Y;
        else if (std::fabs(dtUs - kSpacingYInterrogationUs) <= kSpacingTolUs) { ch = Channel::Y; interrogation = true; }
        if (ch != Channel::None) {
            pairs_.emplace_back(prevStartSec_, ch);
            if (ch == Channel::X) t_.pairsX++;
            else { t_.pairsY++; if (interrogation) t_.pairsYInterrogation++; }
            binPairs_++;
            havePrev_ = false;          // the second pulse is used up: the next pulse starts a new pair
            onPair(prevStartSec_);
            return;
        }
    }
    havePrev_ = true;
    prevStartSec_ = startSec;
}

// A pair at startSec. A run of pairs at the 1350 pp/s period is the keyed ident (replies and squitter are random).
void Receiver::onPair(double startSec) {
    const double period = 1.0 / kIdentRatePps;
    if (havePair_) {
        const double dt = startSec - lastPairSec_;
        if (std::fabs(dt - period) <= kIdentTolUs * 1e-6) {
            regular_++;
            intervalSum_ += dt;
            if (regular_ >= kKeyRun) {
                if (!keyed_) {
                    // a new key-down: close the letter that the gap before it ended, then start counting
                    const double off = (startSec - (double)kKeyRun * period) - offStartSec_;
                    if (!code_.empty() && off >= kLetterGapSec) decodeLetter();
                    keyed_ = true;
                    keyStartSec_ = startSec - (double)kKeyRun * period;
                    letterFlushed_ = false;
                    wordFlushed_ = false;
                }
                lastRegSec_ = startSec;
                t_.identIntervalUs = intervalSum_ / regular_ * 1e6;
            }
        } else if (dt > 0.5 * period) {   // another pair that is not at the period: the run is over (a pair closer than half a period is a double detection)
            regular_ = 0;
            intervalSum_ = 0;
        }
    }
    havePair_ = true;
    lastPairSec_ = startSec;
}

void Receiver::endKey() {
    const double period = 1.0 / kIdentRatePps;
    const double dur = lastRegSec_ - keyStartSec_ + period;
    if (dur >= kMinKeySec) code_ += dur < kDashSec ? '.' : '-';
    keyed_ = false;
    regular_ = 0;
    intervalSum_ = 0;
    offStartSec_ = lastRegSec_ + period;
    letterFlushed_ = false;
    wordFlushed_ = false;
}

void Receiver::closeBin() {
    binPairs_ = 0;
    const double now = timeSec();
    if (keyed_ && now - lastRegSec_ > 3.0 / kIdentRatePps) endKey();       // no regular pair for three periods: key up
    if (!keyed_) {                       // the key-up: a letter gap ends the letter, a longer one the word
        const double off = now - offStartSec_;
        if (!code_.empty() && !letterFlushed_ && off >= kLetterGapSec) {
            decodeLetter();
            letterFlushed_ = true;
        }
        if (!wordFlushed_ && !t_.ident.empty() && off >= kWordGapSec) {
            if (t_.ident.back() != ' ') t_.ident += ' ';
            wordFlushed_ = true;
        }
    }
    t_.identKeyed = keyed_;
    updateTelemetry(now);
}

void Receiver::decodeLetter() {
    t_.ident += morseLetter(code_);
    code_.clear();
    if (t_.ident.size() > kIdentMax) t_.ident.erase(0, t_.ident.size() - kIdentMax);
}

void Receiver::updateTelemetry(double now) {
    while (!pairs_.empty() && pairs_.front().first < now - 1.0) pairs_.pop_front();
    int nx = 0, ny = 0;
    for (const auto& p : pairs_) {
        if (p.second == Channel::X) nx++;
        else ny++;
    }
    const double window = std::max(0.05, std::min(1.0, now));
    t_.pairRatePps = static_cast<double>(pairs_.size()) / window;
    t_.tacanValid = tacan_.valid(); t_.tacanRadialDeg = tacan_.radialDeg(); t_.tacanDepth15 = tacan_.depth15(); t_.tacanDepth135 = tacan_.depth135();
    if (nx == 0 && ny == 0) t_.channel = Channel::None;
    else t_.channel = nx >= ny ? Channel::X : Channel::Y;
}

int channelFromReplyMHz(double mhz, bool& y) {
    const long f = std::lround(mhz);
    if (std::fabs(mhz - (double)f) > 0.01 || f < 962 || f > 1213) return 0;
    struct R { long lo, hi; int first; bool isY; };   // reply range, channel number of its first frequency, X or Y
    static const R kPlan[] = {
        {962, 977, 1, false},     // 1X - 16X
        {978, 1020, 17, true},    // 17Y - 59Y
        {1021, 1030, 60, false},  // 60X - 69X
        {1031, 1087, 70, true},   // 70Y - 126Y
        {1088, 1103, 1, true},    // 1Y - 16Y
        {1104, 1146, 17, false},  // 17X - 59X
        {1147, 1156, 60, true},   // 60Y - 69Y
        {1157, 1213, 70, false},  // 70X - 126X
    };
    for (const R& r : kPlan)
        if (f >= r.lo && f <= r.hi) { y = r.isY; return r.first + (int)(f - r.lo); }
    return 0;
}

} // namespace dect2::dme
