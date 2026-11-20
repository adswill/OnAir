// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Which DVB-T2 configurations the standard allows, for the T2-Base profile and the T2-Lite profile (ETSI EN 302 755 V1.4.1, annex I).
// Used where a configuration is chosen: the transmit window (tx_params.cpp) and the video generator's mode selector (dvb_modes.cpp).
// The generator and the receiver do not use it: they make and read whatever they are given.
//
// The numbering is that of the L1 signalling, as in TxParams (t2gen.h): giIdx 0..6 = 1/32 1/16 1/8 1/4 1/128 19/128 19/256 (table 29),
// pilot pattern bit p = PP(p+1), PLP_MOD 0..3 = QPSK 16-QAM 64-QAM 256-QAM, PLP_COD 0..7 = 1/2 3/5 2/3 3/4 4/5 5/6 1/3 2/5 (table 32).
#pragma once

namespace dect2 {

// The pilot patterns allowed with this FFT size (1024 .. 32768 points) and guard interval, as a mask (bit 0 = PP1); 0: the combination
// is not allowed. Base: table 58 (SISO), table 59 (MISO); Lite: table I.5 (SISO), table I.6 (MISO). Extended carrier mode allows the same.
int t2PilotMask(int fftN, int giIdx, bool miso, bool lite);
// The table t2PilotMask() takes its answer from: "table 58", "table 59", "table I.5" or "table I.6"
const char* t2PilotTable(bool miso, bool lite);

// T2-Lite: FFT sizes 2K, 4K, 8K, 16K only (I.7.1)
bool t2LiteFft(int fftN);
// The data code rates of a profile: Base 1/2 .. 5/6 (table 7), Lite 1/3, 2/5, 1/2 .. 3/4 (I.3, table I.1)
bool t2CodeRateAllowed(int cod, bool lite);
// T2-Lite: 256-QAM not with code rates 2/3 or 3/4 (I.5, table I.4); Base allows every pair
bool t2ModCodAllowed(int mod, int cod, bool lite);
// T2-Lite: no rotated constellation with 256-QAM (I.5, table I.4)
bool t2RotationAllowed(int mod, bool lite);

// T2-Lite: a data PLP and its common PLP carry at most 4 Mbit/s together, and a transport stream input is at most 4 Mbit/s (I.10)
constexpr double kT2LiteMaxPlpBps = 4e6;
// The time-interleaver memory in cells, M_TI (cl. 6.5.2; I.8 for Lite)
constexpr long kT2TiCellsBase = (1L << 19) + (1L << 15);
constexpr long kT2TiCellsLite = 1L << 18;
// A T2-frame lasts at most 250 ms (cl. 8.3.1)
constexpr double kT2MaxFrameSec = 0.25;

// T2-Lite, one PLP: the most FEC blocks a T2-frame may carry. The 4 Mbit/s of I.10 with N_BLOCKS_IF_MAX blocks each frame of frameSec
// (P_I = I_JUMP = 1, no FEF), and with tiBlocks TI-blocks a frame the 2^18 cells of I.8: N_FEC_TI_MAX x N_CELLS <= M_TI (cl. 6.5.2).
// tiBlocks 0: no time interleaving (TIME_IL_LENGTH = 0), so no memory limit.
int t2LiteMaxBlocks(int kBch, int cellsPerBlock, double frameSec, int tiBlocks);

} // namespace dect2
