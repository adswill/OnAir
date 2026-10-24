// APCO Project 25 Phase 2 receiver (TIA-102.BBAB / BBAC): TDMA, two time slots, 6000 symbols/s differential QPSK in 12.5 kHz. Receive only.
//
// The channel is mixed to 0 Hz, decimated to 48 kHz (8 samples per symbol), root raised cosine filtered, and every sample phase is differentially
// demodulated in parallel; the 40-bit sync word (also giving the carrier offset and the SNR) picks the sample phase and frames the 180-dibit bursts.
// Per burst: the ISCH (the sync word, or the I-ISCH giving the logical channel and the place in the superframe), the DUID, the descrambling (seeded
// from the WACN, system and NAC of the control channel: setSystem), then 4V / 2V voice bursts (AMBE+2 half-rate frames and the encryption sync, ESS)
// or SACCH / FACCH MAC PDUs (Reed-Solomon + CRC: PTT, END_PTT, ACTIVE, IDLE and the TSBK-equivalent messages).
// Encryption comes from the ESS (and the PTT where a MAC PDU decodes): an encrypted call is reported, its voice never decoded; voice that arrives
// before the first ESS of a call is held until it says clear.
// Voice: AMBE+2 through the user-installed codec plug-in "ambe2" or mbelib (the NXDN / DMR path, nxdn_ambe.h); without one the voice frames are counted, no sound.
// The burst layer follows XMC_SDR's decoder and was checked on a recording of the NSW PSN (p25p2_proto.h); the MAC PDU layout is this program's own.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct P25p2Telemetry;   // p25p2_tel.h

class P25p2Receiver {
public:
    P25p2Receiver();
    ~P25p2Receiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                                        // after a retune (the logs and the site stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(P25p2Telemetry& out, uint64_t lastSeq);
    void clearResults();                                 // the call log, talkgroups, messages and the site
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);              // decode but never open the sound device
    void setListenSlot(int slot);
    // The descrambler seed (TIA-102.BBAC 7.2.5): the WACN, system ID and colour code (NAC) of the system, from its control channel. All 0: bursts
    // are taken as sent (OnAir's own test signal without a seed). Applied at the next feed().
    void setSystem(int wacn, int sysId, int nac);
    // What the control channel's grant said about the call on a slot (1 or 2): the voice channel does not name it (its MAC PDUs are not decoded
    // on a real channel). Used for a call that starts without IDs; dst -1 clears it. carrierHz: shown in the voice monitor.
    void setCallHint(int slot, int src, int dst, bool group, double carrierHz);
    // A new call to follow: the receiver starts afresh, and every report from then on carries tag (P25p2Telemetry::followTag), so the follower
    // never reads what the previous call left behind
    void startFollow(uint64_t tag);
        // 0 the first slot heard (default), 1 or 2: only that logical channel is decoded and played
    // tests: the 8 kHz voice that was played
    void setAudioTap(std::function<void(const float* x, size_t n)> cb);
    // tests and recording: every voice frame as received (logical channel 1 or 2, the 72 voice bits, whether the call is known to be clear)
    void setVoiceTap(std::function<void(int slot, const uint8_t bits72[72], bool clear)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning p25p2Tuning();

} // namespace dect2
