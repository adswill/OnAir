// RDS-TMC (Traffic Message Channel, ISO 14819-1/2/3): the ODA announcement in group 3A and the traffic messages in group 8A.
// Location codes stay numbers: the location tables (one per country, "LTN") are national data and are not shipped.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct TmcMessage {
    int event = 0;              // ISO 14819-2 event code (1 - 2047)
    std::string text;           // the event in English ("event N" when it is not in the built-in table)
    std::vector<std::string> more;   // additional events and details (speed limit, length of the route) from multi-group messages
    struct Detail { int label = 0, value = 0; };   // the same details as numbers (ISO 14819-1 label, value): 2 length, 3 speed, 9 event, 10 diversion, 11 destination
    std::vector<Detail> details;
    int location = 0;           // location code in the table LTN
    int extent = 0;             // locations the message reaches beyond the first (0 - 7)
    bool negative = false;      // direction: true = the direction of decreasing location numbers
    bool diversion = false;     // diversion advised
    bool multiGroup = false;
    int durationCode = 0;       // 0 = none given (the event's default), 1 - 7
    std::string durationText;   // "30 minutes", "rest of the day", ... ("" = default)
    double received = 0;        // first time received, seconds since 1970
    double lastSeen = 0;        // last time it came round again
    double expires = 0;         // lastSeen + duration (or the default)
    int count = 1;              // times received
};

struct TmcSystem {
    bool seen = false;          // a 3A group announced TMC
    int aid = 0;                // 0xCD46 or 0xCD47
    int ltn = -1;               // location table number, -1 = not received yet
    int afi = -1;               // the service's alternative frequencies are the same as the programme's
    int mode = -1;              // 0 = basic mode, 1 = enhanced mode
    int sid = -1;               // service identifier
};

class TmcDecoder {
public:
    static constexpr size_t kMaxMessages = 64;
    void reset() { *this = TmcDecoder(); }
    // Group 3A (all four blocks good): block B low 5 bits, C and D.
    void onGroup3A(uint16_t b, uint16_t c, uint16_t d);
    // Group 8A (blocks B, C and D good): B low 5 bits, C, D. `now` = seconds since 1970.
    void onGroup8A(uint16_t b, uint16_t c, uint16_t d, double now);
    const TmcSystem& system() const { return sys_; }
    // The messages that have not expired at `now`, newest first.
    std::vector<TmcMessage> messages(double now) const;
    bool active() const { return sys_.seen; }

    static std::string eventText(int code, bool* longLasting = nullptr);   // the built-in English table (about 170 frequent events)
    static bool eventKnown(int code);                                       // in the built-in table
    // The duration or persistence text of a duration code (1 - 7) for a dynamic (longLasting false) or longer-lasting event; "" for 0
    static std::string durationText(int code, bool longLasting);

private:
    struct Pending {
        bool open = false;
        TmcMessage m;
        bool longLasting = false;
        int remaining = -1;     // groups still to come after the last one received; -1 = second group not seen yet
        std::vector<uint8_t> bits;
    };
    void finish(Pending& p, double now);
    void store(TmcMessage m, double now);
    TmcSystem sys_;
    Pending pend_[8];
    std::vector<TmcMessage> msgs_;
};

} // namespace dect2
