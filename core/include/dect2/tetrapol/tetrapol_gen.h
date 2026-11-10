// TETRAPOL test signal (tetrapol_rx.h): one GMSK 8 kbit/s carrier of PAS 0001-2 frames. Nothing is transmitted; the samples only exist
// in memory or in a file.
//
// SynthConfig (makeTetrapolSynth):
//   snrDb, cfoHz, sroPpm
//   modeOpt[0] scrambling constant 1 .. 127 (default 5)
//   modeOpt[1] 0 control channel: a 200-frame superframe (PAS 0001-3-3 5.1.3) with D_SYSTEM_INFO on the BCH (frames 0..3, 100..103),
//              the PCH (98, 99, 198, 199) paging the clear group, RCH acknowledgements (frames 14, 39 ...), and on the SDCH a clear and an
//              encrypted D_GROUP_ACTIVATION, D_NEIGHBOURING_CELL, then fill frames;
//              1 traffic channel with a clear call,
//              2 traffic channel with an encrypted call, 3 traffic channel with a call whose D_CALL_START has no key reference
//              (a call every 4 s: D_CALL_START x3, UI_CD TP_ADDRESS (the talking party) and D_START_SPEECH on the VCH, 3 s of voice frames
//              with the knownVoiceBits pattern, D_CALL_END x2, D_CHANNEL_FREE, fill frames)
//   The encrypted call of the control channel scene is announced for D scramble = scramble + 1 (a traffic channel scene with that
//   scrambling constant is that call), the clear one for D scramble = scramble.
//   modeOpt[2] 0 normal polarity, 1 inverted (as the uplink is to the downlink)
//   modeOpt[3] 0 UHF frame coding, 1 VHF
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

constexpr double kTetrapolTestCentreHz = 392e6;
constexpr int kTetrapolTestCountry = 33;
constexpr int kTetrapolTestSystem = 7;
constexpr int kTetrapolTestBn = 2;
constexpr int kTetrapolTestCell = 18;      // BS id
constexpr int kTetrapolTestRsw = 3;
constexpr int kTetrapolTestLa = 101;
constexpr int kTetrapolTestScramble = 5;
constexpr int kTetrapolTestGroup = 1234;   // the clear group call of the control channel scene
constexpr int kTetrapolTestChannel = 40;
constexpr int kTetrapolTestKeyClear = 0x00;   // key type 0 (RNK), key index 0: a clear call (PAS 0001-3-2 5.3.41)
constexpr int kTetrapolTestKeyCiph = 0x13;    // key index 3: encrypted
constexpr int kTetrapolTestVoiceFrames = 150; // voice frames of one test call (3 s)
constexpr int kTetrapolTestNeighbourCh = 52;  // D_NEIGHBOURING_CELL: channel 52, BN 2, cell 19 / RSW 3 (and channel 41, cell 20 / RSW 3)
constexpr const char* kTetrapolTestTalker = "290-2-84-509";   // TP_ADDRESS of the traffic channel scenes
constexpr int kTetrapolTestRchX = 0x123;      // RTI 0.1.0x123 acknowledged on the RCH

std::unique_ptr<ModeSynth> makeTetrapolSynth(const SynthConfig& cfg, double sampleRate);
// The raw frame bits of one cycle of the test signal (before the GMSK), for the tests
std::vector<uint8_t> tetrapolTestBits(int scene, int scramble, int band);

} // namespace dect2
