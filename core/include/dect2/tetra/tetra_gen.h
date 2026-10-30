// TETRA test signal: base station carriers sending a continuous downlink as a real cell does: the synchronisation burst (BSCH) and
// the SYSINFO broadcast (BNCH) in frame 18, the main control channel on timeslot 1 with the neighbour cell broadcast (D-NWRK-BROADCAST)
// and, on the cell that uses encryption, encrypted signalling (random bits), a traffic timeslot of random bits, and idle null PDUs
// elsewhere; every slot with its access assignment (AACH). It uses the test network code MCC 1, MNC 1, not a real network.
// Nothing is transmitted: the samples only exist in memory.
//
// SynthConfig (makeTetraSynth):
//   snrDb       signal to noise inside the 25 kHz channel of the strongest carrier
//   cfoHz       the radio's tuning error
//   sroPpm      the base station's symbol clock relative to the radio's sample clock
//   modeOpt[0]  scene: 0 one cell (392.4125 MHz, encrypted network), 1 two cells (392.4125 and 392.6000 MHz, the second in the clear),
//               2 a wide capture (3.4): modeOpt[1] cells (default 12) spread over the middle 72 % of the sample rate on the 25 kHz raster, each with its
//               (the default scene 0 becomes this one when the capture is 4 Msps or more), each with its
//               own colour code, location area and base-station tuning error (up to +-4 kHz), at levels up to 12 dB apart; every second one encrypted
// The carriers sit at their frequencies relative to a radio centred on 392.5 MHz (the mode's default).
#pragma once
#include "dect2/engine/mode_synth.h"
#include "tetra_pdu.h"
#include <memory>
#include <vector>

namespace dect2 {

struct SynthConfig;

struct TetraCellCfg {
    tetra::Sync sync;              // MCC, MNC, colour code, sharing mode (tn / fn / mn are set per burst)
    tetra::Sysinfo si;             // main carrier, band, location area, services
    tetra::NwrkBroadcast nwrk;     // neighbours, network time
    bool encryptedSignalling = false;   // MCCH PDUs sent with encryption mode 1 (random content)
    int trafficTs = 2;             // a timeslot marked as traffic (random bits), 0 none
    float amp = 0.25f;
    uint32_t startSlot = 0;        // the slot counter (multiframe, frame, timeslot) at the start of the signal
};

constexpr double kTetraTestCentreHz = 392.5e6;

int tetraTestCells(int scene);
TetraCellCfg tetraTestCell(int scene, int index);

// Scene 2: cell `index` of `count` for a capture of `rateHz`: its carrier offset from the centre (a multiple of 25 kHz), its own tuning error and its
// configuration (colour code 1 + index; main carrier and frequency match). The offsets are distinct and sorted by index.
struct TetraWideCell { TetraCellCfg cfg; double offHz = 0, errHz = 0; };
TetraWideCell tetraWideCell(int index, int count, double rateHz);

namespace tetra {
// The 510 bits of one downlink slot of this cell; abs counts slots from multiframe 1, frame 1, timeslot 1
Bits cellSlot(const TetraCellCfg& c, uint32_t abs);
}

std::unique_ptr<ModeSynth> makeTetraSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
