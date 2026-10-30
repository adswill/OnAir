// TV channel plans: the numbered channels of a country or region for every kind of TV OnAir receives (DVB-T/T2, ATSC 1.0/3.0, ISDB-T/Tb, DTMB, DVB-C,
// analog). Pure data plus lookup, no UI; the TV modes' "Channel" selector (app/bandplan/tvchan_ui.cpp), the setup wizard (country -> plan) and
// tests/channels/test_tvplan.cpp use it. Tables and sources: docs/TV-CHANNELS.md. Frequencies in Hz.
//
// "Channel 2" is not one frequency worldwide: a plan belongs to a standard AND a region, so the selector first picks the plan (default from the country).
#pragma once
#include <string>
#include <vector>

namespace dect2::tvplan {

enum Std : unsigned {
    DvbT = 1, DvbT2 = 2, Atsc = 4, Atsc3 = 8, IsdbT = 16, Dtmb = 32, DvbC = 64, Analog = 128,
    DvbAny = DvbT | DvbT2,
};

struct Channel {
    std::string label;        // "21", "E5", "S11", "9A": what the plan calls the channel
    double centreHz = 0;      // the frequency to tune: the centre of the channel (ISDB-T: with its 1/7 MHz offset)
    double bwHz = 0;          // channel width
    bool legacy = false;      // reallocated or analog-only today; still listed so it can be tuned
    double lowHz() const { return centreHz - bwHz / 2; }
    double highHz() const { return centreHz + bwHz / 2; }
};

struct Plan {
    std::string id, name, region;
    unsigned stds = 0;                     // Std bits the channels are valid for
    std::vector<std::string> countries;    // ISO 3166 codes that use it by default
    bool verified = true;                  // false: the raster is from one secondary source only (see docs/TV-CHANNELS.md)
    bool overlaps = false;                 // the plan's own channels overlap (Japan VHF 7 / 8): the tests allow it
    std::string source, note;
    // analog plans: the system and where the carriers sit (vision carrier above the lower channel edge; sound above the vision carrier)
    std::string system;                    // "B/G", "I", "D/K", "L", "M" ... empty for digital plans
    double visionOffHz = 0, soundOffHz = 0;
    std::vector<Channel> channels;         // ascending by centre

    // by label ("21", "e21", "ch 21", "S 11"); nullptr when not in the plan
    const Channel* byLabel(const std::string& label) const;
    // the channel whose centre is nearest to hz and within tolHz (default: half of its width); nullptr when none
    const Channel* at(double hz, double tolHz = -1) const;
    int indexOf(const Channel* c) const { return c ? (int)(c - channels.data()) : -1; }
    // the channel after / before c on the raster (dir +1 / -1); nullptr at the end. c may be null: the first channel above / below fromHz
    const Channel* step(const Channel* c, int dir, double fromHz = 0) const;
};

const std::vector<Plan>& plans();                      // built once
const Plan* find(const std::string& id);
std::vector<const Plan*> plansFor(unsigned stdMask);   // plans carrying any of these standards, in listing order
// the plan to start from for a country (ISO code, any case) and a standard: the plan that lists the country, else the first plan of the
// standard (the Europe plan for DVB, the US plan for ATSC ...); null only for a standard without any plan
const Plan* defaultPlan(const std::string& country, unsigned stdMask);
// "21 (474 MHz)" for a frequency within a plan, or "" when no channel is there
std::string describe(const Plan& p, double hz);

} // namespace dect2::tvplan
