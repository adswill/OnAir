// Wide multi-signal decode: ONE wide capture, several different signals decoded at once and found automatically.
//
//   span -> TV: SpanScanner (which 7 / 8 MHz slots hold a signal) -> detectTv on each (DVB-T or DVB-T2) -> MultiTv per standard, one receiver per slot
//        -> narrow: CarrierFinder (the TV zones left out) -> per new carrier ~1.2 s at baseband -> classifyVoice (DMR / P25 / NXDN / dPMR / D-STAR /
//           Fusion / M17 / TETRA / analogue FM), else the known channel list (pagers, packet, ...), else the air band (AM)
//        -> the decoder of what it is: WideModes bank of that mode (follow list = just the carriers identified as that mode), or VfoBank (AM / NFM)
//
// This does not replace anything: it puts the existing pieces (CarrierFinder, WideModes banks, MultiTv, VfoBank, autodet) behind one identifier.
// WideModes alone decodes ONE mode over the span; MultiTv alone ONE TV standard; the Multi-VFO bank needs the user to place every channel.
//
// The list (entries()): every signal found with what it is, its state and its cost. A user can switch an entry off and on, pin it (never dropped),
// raise or lower its priority, or give it a kind by hand (an unidentified carrier). CPU budget guard: the work of all decoders is measured against
// real time; above the budget the lowest ranked entry that is not pinned or listened to is switched off ("dropped for CPU", shown with the reason),
// and it comes back when the load has stayed well under the budget. An entry is dropped or restored only between blocks (a whole block is fed or
// not), and the sound path (VfoBank mix) of the entries that stay is not touched, so a drop never cuts the audio of the ones that remain.
// Nothing is refused: a decoder that cannot run (no decoder for that system in this build, a standard the multi-channel TV cannot take) is listed
// as "identified, no decoder here".
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/vfo.h"
#include "dect2/engine/wide_modes.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

class MultiTv;
struct TvChannelRow;

enum class WsKind : int {
    Unknown = 0,        // not identified (yet)
    Dmr, P25, Nxdn, Dpmr, Dstar, Fusion, M17,   // digital voice: the WideModes banks
    Pager, Packet,      // known channels: the WideModes banks
    Am, Nfm,            // analogue voice: the Multi-VFO bank (air band: AM)
    DvbT, DvbT2,        // TV: MultiTv
    NoDecoder,          // identified, but nothing here decodes it (TETRA, ISDB-T, ATSC ...); identity says what it is
    Count
};
const char* wsKindName(WsKind k);
inline bool wsIsTv(WsKind k) { return k == WsKind::DvbT || k == WsKind::DvbT2; }
int wsKindStdMode(WsKind k);   // the WideModes mode of a bank kind (10 DMR, 51 P25 ...), 0 for the others

enum class WsState : int {
    Identifying = 0,    // watching the signal to find out what it is
    Running,            // a decoder works on it
    Off,                // switched off by the user
    Dropped,            // switched off by the CPU guard
    NoDecoder,          // identified; nothing decodes it
    Unidentified        // could not be identified (it can be given a kind by hand)
};
const char* wsStateName(WsState s);

struct WsEntry {
    int id = 0;
    WsKind kind = WsKind::Unknown;
    std::string identity;        // what the identifier said: "DMR (sure)", "DVB-T2, 32K, GI 1/8, 8 MHz", "TETRA", "known channel: pocsag"
    WsState state = WsState::Identifying;
    double offsetHz = 0;         // from the capture centre
    double absHz = 0;            // offsetHz + the centre (0 centre: the offset)
    double bandwidthHz = 0;
    double levelDb = -200;
    double snrDb = 0;
    bool active = false;         // on the air now
    bool locked = false;         // the decoder is locked (TV: transport stream flowing)
    bool enabled = true;         // the user's switch
    bool pinned = false;         // never dropped by the CPU guard
    int priority = 0;            // the user's adjustment (added to the automatic rank)
    double rank = 0;             // what the CPU guard compares (higher stays)
    double cost = 0;             // share of one core, measured (a dropped entry: the last measurement)
    uint64_t items = 0;          // calls / pages / frames / services so far
    std::string info;            // the decoder's one-line state
    std::string why;             // the reason for Off / Dropped / NoDecoder / Unidentified
};

struct WsStatus {
    double load = 0;             // all decoders' work as a share of real time (analysis thread + TV receiver threads), 1 = one core
    double budget = 0;           // the limit (0: none)
    bool tooSlow = false;        // the guard has dropped something that is not back yet
    int droppedForCpu = 0;       // entries dropped for the CPU now
    uint64_t dropEvents = 0;     // drops so far
    int running = 0;
    double noiseDb = -200;
    bool tvPresent = false;
    double tvWidthMhz = 0;
};

struct WsConfig {
    double spanRate = 0;
    double centerHz = 0;         // absolute frequency of the capture centre (0 unknown: offsets only; the air band and the known channels then cannot be used)
    bool narrow = true;          // narrowband carriers
    bool tv = true;              // wide TV channels (only when the span can hold one)
    std::vector<double> tvWidthsMhz = {8, 7};   // channel widths tried
    double cpuBudget = 0.8;      // 0: none
    int maxNarrow = 24;          // carriers followed at most
    double identSec = 1.2;       // how long a carrier is watched before it is classified
    int identAttempts = 3;
    bool blockingTv = false;     // the TV receivers wait for each other (files, tests); false: a slow one loses blocks (a live radio)
    bool airband = true;         // an unidentified or analogue narrow carrier between 118 and 137 MHz is AM
};

// A channel the identifier cannot see in the signal (a pager or packet channel looks like nothing in particular) but the user or the frequency
// database knows: carriers on it (within 7 kHz) get this kind when the measurement does not say better.
struct WsKnown { double absHz = 0; WsKind kind = WsKind::Unknown; };

class WideSession {
public:
    WideSession();
    ~WideSession();
    WideSession(const WideSession&) = delete;
    WideSession& operator=(const WideSession&) = delete;

    void configure(const WsConfig& c);
    void reset();                                   // the carriers are forgotten (results stay)
    void setCenterHz(double hz);
    void feed(const cf32* x, size_t n);             // the analysis thread
    void wait();                                    // tests / end of a file: the identifier and the TV receivers have taken what they were given

    std::vector<WsEntry> entries() const;           // strongest rank first
    WsStatus status() const;

    // ---- control (any thread)
    void setEnabled(int id, bool on);
    void setPinned(int id, bool on);
    void setPriority(int id, int adj);
    void setKind(int id, WsKind k);                 // by hand: for an unidentified carrier, or to overrule the identifier (WsKind::Unknown: back to automatic)
    void setListen(int id);                         // the entry whose sound / detail the user follows: never dropped (0: none)
    void setBudget(double frac);                    // 0: no limit
    void setLoadOverride(double v);                 // test hook: the load the guard sees (< 0: measure)
    void setKnownChannels(const std::vector<WsKnown>& k);
    void useFrequencyDatabase(bool on);             // the built-in frequency database as known channels (default on)

    // ---- results
    WideModes& modes();                             // the banks: telemetry(stdMode, ...) gives the calls / pages / frames of every carrier of that mode
    MultiTv* tv(bool dvbt);                         // the TV receivers (nullptr until a channel of that standard was found)
    void setAudioSink(VfoBank::Sink s);             // the mixed sound of the AM / NFM entries (48 kHz stereo)
    bool vfoStatusOf(int id, VfoStatus& s) const;

    // test hook: how many identifications were started so far
    int identCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
