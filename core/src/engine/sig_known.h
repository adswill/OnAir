// The table of modes the signal identifier knows (sig_ident.h): one row per mode the app can decode, what its signal looks like.
// Adding a mode: one row in sig_known.cpp (docs/IDENTIFIER.md), then run test_specx_idmatrix and look at its line in the table. A mode without a
// hand-written row gets one measured on its own test signal (sig_known_gen.cpp, written by test_specx_idmatrix --signatures) or, without a test
// signal, one made from the mode list (signatures() below): every mode the app has can be a candidate.
#pragma once
#include "dect2/engine/sig_ident.h"
#include <vector>

namespace dect2 {
namespace sigid {

enum SyncKind { SyncNone = 0, SyncDmr, SyncP25, SyncNxdn, SyncDpmr, SyncPocsag, SyncAis, SyncRos, SyncSsb, SyncCount };   // SyncRos: the ROS tone-grid check (dect2/ros/ros_detect.h), SyncSsb: the speech-envelope check of single sideband voice (sig_ident.cpp), neither is a frame sync

constexpr unsigned MB(int m) { return 1u << m; }   // a modulation class in a mask

struct Known {
    const char* name; const char* modeId; const char* subId; const char* family;   // family: the group the accuracy table is kept by
    unsigned mods;                 // the classes the signal may show as (MB(ModFsk4) | ...)
    int mod;                       // the one it should show as (it scores more)
    double rates[20];               // symbol rates; OFDM: useful symbol lengths in seconds; 0 ends the list
    double bw[6];                  // occupied widths (99 %), Hz, the usual ones; 0 ends the list; for AM / FM / carriers an upper limit
    double devOuter;               // FSK outer deviation, Hz (0 not FSK)
    int duty;                      // 0 either, 1 continuous, 2 bursty
    double burstMs;                // length of a burst when the protocol fixes it (0 free)
    double periodMs;               // spacing of the bursts when the protocol fixes it (0 free)
    int sync;
    float bands[10];               // MHz (lo, hi) pairs, 0 = end; none = anywhere
    const char* note;
    // optional (0 = not used by the rule)
    int tones = 0;                 // MFSK: the number of tones
    double toneHz = 0;             // MFSK / multi-carrier: the tone or carrier spacing, Hz
    double defMhz = 0;             // the mode's usual frequency (rows without bands: from the mode list)
    int source = 0;                // 0 hand-written, 1 measured on the test signal, 2 the mode list only (SigInfo::source)
};
extern const Known kKnown[];
extern const int kKnownCount;
extern const Known kKnownMeasured[];   // sig_known_gen.cpp
extern const int kKnownMeasuredCount;

// The whole table: the hand-written rows, the measured ones for the modes without a hand-written row, and a row from the mode list for every
// mode and sub-mode that has neither (built once, then constant)
const std::vector<Known>& signatures();

} // namespace sigid
} // namespace dect2
