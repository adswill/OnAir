// DRM (ETSI ES 201 980): what the standard fixes - OFDM parameters, the position and phase of every pilot, FAC and SDC cell, the cell counts
// of the channels and the code tables. Shared by the receiver and the test signal generator.
// Clause numbers refer to V4.3.1 (2023-11). Robustness modes A to D are DRM30 (below 30 MHz), mode E is DRM+ (VHF).
#pragma once
#include "ring.h"
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace dect2 { namespace drm {

enum { kModeA = 0, kModeB, kModeC, kModeD, kModeE };

struct ModeParams {
    char name;                  // 'A' .. 'E'
    int tu12, tg12;             // Tu and Tg in units of T = 83 1/3 us (the sample period at 12 kHz), Table 2
    int ns;                     // OFDM symbols per transmission frame, Table 47
    int frames;                 // transmission frames per transmission super frame (3, mode E: 4)
    int gx, gy, gk0;            // gain reference pattern: carriers k0 + x*n + x*y*p in the symbols with s mod y = n (Tables 58 and 60)
    int sdcSymbols;             // symbols at the start of the super frame that carry the SDC (clause 8.5.3.1)
    int depth;                  // time interleaving depth D of the long interleaver, in multiplex frames (clause 7.6)
    double spacingHz() const { return 12000.0 / tu12; }
    double symbolMs() const { return (tu12 + tg12) / 12.0; }
    double frameMs() const { return ns * symbolMs(); }
};
const ModeParams& modeParams(int mode);

// Spectrum occupancy (FAC field, Table 48): carriers kmin..kmax relative to the reference frequency (Table 49). false when the mode has no such occupancy.
bool occupancyValid(int mode, int occ);
void carrierRange(int mode, int occ, int& kmin, int& kmax);
double occupancyKhz(int mode, int occ);                 // nominal channel width (4.5, 5, 9, 10, 18, 20; mode E: 100)
bool carrierUnused(int mode, int k);                    // Table 50: carriers around DC that are never used

enum CellType : uint8_t { kCellNone = 0, kCellFreqRef, kCellTimeRef, kCellGainRef, kCellAfsRef, kCellFac, kCellSdc, kCellMsc };

struct Pilot { int16_t k; uint8_t kind; cf32 ref; };    // the reference value includes the amplitude (sqrt 2, 2 for the boosted edge cells, 1 for AFS cells)

// The cell structure of one transmission super frame for a robustness mode and occupancy. Symbols are numbered 0 .. frames*ns-1 from the start of the
// super frame (the SDC is in the first symbols). Cells of the FAC, the SDC and the MSC are listed in the order the data uses them: by symbol, then by
// increasing carrier (clauses 8.5.2.2, 8.5.3.2, 8.6.2).
struct Layout {
    int mode = 0, occ = 0, kmin = 0, kmax = 0, ns = 0, frames = 0;
    int nFac = 0;                                       // FAC cells per transmission frame (65, mode E: 244)
    int nSdc = 0;                                       // SDC cells per super frame (Table 25)
    int nSfa = 0;                                       // MSC cells per super frame (Tables 41 to 45)
    int nMux = 0;                                       // MSC cells per multiplex frame: nSfa / frames, rounded down
    int width() const { return kmax - kmin + 1; }
    int symbols() const { return ns * frames; }
    std::vector<uint8_t> type;                          // [sym * width + (k - kmin)] -> CellType
    std::vector<std::vector<Pilot>> pilots;             // per symbol of the super frame
    std::vector<std::pair<int16_t, int16_t>> facCells;  // (symbol of the transmission frame, carrier)
    std::vector<std::pair<int16_t, int16_t>> sdcCells;  // (symbol of the super frame, carrier)
    std::vector<std::pair<int16_t, int16_t>> mscCells;  // (symbol of the super frame, carrier): nSfa of them; the last nSfa - frames*nMux carry dummy cells
    CellType cellType(int sym, int k) const { return (CellType)type[(size_t)sym * width() + (k - kmin)]; }
};
std::shared_ptr<const Layout> layout(int mode, int occ);   // cached; nullptr for an occupancy the mode does not have

// Reference cell of the transmission frame: carrier k in symbol s of any frame (s = 0 .. ns-1), or nothing. Pilot positions repeat every frame.
// frame0 is true for the first frame of a mode E super frame (the AFS cells of symbol 4 belong to it) and frame3 for the last (symbol 39).
bool pilotRef(int mode, int occ, int s, int k, cf32& ref, int* kind = nullptr);

// ---- channel coding parameters (clause 7)
struct Rate { int rx, ry; };
Rate mscRate(int mode, int qamBits, int protLevel, int level);    // code rate of a level: qamBits 2 (4-QAM), 4 (16-QAM), 6 (64-QAM) per cell; level 0 .. bits/2 - 1
int mscRyLcm(int mode, int qamBits, int protLevel);                // RYlcm of Tables 30 to 32 (mode A..D 16-QAM and 64-QAM, mode E 16-QAM); the denominator itself for 4-QAM
int mscProtLevels(int mode, int qamBits);                          // number of protection levels the tables define (0 when the combination does not exist)

// SDC (Tables 21, 25 and 36 to 38): data field length in bytes for an SDC mode (0: the larger constellation), or 0 when undefined
int sdcDataBytes(int mode, int occ, int sdcMode);
int sdcQamBits(int mode, int sdcMode);                             // 4 (16-QAM) or 2 (4-QAM) bits per cell
int sdcLevels(int mode, int sdcMode);

}} // namespace dect2::drm
