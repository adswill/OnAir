// The first-run setup and the lessons, pure part (no ImGui, no App): the goals, the antennas, the regions, a one-line note per radio,
// the lesson table (data: add a row to add a lesson), the step checks, the saved progress, the checkup rules and the glossary.
// Drawn by wizard.cpp (the setup) and learn.cpp (lessons, glossary, checkup); tested by tests/app/test_learn.cpp.
// Texts are N_() and go through tr() when shown. House style: plain words, no exclamation marks.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#ifndef N_
#define N_(s) s
#define LEARN_UNDEF_N_
#endif

namespace learn {

// ---- goals: "What do you want to do?" ----
enum Goal { GoalListen, GoalAirband, GoalTv, GoalTrack, GoalHam, GoalSat, GoalScan, GoalExplore, GoalCount };

struct GoalDef {
    const char* label;   // N_()
    const char* blurb;   // N_(): one line, what it means
    const char* modeId;  // the starting mode ("fm" and "tv" are resolved per region by the app)
};
inline const GoalDef* goals() {
    static const GoalDef k[GoalCount] = {
        {N_("Listen to FM radio"), N_("Music and news from local FM stations. The easiest place to start."), "fm"},
        {N_("Listen to aircraft (airband)"), N_("Pilots and control towers talking, between 118 and 137 MHz."), "airband"},
        {N_("Watch TV"), N_("Free digital TV from the nearest transmitter. Needs a stronger signal than radio."), "tv"},
        {N_("Track aircraft and ships"), N_("Planes (ADS-B) and ships (AIS) on a map, from the positions they send."), "adsb"},
        {N_("Decode ham radio modes"), N_("FT8 and other amateur radio data modes on the short-wave bands."), "ft8"},
        {N_("Satellites and weather images"), N_("Weather satellite pictures and satellite passes over your place."), "meteor"},
        {N_("Scan everything"), N_("Sweep wide bands and find what is on the air near you."), "specan"},
        {N_("Just explore"), N_("Look around the spectrum and try modes as you go."), "specan"},
    };
    return k;
}

// ---- antennas ----
enum Antenna { AntWhip, AntDipole, AntDiscone, AntLoop, AntLong, AntNone, AntCount };
struct AntennaDef { const char* label; const char* blurb; };
inline const AntennaDef* antennas() {
    static const AntennaDef k[AntCount] = {
        {N_("The small antenna that came with the radio"), N_("A short telescopic or magnet whip. Fine for FM and strong local signals.")},
        {N_("A dipole (two rods in a V or a line)"), N_("Good when its length matches the band. Turn the rods for each band.")},
        {N_("A discone (a cone of rods, usually outdoors)"), N_("Wideband, about 25 MHz to 1.3 GHz. Good for airband and scanning.")},
        {N_("A loop antenna"), N_("Quiet on the long and short waves, good indoors for HF.")},
        {N_("A long wire"), N_("A wire many metres long. Good for short wave, needs an unun or tuner.")},
        {N_("No antenna, or I do not know"), N_("Without an antenna almost nothing is heard. The demo signal works without one.")},
    };
    return k;
}

// ---- regions: TV standard, FM band and de-emphasis, units ----
struct Region {
    const char* name;      // N_()
    int tvFamily, tvStd;   // the family of the digital TV there (0 DVB, 1 ATSC, 4 ISDB-T), and the DVB standard (1 T2, 2 T)
    double fmMhz;          // a sensible FM broadcast frequency
    const char* tvId;      // when not null: the TV mode by id instead (China)
    int fmDeemphUs;        // FM de-emphasis: 50 or 75 microseconds
    bool imperial;         // miles and feet in distances
    double fmLoMhz, fmHiMhz;   // the FM broadcast band
    const char* bandPlan;  // the band plan (data/bandplans/builtin.json id) for the TV channels, "" to keep the automatic one (from the language)
};
inline const Region* regions(int* n) {
    static const Region k[] = {
        {N_("Europe"), 0, 1, 100.0, nullptr, 50, false, 87.5, 108.0, ""},
        {N_("United Kingdom and Ireland"), 0, 1, 97.0, nullptr, 50, true, 87.5, 108.0, "gb"},
        {N_("North America"), 1, 0, 99.5, nullptr, 75, true, 88.0, 108.0, "us"},
        {N_("South and Central America"), 4, 0, 99.5, nullptr, 75, false, 88.0, 108.0, "br"},
        {N_("Middle East"), 0, 1, 93.9, nullptr, 50, false, 87.5, 108.0, "ae"},
        {N_("Africa"), 0, 1, 98.0, nullptr, 50, false, 87.5, 108.0, "za"},
        {N_("Australia and New Zealand"), 0, 2, 101.1, nullptr, 50, false, 87.5, 108.0, "au"},
        {N_("Japan"), 4, 0, 80.0, nullptr, 50, false, 76.0, 95.0, "jp"},
        {N_("South Korea"), 1, 0, 95.1, nullptr, 75, false, 88.0, 108.0, ""},
        {N_("China"), 0, 0, 100.0, "dtmb", 50, false, 87.5, 108.0, "cn"},
        {N_("India and South Asia"), 0, 1, 98.3, nullptr, 50, false, 87.5, 108.0, "in"},
        {N_("Elsewhere"), 0, 1, 100.0, nullptr, 50, false, 87.5, 108.0, ""},
    };
    if (n) *n = (int)(sizeof k / sizeof *k);
    return k;
}
inline int regionCount() { int n = 0; regions(&n); return n; }
inline int clampRegion(long i) { return (int)std::max(0L, std::min((long)regionCount() - 1, i)); }

// ---- radios: one plain line per kind, from the board or the name the driver reports ----
inline std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
inline const char* radioBlurb(const std::string& board, const std::string& name) {
    const std::string b = lower(board + " " + name);
    struct R { const char* key; const char* text; };
    static const R k[] = {
        {"rtl_tcp", N_("rtl_tcp: an RTL-SDR shared by another computer over the network.")},
        {"rtltcp", N_("rtl_tcp: an RTL-SDR shared by another computer over the network.")},
        {"airspyhf", N_("Airspy HF+: a very sensitive receiver for long, medium and short wave and FM, up to 260 MHz.")},
        {"fobos", N_("Fobos SDR: a wideband receiver, 100 kHz to 6 GHz. Needs the radio's own library, see the ? in Radio settings.")},
        {"rfnm", N_("RFNM: a wideband radio board with one or two daughter boards, on USB or the network.")},
        {"nrspst", N_("SDRplay nRSP-ST: a network radio, used through SDRconnect running on a computer.")},
        {"tci", N_("TCI: a radio of the ExpertSDR family (SunSDR and others) on the network. Receive only.")},
        {"sdrpp", N_("SDR++ server: a radio shared by another computer running SDR++.")},
        {"audio", N_("A sound card or a web radio used as the input.")},
        {"hackrf", N_("HackRF: covers 1 MHz to 6 GHz with a wide view, and can transmit. Less sensitive than an RTL-SDR.")},
        {"rtl", N_("RTL-SDR: the cheap and popular USB stick, about 24 MHz to 1.7 GHz. Good for FM, airband, ADS-B and AIS.")},
        {"hydrasdr", N_("HydraSDR RFOne: a sensitive receive-only radio with three antenna inputs.")},
        {"airspy", N_("Airspy: a sensitive receiver with low noise, good for weak signals.")},
        {"sdrplay", N_("SDRplay RSP: covers long wave to 2 GHz, very good on short wave. Needs the SDRplay API.")},
        {"rsp", N_("SDRplay RSP: covers long wave to 2 GHz, very good on short wave. Needs the SDRplay API.")},
        {"pluto", N_("PlutoSDR: receives and transmits, 70 MHz to 6 GHz (more after the usual tweak).")},
        {"icr8600", N_("Icom IC-R8600: the I/Q OUT port of the receiver, 10 kHz to 3 GHz. Needs Icom's firmware file, see the ? in Radio settings. Receive only.")},
        {"rx888", N_("RX888: a direct-sampling receiver, excellent for long, medium and short wave. Needs USB 3.")},
        {"lime", N_("LimeSDR: a wideband transceiver, 10 MHz to 3.5 GHz.")},
        {"blade", N_("bladeRF: a wideband transceiver for advanced use.")},
        {"usrp", N_("USRP: a laboratory-grade transceiver.")},
        {"hermes", N_("Hermes Lite 2: a short-wave transceiver on the network.")},
        {"kiwi", N_("KiwiSDR: a short-wave receiver somewhere on the internet, shared by its owner.")},
        {"spyserver", N_("SpyServer: a radio shared by another computer over the network.")},
        {"hamlib", N_("A transceiver controlled over CAT, with its sound card for audio.")},
    };
    for (const R& r : k) if (b.find(r.key) != std::string::npos) return r.text;
    return N_("A software-defined radio. OnAir talks to it through its driver.");
}

// ---- lessons ----
// What a lesson waits for. The app fills an Observed each frame; the check compares it with the Observed when the lesson started.
enum class Check {
    Manual,         // the person presses "Got it"
    FreqChanged,    // the frequency moved (clicked the spectrum, scrolled the digits)
    TuneTo,         // the frequency is within tolKhz of target (target 0: the region's FM frequency)
    TuneInBand,     // the frequency is between lo and hi
    ModeChanged,    // another mode than at the start
    ModeIs,         // the mode with this id (modeId)
    GainAuto,       // automatic gain on, or the automatic gain ran
    GainChanged,    // the gain moved, or automatic gain on
    VolumeChanged,  // the volume or mute changed
    Running,        // the receiver is running
    Recording,      // an audio (or IQ) recording runs
    SquelchChanged, // the general receiver's squelch setting changed
    BandwidthChanged,   // the general receiver's filter width changed
    WindowOpen,     // a window opened (Observed::windows bit `window`)
};
enum Window { WinMap = 1, WinPasses = 2, WinSweep = 4, WinGlossary = 8 };
// A button in the lesson that does the step for the person (they still see where it is).
enum class Help { None, StartRadio, SetMode, TuneTarget, AutoGain, OpenMap, OpenPasses, OpenSweep };

struct Lesson {
    const char* id;        // stable: the saved progress
    const char* title;     // N_()
    const char* text;      // N_(): what to do; one or two short paragraphs
    const char* anchor;    // the part of the window to light up (tourMark ids), "" for none
    Check check;
    double target, tolKhz, lo, hi;   // TuneTo: target (0: the region's FM) and tolerance; TuneInBand: lo..hi in MHz
    const char* modeId;    // ModeIs, and Help::SetMode
    int window;            // WindowOpen
    Help help;
    const char* helpLabel; // N_(): the button text
    const char* done;      // N_(): said when the check passes; may hold one %.3f (the frequency in MHz)
    int goalMask;          // 0: a basic lesson for everybody; else the goals (1 << Goal) it belongs to
};
inline const Lesson* lessons(int* n) {
    constexpr int kAll = 0;
    static const Lesson k[] = {
        // the basics, in order
        {"start", N_("Start the receiver"), N_("The round button at the top left starts and stops the receiver. Nothing is received while it is stopped."),
         "freq", Check::Running, 0, 0, 0, 0, nullptr, 0, Help::StartRadio, N_("Start it for me"), N_("Well done. The receiver is running."), kAll},
        {"waterfall", N_("Read the waterfall"), N_("The top graph is the spectrum: left is lower frequency, right is higher, up is a stronger signal. Below it the waterfall shows the same over time, newest at the top.\nA signal is a bright bump or stripe that stands out from the grainy noise floor. A steady stripe is a broadcast; short dashes are bursts of data or speech."),
         "spectrum", Check::Manual, 0, 0, 0, 0, nullptr, 0, Help::None, nullptr, N_("Good. You know how to spot a signal."), kAll},
        {"click", N_("Tune by clicking"), N_("Click a bright stripe in the spectrum or the waterfall to tune to it. Scroll on the spectrum to zoom in and out."),
         "spectrum", Check::FreqChanged, 0, 0, 0, 0, nullptr, 0, Help::None, nullptr, N_("Well done. You tuned to %.3f MHz by clicking."), kAll},
        {"type", N_("Type a frequency"), N_("Click the big frequency digits and type a number, then press Enter. Scrolling over a digit steps it up or down: the digit you point at sets the step size.\nTry your local FM frequency shown below."),
         "freq", Check::TuneTo, 0, 1, 0, 0, nullptr, 0, Help::TuneTarget, N_("Tune it for me"), N_("Well done. You tuned to %.3f MHz."), kAll},
        {"gain", N_("Set the gain"), N_("Gain is how much the radio amplifies before it measures the signal. Too little and weak signals vanish in the noise; too much and strong ones distort everything.\nThe easy way: turn on automatic gain, which finds a good level for this mode."),
         "gain", Check::GainAuto, 0, 0, 0, 0, nullptr, 0, Help::AutoGain, N_("Set the gain for me"), N_("Good. Automatic gain is on."), kAll},
        {"mode", N_("Change the mode"), N_("The mode decides what OnAir decodes: FM sound, TV pictures, aircraft positions and more. Pick another one in the Mode list, or type a name in its search box."),
         "modes", Check::ModeChanged, 0, 0, 0, 0, nullptr, 0, Help::None, nullptr, N_("Well done. You changed the mode."), kAll},
        {"bandwidth", N_("Bandwidth"), N_("The General receiver listens to any frequency. Its filter width (bandwidth) decides how much around the frequency you hear: narrow cuts noise and neighbours, wide lets a broad signal through.\nPick another width in the mode panel."),
         "panel", Check::BandwidthChanged, 0, 0, 0, 0, "general", 0, Help::SetMode, N_("Open the General receiver"), N_("Good. You changed the bandwidth."), kAll},
        {"squelch", N_("Squelch"), N_("Squelch keeps the sound muted until a signal is there, so you do not listen to hiss between transmissions. Switch it on or move its level in the mode panel."),
         "panel", Check::SquelchChanged, 0, 0, 0, 0, "general", 0, Help::SetMode, N_("Open the General receiver"), N_("Good. You set the squelch."), kAll},
        {"volume", N_("Volume"), N_("The volume and mute are at the top right. Move the slider or press the speaker."),
         "audio", Check::VolumeChanged, 0, 0, 0, 0, nullptr, 0, Help::None, nullptr, N_("Good. You changed the volume."), kAll},
        {"record", N_("Recording"), N_("Record audio saves the sound you hear to a file. Record IQ (in the Source section) saves the raw radio signal, so you can decode it again later. Start an audio recording."),
         "record", Check::Recording, 0, 0, 0, 0, nullptr, 0, Help::None, nullptr, N_("Well done. A recording is running; press Stop when you have enough."), kAll},
        // per goal
        {"g_fm", N_("FM radio"), N_("FM stations are between 87.5 and 108 MHz in most places. In FM mode, tune to a strong stripe; the station name and song appear when the station sends RDS."),
         "freq", Check::TuneInBand, 0, 0, 87.5, 108.0, "fm", 0, Help::SetMode, N_("Switch to FM for me"), N_("Well done. You are on FM at %.3f MHz."), 1 << GoalListen},
        {"g_air", N_("Airband"), N_("Aircraft talk in AM between 118 and 137 MHz. The airport tower and the approach frequency are the busiest. Transmissions are short, so leave the squelch on and wait."),
         "freq", Check::TuneInBand, 0, 0, 118.0, 137.0, "airband", 0, Help::SetMode, N_("Switch to Airband for me"), N_("Well done. You are on the airband at %.3f MHz."), 1 << GoalAirband},
        {"g_adsb", N_("Aircraft on the map"), N_("ADS-B mode decodes the positions planes send on 1090 MHz. Start it, then open View > Map to see them. A short whip sees planes about 50 to 100 km away; an antenna up high sees much further."),
         "modes", Check::ModeIs, 0, 0, 0, 0, "adsb", 0, Help::SetMode, N_("Switch to ADS-B for me"), N_("Good. ADS-B is selected. Open the map to see the planes."), 1 << GoalTrack},
        {"g_map", N_("Open the map"), N_("The map shows what the decoders found: planes, ships and other positions, around your own place."),
         "view", Check::WindowOpen, 0, 0, 0, 0, nullptr, WinMap, Help::OpenMap, N_("Open the map for me"), N_("Good. The map is open."), 1 << GoalTrack},
        {"g_ais", N_("Ships (AIS)"), N_("Ships send their position on 161.975 and 162.025 MHz. Near the sea or a big river you will see them on the map within minutes."),
         "modes", Check::ModeIs, 0, 0, 0, 0, "ais", 0, Help::SetMode, N_("Switch to AIS for me"), N_("Good. AIS is selected."), 1 << GoalTrack},
        {"g_tv", N_("Find TV channels"), N_("Digital TV is in the UHF band, about 470 to 700 MHz. In the TV mode, the scan in its panel steps through the channels and lists the programmes it finds. Point the antenna towards the transmitter."),
         "panel", Check::ModeIs, 0, 0, 0, 0, "tv", 0, Help::SetMode, N_("Switch to TV for me"), N_("Good. Now run the channel scan in the panel."), 1 << GoalTv},
        {"g_ft8", N_("FT8 basics"), N_("FT8 is a ham radio mode for weak signals: short messages with call signs, signal reports and locators, every 15 seconds. On 20 metres it is at 14.074 MHz. It needs a short-wave antenna, and an upconverter for radios that start at 24 MHz."),
         "freq", Check::TuneTo, 14.074, 1, 0, 0, "ft8", 0, Help::SetMode, N_("Switch to FT8 for me"), N_("Well done. You are on the FT8 frequency, %.3f MHz."), 1 << GoalHam},
        {"g_pass", N_("Satellite passes"), N_("A satellite is only heard while it passes overhead, a few minutes at a time. The Passes window lists when the next ones come, from your location."),
         "view", Check::WindowOpen, 0, 0, 0, 0, nullptr, WinPasses, Help::OpenPasses, N_("Open Passes for me"), N_("Good. The next passes are listed."), 1 << GoalSat},
        {"g_sweep", N_("Sweep scanner"), N_("The sweep scanner steps the radio across a wide band and draws everything it finds in one picture. Strong peaks are worth a closer look."),
         "view", Check::WindowOpen, 0, 0, 0, 0, nullptr, WinSweep, Help::OpenSweep, N_("Open the sweep scanner for me"), N_("Good. The sweep scanner is open."), (1 << GoalScan) | (1 << GoalExplore)},
    };
    if (n) *n = (int)(sizeof k / sizeof *k);
    return k;
}
inline int lessonCount() { int n = 0; lessons(&n); return n; }
inline int lessonIndex(const std::string& id) { int n = 0; const Lesson* L = lessons(&n); for (int i = 0; i < n; i++) if (id == L[i].id) return i; return -1; }
// the lessons for these goals (a bit per Goal): the basics first, then the goal lessons
inline std::vector<int> lessonPlan(unsigned goalBits) {
    std::vector<int> v;
    int n = 0;
    const Lesson* L = lessons(&n);
    for (int i = 0; i < n; i++) if (!L[i].goalMask) v.push_back(i);
    for (int i = 0; i < n; i++) if (L[i].goalMask && (L[i].goalMask & (int)goalBits)) v.push_back(i);
    return v;
}

struct Observed {
    double freqMhz = 0;
    std::string modeId;    // the selected mode's id
    bool agcOn = false, autogainRan = false;
    double gainDb = 0;     // any number that changes when the gain does
    float volume = 1.f;
    bool muted = false;
    bool running = false, recording = false;
    double squelch = 0;    // any number that changes with the squelch setting
    double bandwidthHz = 0;
    int windows = 0;       // Window bits open
};

inline bool freqNear(double mhz, double targetMhz, double tolKhz) { return std::fabs(mhz - targetMhz) * 1000.0 <= tolKhz + 1e-9; }
inline double lessonTarget(const Lesson& l, double regionFmMhz) { return l.target > 0 ? l.target : regionFmMhz; }

// Whether the lesson's step is done: `now` against `start` (the state when the lesson opened).
inline bool verify(const Lesson& l, const Observed& start, const Observed& now, double regionFmMhz) {
    switch (l.check) {
    case Check::Manual: return false;
    case Check::FreqChanged: return std::fabs(now.freqMhz - start.freqMhz) > 1e-6;
    case Check::TuneTo: return freqNear(now.freqMhz, lessonTarget(l, regionFmMhz), l.tolKhz) && (!l.modeId || now.modeId == l.modeId);
    case Check::TuneInBand: return now.freqMhz >= l.lo - 1e-9 && now.freqMhz <= l.hi + 1e-9 && (!l.modeId || now.modeId == l.modeId);
    case Check::ModeChanged: return now.modeId != start.modeId;
    case Check::ModeIs: return l.modeId && now.modeId == l.modeId;
    case Check::GainAuto: return now.agcOn || now.autogainRan;
    case Check::GainChanged: return now.agcOn || now.autogainRan || std::fabs(now.gainDb - start.gainDb) > 1e-6;
    case Check::VolumeChanged: return std::fabs(now.volume - start.volume) > 1e-4f || now.muted != start.muted;
    case Check::Running: return now.running;
    case Check::Recording: return now.recording;
    case Check::SquelchChanged: return std::fabs(now.squelch - start.squelch) > 1e-6;
    case Check::BandwidthChanged: return std::fabs(now.bandwidthHz - start.bandwidthHz) > 1e-3;
    case Check::WindowOpen: return (now.windows & l.window) != 0;
    }
    return false;
}

// ---- progress, per lesson: saved as "id=1;id=2" (1 done, 2 skipped) in one preference ----
enum State { Todo = 0, Done = 1, Skipped = 2 };
using Progress = std::map<std::string, int>;
inline std::string encode(const Progress& p) {
    std::string s;
    for (const auto& kv : p) if (kv.second == Done || kv.second == Skipped) { if (!s.empty()) s += ';'; s += kv.first + '=' + char('0' + kv.second); }
    return s;
}
inline Progress decode(const std::string& s) {
    Progress p;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(';', i);
        if (j == std::string::npos) j = s.size();
        const std::string item = s.substr(i, j - i);
        const size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0 && eq + 2 == item.size() && (item[eq + 1] == '1' || item[eq + 1] == '2')) p[item.substr(0, eq)] = item[eq + 1] - '0';
        i = j + 1;
    }
    return p;
}
inline int stateOf(const Progress& p, const char* id) { auto it = p.find(id); return it == p.end() ? Todo : it->second; }
// where to resume a plan: the first lesson neither done nor skipped; -1 when all are
inline int resumeAt(const std::vector<int>& plan, const Progress& p) {
    for (int k = 0; k < (int)plan.size(); k++) if (stateOf(p, lessons(nullptr)[plan[k]].id) == Todo) return k;
    return -1;
}
inline int doneCount(const std::vector<int>& plan, const Progress& p) { int c = 0; for (int i : plan) c += stateOf(p, lessons(nullptr)[i].id) == Done; return c; }

