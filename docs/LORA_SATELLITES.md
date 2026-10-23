# LoRa satellites (mode "LoRa satellites", family 201) and LoRaWAN (all channels, family 200)

Receive only. Nothing is transmitted, no key is handled, nothing is decrypted.

## What the satellite mode adds to the LoRa decoder

A LEO pass at 600 km moves the signal by up to about +-10 kHz at 436 MHz and +-20 kHz at 868 / 915 MHz, and the shift changes by up to
roughly 140 Hz/s (436 MHz) to 300 Hz/s (868 MHz) near the closest approach. Over a long frame (SF11 / SF12 last 1 - 3 s) that is several FFT
bins of drift, which a ground-link decoder does not follow. The satellite mode:

- widens the channel filter by twice the Doppler allowance (filter = bandwidth + 2 x allowance, set in Settings or worked out from the dial);
- runs the demodulator with `lora::Params::cfoTrack` (core/src/mesh/mesh_lora_demod.cpp): a frequency loop on the phase of the peak bin from
  symbol to symbol (the chirps' phase is continuous), the timing loop corrected for the window moves, the preamble's ramp split into the
  carrier's drift and the stretched symbol clock (`carrierHz`);
- shows each frame's carrier offset at the first data symbol and the drift through the frame; the Doppler tab draws the pass from them;
- does not let the pass planner retune the radio (a retune in the middle of a frame would break it); the planner only names the satellite.

Everything is off (bit for bit the old behaviour) for the other LoRa modes: `cfoTrack` defaults to 0.

## Measured on the generator (not on real signals)

tests/lorasat/test_lorasat_rx.cpp, 2 Msps, noise as given:

- 436.8 MHz, SF11 / 125 kHz, seven frames from +10.6 kHz to -10.6 kHz through the closest approach (-140 Hz/s): all decoded; the measured
  offsets follow the pass model, the measured rates follow it within 30 %.
- 868.1 MHz (+-21 kHz, -276 Hz/s at the closest approach), SF9 - SF11: all frames; SF12: 4 of 5 (one lost), and at the closest approach 2 of 3.
  That is the edge of what the decoder follows. Without the carrier tracking: 0 of 3.
- SF7, SF8, SF9 at 250 kHz; -5 dB with +3 kHz radio error; SF12 at -8 dB (at least one of two).

## Unverified without real signals

- No real LoRa satellite recording was available. Everything above is the generator's own model (straight-line pass, clock stretched with the
  Doppler shift). Real satellites add oscillator drift, antenna fading, interference and unknown framing.
- The sync word, coding rate and preamble length of real satellites are guesses from public lists; a mismatch shows as "preambles but no frames".
- The Doppler loop limits above (SF12 at 300 Hz/s) are measured on the generator only.

## Presets (core/src/lorasat/lorasat_presets.cpp)

Public knowledge only, none verified, each flagged `uncertain` or `plausible` (nothing is marked `documented`; a test enforces that):

| Preset | MHz | SF / BW | Sure? | Note |
|---|---|---|---|---|
| FOSSA Systems pico-satellites (FOSSASAT-1B / 2 family) | 436.8 | 11 / 125 kHz, 4/8, sync 0x12 | uncertain | later FOSSA satellites use other frequencies in 436 MHz and other modulations too |
| FOSSASAT-2 family (second frequency) | 436.2 | 11 / 125 | uncertain | seen in community lists |
| Tianqi constellation downlink | 400.45 | search / 500 kHz | uncertain | commercial IoT service, signal plan not fully published |
| Lacuna Space uplinks 868.1 / 868.3 / 868.5 | EU868 | search / 125, sync 0x34 | plausible | ground devices to the satellite: you hear the devices, not the satellite |
| Space LoRaWAN uplinks, US915 sub-band 2 | 904.6 | search / 125 | uncertain | which services listen there is not known |
| Generic 70 cm LoRa cubesat | 437.4 | 10 / 125 | uncertain | a search start for experimenters, not a fact |

Check each satellite on SatNOGS DB or TinyGS before relying on a row. The pass planner offers a satellite when the name in the elements
files starts with the preset's `tleName` (FOSSASAT, TIANQI, LACUNA).

## LoRaWAN (all channels)

core/src/lorawan: regional plans (EU868, US915, AU915, AS923-1..4, EU433; RP002-1.0.x), a bank that demodulates every channel that fits the
captured band at once (SF7 - SF12 at 125 kHz, SF7 at 250 kHz, SF8 at 500 kHz uplink, SF7 - SF12 at 500 kHz downlink, gateway frames on inverted IQ),
and the MAC parser (TS001 1.0.4: MHDR, DevAddr with its type and NwkID, FCtrl, 16-bit FCnt, FOpts MAC commands with names and short values, FPort,
MIC, join request, join accept framing, rejoin request, proprietary). "Level" is dB re full scale, not calibrated dBm. The MIC is shown and never
checked; the 32-bit counter is not rebuilt (it needs the session). Unverified on real traffic: only the generator was used.

Mode numbers used: families 200 and 201 (engine standards 202, 203; active 201, 202; poll slots 160, 161); kNumFamilies 202, kPollSeqSlots 170.

## Presets from issue #229

Two entries come from the settings a user posted from TinyGS: FOSSASAT on 401.703 MHz (SF11, 125 kHz, CR 4/8) and LoRa telemetry on
137.055 MHz (SF8, 41.7 kHz, CR 4/5, sync 0x12). 41.7 kHz is now a bandwidth of its own (the Bandwidth list and the presets). Neither was
checked against a real signal. The FSK satellites of the same thread (InnoSat 9.6 kbit/s FSK with the 0x930B51DE sync word) belong to the
CubeSat telemetry mode, which hunts that sync word.
