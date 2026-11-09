// "What is this station" on shortwave: an estimate built from what is on the air and what the user has installed. Language comes from the user-installed speech model (the transcriber's entry language),
// a repeating interval signal from the audio itself, and station names only from a schedule file the user imports (EiBi or HFCC style; none is shipped) or from interval signals the user has taught.
// Every answer is labelled estimated.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace dect2 {

struct SwSchedEntry {
    double khz = 0;
    int startMin = 0, endMin = 1440;   // UTC minutes of the day
    uint8_t days = 0x7F;               // bit 0 Monday .. bit 6 Sunday
    std::string station, lang, itu, target;
};

// EiBi's semicolon CSV ("kHz:75;Time(UTC):93;Days:59;ITU:49;Station:201;Lng:49;Target:62;...") -> entries; unknown lines are skipped
std::vector<SwSchedEntry> parseEibi(const std::string& text);

// A signal that repeats over and over (an interval signal: a short melody between programmes): found from the self-similarity of its 12-note chroma.
class IntervalDetector {
public:
    void configure(int audioRate);
    void reset();
    void push(const float* x, size_t n);
    bool repeating() const { return strength_ >= 0.6f; }
    float periodSec() const { return period_; }
    float strength() const { return strength_; }
    // The melody as a chroma loop (for "teach this signal"), empty until repeating()
    std::vector<float> fingerprint() const;
    static float match(const std::vector<float>& a, const std::vector<float>& b);   // 0..1, invariant to where the loop starts
private:
    void analyse();
    int rate_ = 48000, hop_ = 0, fftN_ = 0;
    std::vector<float> buf_;
    size_t fill_ = 0;
    std::deque<std::vector<float>> chroma_;
    float period_ = 0, strength_ = 0;
    int periodFrames_ = 0;
    uint64_t since_ = 0;
};

struct SwGuess {
    std::string station, why;
    float score = 0;
    bool estimated = true;
};

struct SwIdentInput {
    double khz = 0;
    int utcMinute = 0;                 // minutes of the day
    int weekdayMon0 = 0;               // 0 Monday
    std::string language;              // ISO 639-1 from the speech model; "" when none
    float languageProb = 0;
    bool intervalSignal = false;
    std::vector<float> intervalPrint;  // from IntervalDetector::fingerprint
};

class SwIdent {
public:
    void addSchedule(std::vector<SwSchedEntry> e);
    size_t scheduleSize() const { return sched_.size(); }
    void teachInterval(const std::string& name, const std::vector<float>& print) { taught_.push_back({name, print}); }
    std::vector<SwGuess> guess(const SwIdentInput& in) const;     // best first; empty when nothing fits
private:
    std::vector<SwSchedEntry> sched_;
    struct Taught { std::string name; std::vector<float> print; };
    std::vector<Taught> taught_;
};

} // namespace dect2