// the setup's saved step: resumes where it was closed, but never past the last step
inline int setupResumeStep(long saved, int steps) { return (int)std::max(0L, std::min((long)steps - 1, saved)); }

// ---- checkup ----
enum Level { Ok, Tip, Warn };
struct CheckupIn {
    bool radio = false;          // a radio is selected (not the demo or a file)
    bool networkRadio = false;
    bool radioOpened = false;    // the receiver ran on it
    std::string startError;      // the last start error, empty when none
    int antenna = AntWhip;
    unsigned goals = 0;
    bool location = false;
    bool agcOn = false;
    int lessonsDone = 0, lessonsTotal = 0;
    double fmMhz = 100;
};
struct CheckItem { Level level; const char* text; };   // text is N_()
inline std::vector<CheckItem> checkup(const CheckupIn& c) {
    std::vector<CheckItem> v;
    auto has = [&](Goal g) { return (c.goals >> g) & 1u; };
    if (!c.radio) v.push_back({Tip, N_("No radio is selected. The demo signal and recordings work; plug in a radio and press Look again in Setup to receive the real air.")});
    else if (!c.startError.empty()) v.push_back({Warn, N_("The radio did not start the last time. Help > Troubleshooting and the How to fix box next to the error explain the usual causes (driver, permission, USB cable).")});
    else if (c.radioOpened) v.push_back({Ok, N_("Your radio works: the receiver ran on it.")});
    else v.push_back({Tip, N_("Your radio is found. Start the receiver once to check that it opens.")});
    if (c.networkRadio) v.push_back({Tip, N_("A network radio is limited by its link: keep the sample rate low if the sound breaks up.")});
    switch (c.antenna) {
    case AntWhip:
        v.push_back({Ok, N_("The stock whip is fine for FM and strong local stations.")});
        if (has(GoalAirband)) v.push_back({Tip, N_("For airband, put the whip near a window, pull it out to about 60 cm, or try a discone or a vertical outdoors.")});
        if (has(GoalTrack)) v.push_back({Tip, N_("For ADS-B, a 1090 MHz antenna (6.9 cm vertical) high up and near a window sees planes much further away.")});
        if (has(GoalTv)) v.push_back({Tip, N_("For TV, a roof or loft TV antenna pointed at the transmitter works far better than the whip.")});
        if (has(GoalHam)) v.push_back({Warn, N_("The whip hears almost nothing on short wave. FT8 needs a long wire or a loop, and an upconverter for radios that start at 24 MHz.")});
        if (has(GoalSat)) v.push_back({Tip, N_("For weather satellites, a V-dipole (two 53 cm rods at 120 degrees, lying flat) works well outdoors.")});
        break;
    case AntDipole:
        v.push_back({Ok, N_("A dipole works well when its rods match the band: about 75 cm each for FM, 53 cm for satellites at 137 MHz.")});
        break;
    case AntDiscone:
        v.push_back({Ok, N_("A discone covers FM, airband, AIS and ADS-B. It is a good all-round choice.")});
        if (has(GoalHam)) v.push_back({Tip, N_("A discone is weak below 25 MHz: for FT8 add a long wire or a loop.")});
        break;
    case AntLoop:
        v.push_back({Ok, N_("A loop is good for long, medium and short wave.")});
        if (has(GoalTrack) || has(GoalTv) || has(GoalAirband)) v.push_back({Tip, N_("A short-wave loop is weak above 30 MHz: for VHF and UHF use a whip, a dipole or a discone.")});
        break;
    case AntLong:
        v.push_back({Ok, N_("A long wire is good for short wave and FT8.")});
        if (has(GoalTrack) || has(GoalTv) || has(GoalAirband)) v.push_back({Tip, N_("For VHF and UHF a long wire is a poor match: use a whip, a dipole or a discone.")});
        break;
    default:
        v.push_back({Warn, N_("Without an antenna only very strong signals come through. Even a short piece of wire in the antenna socket helps.")});
        break;
    }
    if (!c.agcOn) v.push_back({Tip, N_("Automatic gain is off. If signals look weak or the spectrum is full of copies, turn it on (the AGC switch in Gain).")});
    if (!c.location && (has(GoalTrack) || has(GoalSat))) v.push_back({Tip, N_("Add your location in Setup: the map centres on it and satellite passes need it. It stays on this computer.")});
    if (c.lessonsTotal > 0) {
        if (c.lessonsDone >= c.lessonsTotal) v.push_back({Ok, N_("All your lessons are done.")});
        else v.push_back({Tip, N_("Some lessons are still open: Help > Setup and lessons > Lessons continues where you stopped.")});
    }
    return v;
}

