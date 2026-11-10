# DMR subscriber transmit (3.8)

Real DMR transmit from OnAir's transmit panel, so a radio that transmits (HackRF, PlutoSDR, USRP, LimeSDR …) can talk on a DMR
repeater's input or, where the system allows it, send a control-channel service request. The signal is built from the operator's own
DMR ID and callsign (entered in the transmit panel / settings) and the payload chosen in the panel.

Branch `feat38-dmr-tx`. Built on `pub/integ-38`.

## What is built (ETSI TS 102 361-1 / -2 Tier II, -4 Tier III subscriber side)

New module `core/src/dmr/dmr_tx.{h,cpp}` with `DmrTxConfig` and `makeDmrTxSynth`. It produces one MS-sourced transmission and plays it
once in the chosen time slot, then the carrier goes off (a real push-to-talk). It reuses the existing burst builders (`dmr_proto.h`,
`dmr_fec.h`), the 4FSK pulse shaping and FM modulator of the generator, so the framing is exactly the receiver's own: 4800 sym/s 4FSK,
264-bit bursts, the 48-bit sync patterns, colour code in the slot type (Golay 20/8), BPTC(196,96), embedded link control, voice LC header
and terminator with Reed-Solomon(12,9), CACH-less inbound bursts with the 2.5 ms guard time between slots.

Call types (transmit panel, step 2, "What to send"):

1. **Group voice call** — talkgroup voice: voice LC header, voice superframes (sync in burst A, embedded LC in B–E), terminator. The
   callsign is sent as the **talker alias** (ISO 8859-1, header + up to three blocks, in every second superframe).
2. **Private (unit-to-unit) voice call** — FLCO unit voice, destination radio ID.
3. **All call** — destination 0xFFFFFF.
4. **SMS text message** — preamble CSBK, short-data-defined header, rate-1/2 data blocks, message CRC-32. Coding is chosen automatically:
   ISO 8859-1 when the text is Latin-1, otherwise UTF-16LE.
5. **LRRP / GPS position report** — a real Motorola LRRP document (Triggered-Location-Report, circle-2d latitude/longitude) wrapped in a
   UDP/IPv4 packet (new encoder `dmrLrrpEncode` in `dmr_lrrp.cpp`, the exact inverse of the receiver's parser), carried in an unconfirmed
   data message. A NoFix status report is also available.
6. **Kerchunk** — one short voice superframe (header + 1 superframe + terminator) to open a repeater without a full call.
7. **Tier III registration** and **8. Tier III voice service request** — a C_RAND control-channel CSBK (CSBKO 01 1111, MFID 0, TS 102 361-4
   §7.1.1.2.1) with the service kind, the target address and the requesting MS as the source. **Subscriber side only**, for a system whose
   parameters the user enters themselves.

### Repeater access

The transmit panel already offers the frequency, an input/output split and the gain; a DMR transmission tunes like the receiver (the mode's
own tune offset is applied in `tx.cpp`). Set the repeater input frequency, the colour code, the time slot and the talkgroup; kerchunk or a
voice call then keys the repeater. BrandMeister / TGIF talkgroups are just the destination talkgroup number, and the Talker Alias carries the
callsign. Aligned vs. offset channel timing (TS 102 361-1 §5.1.1): OnAir transmits inbound bursts in the chosen logical slot with the 1 ms
slot-timing guard built into the burst; a real two-frequency repeater bases the MS timing on the received outbound CACH.

## Voice (AMBE+2)

OnAir **cannot encode speech**: AMBE+2 is patented and both mbelib and the user-installed codec plug-in (`codec_registry.h`,
`speech_codec.h`) expose a **decode** path only — there is no `onair_speech_encode`. So the voice bits of a voice burst are a test pattern;
the destination ID, talkgroup, colour code, talker alias, framing and timing are all correct and on the air. The panel says this plainly and,
as before, offers analog NFM of the microphone / a WAV as an alternative. If a future plug-in exposes an encode path this is the place to wire
it; nothing is bundled.

## Licence / safety

Transmitting on DMR needs a licence (amateur, or the system operator's permission). OnAir never blocks a frequency, power or band; the panel
shows a clear warning and the user must arm TX (the existing TX arm/stop flow in `tx.cpp`). No encryption is applied; the privacy flag of the
service options can be set but the payload is never scrambled, and OnAir never attempts access to an encrypted system or decrypts without
user-imported keys.

## Tests

`tests/dmr/test_dmr_tx.cpp` loops the transmitted bursts back into the real DMR receiver (the `dmr_eval.h` harness: generate → 8-bit round →
`DmrReceiver` → compare with the truth the generator recorded), with impairments:

- group voice with a talker alias, clean, colour code checked;
- group voice with +1.2 kHz carrier offset, 30 ppm clock error, 18 dB SNR, colour code 7, slot 2;
- private voice, all call;
- SMS text (text and CRC must match);
- LRRP position (the decoded fix must match the transmitted latitude/longitude);
- Tier III voice service request (logged as a control message);
- slot selection (a slot-2 transmission is heard in slot 2).

The LRRP encoder is also checked standalone against the receiver's parser for a spread of coordinates (Vilnius, Sydney, 0/0, London, the
antimeridian) and the NoFix form — all pass locally.

## Status / not done

- **Not tested on air against a real repeater.** Everything here is validated against OnAir's own receiver on simulated signals only.
- The full generator→receiver round-trip (`test_dmr_tx.cpp`) is run in CI: the prebuilt overnight library predates the integ-38 Rust proto FFI
  (`onair_dmr_alias_pieces`), so it cannot link locally. The LRRP piece and every changed file were verified locally
  (`clang++ -fsyntax-only`, and a standalone LRRP round-trip).
- Voice carries no speech (no AMBE encoder, by design).
- Tier III is the subscriber request only; there is no TSCC emulation, no registration-accept handling and no full-duplex session — the user
  supplies the system parameters.
