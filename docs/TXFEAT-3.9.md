# TX features 3.9: DTMF keypad and DMR slot sync

Branch `feat39-txfeat`. No new modes. Both features extend existing transmit paths.

## 1. DTMF keypad (NFM, AM, SSB, airband and QO-100 voice)

- Core: `core/include/dect2/tx/tx_dtmf.h`, `core/src/tx/tx_dtmf.cpp`.
  - The keys are 0-9, `*`, `#` and A-D. The tones are the ITU-T Q.23 row and column pairs, with a short raised-cosine ramp on each tone.
  - The panel pushes keys into a thread-safe queue. The analog voice generator (`tx_voice.cpp`) sends each key as a tone, followed by a gap.
  - While a key or its gap plays, the microphone is muted and the VOX gate is held open.
  - On NFM the tone goes through the same pre-emphasis as the voice. The receiver's de-emphasis then leaves the two tones at the level that was set.
- Panel: TX > audio source > "DTMF keypad (EchoLink, IRLP, repeater commands)".
  - A 4x4 keypad and a key-sequence box. Keys go out immediately while transmitting.
  - Under Advanced: tone length (default 100 ms), gap (default 100 ms), level per tone (35 %) and twist (0 dB). These are kept in the settings (`tx.dtmf.*`).
  - Broadcast FM and the digital vocoder modes do not show the keypad.
- Test: `tests/tx/test_tx_dtmf.cpp` (ctest `tx_dtmf`).
  - Every key goes through NFM, AM and USB transmit and back through OnAir's tone decoder.
  - Impairments: carrier offset, noise and timing settings.
  - Simulated signals only. It has not been tested on air against an EchoLink or IRLP node.

## 2. DMR subscriber TX slot timing (ETSI TS 102 361-1)

A subscriber working through a repeater must "synchronize to an outbound channel and base its inbound timing entirely on the outbound timing" (clause 4.4.1). OnAir is half-duplex. It first listens to the repeater output in the DMR receiver, then keys up so that its bursts fall in the right inbound slot.

The rules used, from the spec (V2.6.1):
- The centres of inbound and outbound bursts are aligned (4.2).
- Inbound channels 1 and 2 are offset by 30 ms from outbound channels 1 and 2. The CACH in front of outbound slot N announces (TC, AT) the inbound slot N burst one slot later (4.2, 4.5, figure 4.8). So the centre of inbound slot N is the centre of outbound slot N plus 30 ms.
- Table 5.1: with offset channel timing (the default), logical channel N uses inbound slot N. With aligned timing (an Advanced option) it uses inbound slot 3-N.
- If the repeater is not transmitting, the call starts at once, asynchronously. This is the "BS activation" case.

Code:
- `core/include/dect2/dmr/dmr_slot_sync.h`, `core/src/dmr/dmr_slot_sync.cpp`.
  - `DmrSlotClock` fits the outbound 60 ms frame to the clean CACH reports.
  - Reports that disagree with the median phase by more than 3 ms are dropped, which removes wrongly decoded TC bits.
  - A least-squares fit gives the phase. The frame length is fitted only over spans longer than 20 frames.
  - `dmrTxLeadFor` turns the fit into the silence to put before the first burst.
- Receiver (`dmr_link.cpp`, `dmr_rx.cpp`): every clean CACH (TACT without a correction) reports the centre time of the burst after it, together with TC and AT.
  - The time is taken back to the input clock by removing the filter delay: half of each linear-phase decimation stage, plus a measured 48 kHz part.
  - It is mapped to the steady clock at the end of each `feed()` and passed to `DmrSlotClock::global()`.
  - `DmrReceiver::setBurstTimingCallback` exposes the times on the input clock.
- Generator: a new `DmrGenConfig::txLeadSec` adds silence before the first slot (resolution 1/48000 s).
- Transmit path (`tx_payload.cpp`): a DMR transmission applies the lead when the repeater was heard in the last 10 s.
  - Request options: `dmr.slotSync` (default on), `dmr.aligned` and `dmr.latencyMs`.
- Panel: in the DMR transmit box, "Sync to the repeater's time slots (listen first)" is on by default.
  - It shows the fit (age, bursts, residual), the inbound slot used, and whether the repeater reports that slot busy (AT). This is a warning only; nothing is blocked.
  - Under Advanced: aligned channel timing, and the radio latency to the air in ms.

Test: `tests/dmr/test_dmr_slot_sync.cpp` (ctest `dmr_slot_sync`):
1. The generator delay to symbol 0. The lead and slot 2 shift the burst exactly.
2. The receiver's burst times against the generator's outbound truth at 1.2, 2, 2.4, 8 and 10 Msps (mean and spread of the error), and the TC slot.
3. Listen, fit and key up, for both logical channels with both offset and aligned timing. This runs with carrier offset (up to 2.5 kHz), noise (14 to 35 dB), a 5 ppm clock error, a random start phase and key-up 0.2 to 3 s after listening. Every inbound burst that OnAir's own receiver finds in the transmission must sit within 0.25 ms of its slot. The guard time is 1.25 ms on each side.
4. Fit robustness: 11 % wrong TC bits, a stale fit (more than 10 s old), no observations, and a clock that goes back.

Measured results are in the PATHWAY entry.

Limits, stated plainly:
- The slot phase is exact on the receiver's input clock.
- The step from that clock to the moment the first TX sample reaches the antenna depends on the radio's receive and transmit buffering. OnAir cannot measure that buffering without timestamped hardware, so it is the user-set "Radio latency to the air" (default 0 ms).
- On a HackRF, USB buffering is several milliseconds, which is more than the guard time. The latency must be set before a repeater accepts the call.
- Simulated signals only. It has not been tested against a real repeater.
