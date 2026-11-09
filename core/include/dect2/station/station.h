// My station: the user's radios and antennas, entered once. From them: can a mode be received here and what is missing (the mode list), which radio
// and antenna a mode should use (selecting a mode), which free radio each of several modes gets (several receivers), and whether several modes fit
// one capture (the Everything in view window). Pure logic: no ImGui, no engine, tests in tests/station/test_station.cpp, docs/STATION-3.8.md.
// Nothing here refuses anything: a verdict is a note, the user can always tune any radio anywhere.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::station {

enum AntennaFlag : uint8_t {
    AActive = 1,      // has an amplifier at the antenna (GNSS patch, active loop)
    ADish = 2,        // a dish with an LNB: the radio sees the L-band IF, so its own frequency range does not matter
    ACable = 4        // a cable TV feed, not an antenna
};

struct Radio {
    std::string id;           // same for the same physical radio (deviceClaimKey, or the name)
    std::string name;
    double minHz = 0, maxHz = 0;   // what it tunes, 0 = not known
    double maxRateHz = 0;          // fastest sample rate, 0 = not known
    bool biasTee = false;          // can power an active antenna
    bool present = true;           // plugged in now (a saved radio that is not connected stays in the list and is never picked)
};

struct Antenna {
    std::string name;
    double loHz = 0, hiHz = 0;     // the band it is good for
    uint8_t flags = 0;             // AntennaFlag
    std::string radioId;           // the radio it is connected to; "" = any (moved by hand)
};

struct Station {
    std::vector<Radio> radios;
    std::vector<Antenna> antennas;
};

// A one-line text per radio and antenna, for the settings; unknown lines are skipped so a newer file still loads.
std::string toText(const Station& s);
Station fromText(const std::string& text);

// The common antennas, so adding one is a single click
struct AntennaPreset { const char* name; double loHz, hiHz; uint8_t flags; };
const std::vector<AntennaPreset>& antennaPresets();

// What a mode needs
struct ModeNeed {
    std::string id;           // for the caller only
    double freqHz = 0;        // 0 = not known
    double minRateHz = 0;     // 0 = not known
    uint8_t flags = 0;        // AActive / ADish / ACable: the antenna it must have
};

enum class Level { Unknown = 0, Cannot, Partial, Ready };   // Unknown: no radio entered, no claim made

enum class Missing { RadioRange, RadioRate, NoRadioPlugged, AntennaBand, ActiveAntenna, Dish, CableFeed, NoAntennaPower };

struct Verdict {
    Level level = Level::Unknown;
    int radio = -1, antenna = -1;     // the best pair (index in the Station); -1 = none
    std::vector<Missing> missing;     // what stands between this pair and Ready
    double score = 0;                 // higher is better; only comparable for the same need
};

// The best radio and antenna for the need. busy[i] = radio i is taken by another receiver (may be empty). A busy or unplugged radio is never chosen:
// when no other is left the verdict is Cannot, radio = -1, with NoRadioPlugged first and what the best of them would still lack after it.
Verdict assess(const Station& s, const ModeNeed& n, const std::vector<bool>& busy = {});
// The same check for one radio (the selected one)
Verdict assessRadio(const Station& s, const ModeNeed& n, int radio);

// Should selecting this mode change the radio? Returns the radio to use, or `current` when the current one is as good as the best (or there is no
// better): a working choice of the user is never overruled. current = -1: not one of the station's radios (test signal, file): stays -1.
int pickRadio(const Station& s, const ModeNeed& n, int current, const std::vector<bool>& busy = {});

// Several modes at once, one radio each: the modes in `wanted` get the free radios so that as many as possible are Ready, then Partial, then by score;
// an earlier mode wins a tie for the only radio. Optimal (dynamic programming over the radios), up to 16 radios. result[i] = radio of wanted[i], -1 none.
std::vector<int> assignRadios(const Station& s, const std::vector<ModeNeed>& wanted, const std::vector<bool>& busy = {});

// Several modes on ONE capture: where to tune and how fast to sample so that every channel is inside, keeping the usable 80% of the span.
struct CaptureChannel { double freqHz = 0; double widthHz = 0; };
struct CapturePlan {
    bool fits = false;
    double centerHz = 0, rateHz = 0;
    std::vector<int> outside;       // indices of the channels that could not be included (the widest group that fits is kept; ties: the earlier channels)
};
// maxRateHz 0: no limit. The rate is the smallest that holds them, at least minRateHz; the centre avoids landing on a channel (DC spike) when it can.
CapturePlan planCapture(const std::vector<CaptureChannel>& ch, double maxRateHz, double minRateHz = 0);

} // namespace dect2::station
