// DRM: table types shared between drm_tables.cpp (generated data) and drm_defs.cpp. Not part of the public interface.
#pragma once

namespace dect2 { namespace drm {

struct RefTable {            // list of (carrier, phase) pairs or (symbol, carrier) pairs
    const int (*data)[2];
    int n;
};

struct GainTables {          // clause 8.4.4.3 (phases in 1/1024 cycle)
    const int* w;            // modes A to D: W1024[n][m]; mode E: R1024[n][m]
    const int* z;            // modes A to D: Z256[n][m];  mode E: Z1024[n][m]
    const int* qm;           // mode E: Q1024[n][m]; otherwise nullptr
    int q;                   // modes A to D: Q1024
    int rows, cols;          // n = 0..rows-1 (= y), m = 0..cols-1
};

struct PunctPattern {        // Table 27
    int rx, ry, cols;
    int row[6][8];           // B0..B5, one entry per column
};

extern const RefTable kTimeRefs[5];
extern const RefTable kFreqRefs[5];
extern const GainTables kGainTables[5];
extern const RefTable kFacCells[5];
extern const int kAfsRefs[54][3];
extern const PunctPattern kPunct[15];
extern const int kTailPunct[12][6][6];

}} // namespace dect2::drm