// ---- glossary ----
enum class Pic { None, Snr, Gain, SampleRate, Bandwidth, Offset, Ppm, Squelch, Modulation, Decoder, Waterfall };
struct Term { const char* word; const char* text; Pic pic; };   // N_()
inline const Term* glossary(int* n) {
    static const Term k[] = {
        {N_("Signal and noise floor"), N_("The noise floor is the grainy hiss everywhere in the spectrum. A signal is anything that stands out above it."), Pic::Waterfall},
        {N_("SNR (signal-to-noise ratio)"), N_("How far a signal stands above the noise floor, in decibels (dB). Around 10 dB is usable; 20 dB or more is clean."), Pic::Snr},
        {N_("Gain"), N_("How much the radio amplifies before it measures. Too low: weak signals disappear. Too high: strong signals overload the radio and create false copies."), Pic::Gain},
        {N_("Sample rate"), N_("How many measurements per second the radio sends. It sets how wide a piece of the spectrum you see at once: 2 million samples a second shows about 2 MHz."), Pic::SampleRate},
        {N_("Bandwidth"), N_("How wide a slice around the frequency is used. A filter narrower than the signal loses it; much wider lets in noise and neighbours."), Pic::Bandwidth},
        {N_("Offset"), N_("How far the wanted signal is from the radio's centre frequency. OnAir often tunes a little off centre to avoid the spike at the centre."), Pic::Offset},
        {N_("ppm (frequency error)"), N_("Parts per million: how far the radio's clock is off. 1 ppm at 100 MHz is 100 Hz. Cheap radios can be off by 20 to 50 ppm; a TCXO radio by 1 or less."), Pic::Ppm},
        {N_("Squelch"), N_("A gate that keeps the sound muted until a signal is stronger than its level, so you do not hear hiss between transmissions."), Pic::Squelch},
        {N_("Modulation"), N_("How information rides on the radio wave: AM changes its strength, FM changes its frequency, digital modes switch between set states."), Pic::Modulation},
        {N_("Decoder"), N_("The part of OnAir that turns a received signal back into sound, pictures, text or positions. Each mode has its own."), Pic::Decoder},
        {N_("Spectrum and waterfall"), N_("The spectrum is a live graph of signal strength across frequency. The waterfall stacks those graphs over time, so steady and short signals look different."), Pic::Waterfall},
        {N_("dB (decibel)"), N_("A ratio on a log scale: +3 dB is double the power, +10 dB is ten times, +20 dB a hundred times."), Pic::None},
        {N_("IQ recording"), N_("A file of the raw radio samples. Played back, it behaves like the radio at that moment, so any mode can decode it again."), Pic::None},
        {N_("Antenna"), N_("Catches the radio waves. It matters more than the radio: a good antenna in the right place beats an expensive receiver."), Pic::None},
        {N_("Mode"), N_("In OnAir, a mode is one kind of signal with its decoder: FM, DVB-T, ADS-B, FT8 and so on."), Pic::None},
        {N_("Driver"), N_("The software that lets the computer talk to the radio. On Windows it is usually WinUSB, installed with Zadig; on Linux, udev rules give your account access."), Pic::None},
    };
    if (n) *n = (int)(sizeof k / sizeof *k);
    return k;
}
// case-insensitive search in the word and the explanation (given already translated); an empty query matches everything
inline bool termMatches(const std::string& word, const std::string& text, const std::string& query) {
    if (query.empty()) return true;
    const std::string q = lower(query);
    return lower(word).find(q) != std::string::npos || lower(text).find(q) != std::string::npos;
}

// preference keys
inline const char* prefSetupStep() { return "setupStep"; }
inline const char* prefGoal() { return "setupGoal"; }
inline const char* prefAntenna() { return "setupAntenna"; }
inline const char* prefProgress() { return "lessonProgress"; }

}  // namespace learn

#ifdef LEARN_UNDEF_N_
#undef N_
#undef LEARN_UNDEF_N_
#endif
