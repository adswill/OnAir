// LR-FHSS receiver for a wide capture (the LoRaWAN data rates EU868 DR8 - DR11, US915 DR5 - DR6, and any other LR-FHSS frame that fits):
// 1. hop detector: a spectrogram of about 488 Hz bins, power summed over a header's length (0.23 s); narrow peaks over their surroundings
//    are candidate hops (a LoRa chirp is flat over its band and does not make one), wherever they are in the band (no channel list needed);
// 2. header search: the candidate is mixed to 0 Hz and filtered to 8 samples a bit, a one-bit differential detector (GMSK as differential
//    phase steps of +-90 degrees) and the 32-bit sync word give timing, phase and the carrier; the 80 coded header bits go through the
//    tail-biting Viterbi and the CRC-8 (lrfhss.h). Any of the header copies starts a frame;
// 3. the header gives the hop sequence, the payload length and the coding rate: every payload hop is taken at its predicted time and carrier
//    (relative to the header found: the frame's own carrier offset and the device offset drop out), demodulated the same way, weighted by its
//    quality (a hop lost to a collision weighs little), and the whole payload goes through de-interleaver, Viterbi, CRC-16 and de-whitening.
// Receive only. The payload is handed on as bytes (a LoRaWAN PHY payload for lorawan::parse). Not thread safe: one thread feeds.
// Synthetic tests only; not checked against a real transmitter.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/lrfhss/lrfhss.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {
namespace lrfhss {

struct RxFrame {
    Header hdr;                  // as decoded (sync index of the copy that was found first)
    int headers = 3;             // header copies assumed (3 at CR 1/3, else 2: LoRaWAN), for the start time
    double sec = 0;              // signal time of the frame's start
    double headerHz = 0;         // carrier of the header copy found, in the input (Hz from 0)
    double centreHz = 0;         // the OCW centre in the input, device offset taken as 0 (within +-2 kHz / +-13 kHz of the truth)
    bool crcOk = false;
    std::vector<uint8_t> payload;
    float ebN0Db = 0;            // estimated from the header copy found
    float levelDb = -200;        // dB re full scale
    int hops = 0;                // payload hops
    int hopsHeard = 0;           // payload hops that carried the signal (estimated SNR over 0 dB)
    int bitErrors = 0;           // coded bits the Viterbi decoder corrected
};

struct RxStats {
    uint64_t candidates = 0;     // narrow peaks the detector passed to the header search
    uint64_t syncs = 0;          // sync words found
    uint64_t headers = 0;        // header copies with a good CRC-8
    uint64_t frames = 0;         // payloads with a good CRC-16
    uint64_t crcBad = 0;         // payloads whose CRC-16 failed
};

class Receiver {
public:
    Receiver();
    ~Receiver();
    // Starts afresh. loHz / hiHz: the part of the input searched (0, 0: +-0.45 of the sample rate)
    void configure(double inputRate, double loHz = 0, double hiHz = 0);
    void reset();
    // The samples (any amount); finished frames (good CRC or not) are appended to out
    void feed(const cf32* x, size_t n, std::vector<RxFrame>& out);
    double timeSec() const;
    const RxStats& stats() const;
    double rate() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace lrfhss
} // namespace dect2
