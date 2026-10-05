// ATSC 3.0 OFDM layer (A/322 sections 7.2.5, 7.3 and 8): the Preamble symbols that carry L1-Basic and L1-Detail. Pilots, frequency
// interleaving, IFFT with guard interval, and the way back: FFT, channel estimate from the pilots, equalisation, de-interleaving.
#pragma once
#include "ring.h"
#include <cstdint>
#include <vector>

namespace dect2 {
namespace atsc3 {

// What the bootstrap's preamble_structure value (A/322 Table H.1.1) says about the Preamble.
struct PreambleParams {
    int fftSize = 8192;      // 8192, 16384 or 32768
    int guard = 192;         // guard interval in samples
    int dx = 16;             // spacing of the Preamble pilots
    int l1BasicMode = 1;     // 1..5
};
bool preambleParams(int preambleStructure, PreambleParams& out);

int maxCarriers(int fftSize);                       // NoC for no carrier reduction: 6913, 13825 or 27649
int numCarriers(int fftSize, int reducedCarriers);  // NoC with Cred_coeff 0..4 (A/322 Table 7.1)
int preamblePilotCount(const PreambleParams& p, int cred);

// Relative carrier indices of the common continual pilots for this FFT size and carrier reduction (sorted).
std::vector<int> continualPilots(int fftSize, int cred);

// Reference sequence r_k (A/322 8.1.2), restarted at the beginning of every symbol.
std::vector<uint8_t> referenceSequence(int count);

// Pilot amplitude of the Preamble (Table 8.6) and of the continual pilots (Table 8.5).
double preamblePilotAmplitude(const PreambleParams& p);
double continualPilotAmplitude();

// Cells of a Preamble symbol that carry data (not pilots), in order of increasing carrier number. Equals A/322 Table 7.2.
std::vector<int> preambleDataCarriers(const PreambleParams& p, int cred);

// Frequency interleaver of A/322 7.3: the sequence H_l(p) for a symbol with `nData` data cells; l is the symbol number counted from the first Preamble symbol.
std::vector<int> frequencyInterleaverSequence(int fftSize, int nData, int symbolIndex);

// One Preamble symbol in the time domain: `cells` (unit-power constellation points, preamble data cells before interleaving) are
// interleaved, pilots are added, and the symbol is made with IFFT and guard interval: guard + fftSize samples of unit mean power.
std::vector<cf32> modulatePreambleSymbol(const PreambleParams& p, int cred, int symbolIndex, const std::vector<cf32>& cells);

// The reverse: `x` points at the first sample of the guard interval (guard + fftSize samples). Returns the equalised data cells in
// their original order; `noiseVar` is an estimate of the noise power per cell relative to unit cell power.
bool demodulatePreambleSymbol(const PreambleParams& p, int cred, int symbolIndex, const cf32* x, std::vector<cf32>& cells, float* noiseVar = nullptr);

} // namespace atsc3
} // namespace dect2
