// The Smart CubeSat decoder's satellite knowledge: the transmitter table (frequency, modulation, baud, framing hint; a snapshot of SatNOGS DB,
// CC-BY-SA 4.0, credit SatNOGS DB contributors) and a TLE snapshot (CelesTrak, T.S. Kelso), plus the pass planner built on the SGP4 code of
// geo/passes.h. The snapshot is a starting point: the app refreshes the TLEs from CelesTrak and the user can add transmitters.
#pragma once
#include "dect2/geo/passes.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {
namespace cubesat {

enum ModKind { ModUnknown = 0, ModCw, ModAfsk, ModFsk, ModBpsk, ModLora };
enum FramingHint { HintAuto = 0, HintAx25, HintAx100M5, HintG3ruh, HintUsp, HintTubix, HintDoka };

struct TxEntry {
    int norad = 0;
    std::string name;
    double hz = 0;                 // downlink centre frequency
    int mod = ModUnknown;          // ModKind
    int hint = HintAuto;           // FramingHint
    int baud = 0;                  // 0: unknown (CW: the keying speed is not a baud)
    std::string desc, mode;        // SatNOGS description and mode text
};
struct TxRow { int norad; const char* name; long long hz; int mod, hint, baud; const char* desc; const char* mode; };

const char* modName(int mod);
const char* hintName(int hint);
// Whether a decoder for the entry exists (TUBiX10 and DOKA framings are listed, tracked, but not decoded; USP is decoded since 3.9)
bool decodable(const TxEntry& t);

const std::vector<TxEntry>& txTable();                       // the built-in snapshot
std::vector<TxEntry> txFromText(const std::string& s);       // the user's additions: "norad|name|MHz|mod|hint|baud;..."
std::string txToText(const std::vector<TxEntry>& v);
std::string tleSnapshotText();                               // the built-in TLEs as a CelesTrak text file

struct PlannedPass {
    int norad = 0;
    std::string name;
    Pass p;
    std::vector<size_t> tx;        // indices into the planner's transmitter list
    double epochAgeDays = 0;       // TLE age at the time of the pass
};

class PassPlanner {
public:
    PassPlanner();
    void setObserver(const Observer& o);
    // Merges a CelesTrak text file over the snapshot (newer epochs win); returns the number of satellites set
    size_t addTleText(const std::string& text);
    void setExtraTx(const std::vector<TxEntry>& v);          // the user's transmitters, after the built-in ones
    const std::vector<TxEntry>& transmitters() const { return tx_; }
    // The next passes of satellites that have a decodable transmitter, soonest first, those above minElDeg within `hours`; a pass under way
    // starts now. max: 0 = all
    std::vector<PlannedPass> upcoming(double nowSec, double hours, double minElDeg, size_t max = 0) const;
    bool look(int norad, double unixSec, Look& l) const;
    // Doppler shift (Hz, add to the nominal downlink) of a satellite at a time, false when it has no TLE
    bool doppler(int norad, double unixSec, double freqHz, double& shiftHz) const;
    bool hasSatellite(int norad) const { return sgp_.count(norad) != 0; }
    size_t satelliteCount() const { return sgp_.size(); }
    double newestEpochJd() const;
    std::string nameOf(int norad) const;
private:
    Observer obs_;
    std::vector<TxEntry> tx_, extra_;
    std::map<int, Sgp4> sgp_;
    std::map<int, Tle> tle_;
    void rebuildTx();
};

} // namespace cubesat
} // namespace dect2
