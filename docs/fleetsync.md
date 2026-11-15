# Kenwood FleetSync (family 347)

Receive only. FleetSync is the data burst Kenwood radios send on an analog FM voice channel when the key is pressed. OnAir follows every FM carrier of
the capture (like MDC-1200, family 345, which this mode is modelled on) and decodes the burst on each.

## What is decoded

- 1200 bit/s FFSK, mark (bit 1) 1200 Hz, space (bit 0) 1800 Hz, plain NRZ; either tone sense (inverted discriminator) works.
- Alternating preamble (24 bits, up to 3 wrong), sync word 0xA23E (up to 1 wrong), 4 lead-in bits, two 32 bit words, the low 16 bits of the second word
  are the block check (bit serial, polynomial 0x6815, plus a parity of the check field's own bits).
- FleetSync II: the data repeated as four 64 bit blocks of four 15 bit single-error-correcting code words; one wrong bit per code word is corrected.
- Fleet number = byte 2 of word 1 + 99; unit ID = (byte 3 of word 1 << 4) + high nibble of word 2 + 999.
- The screen shows time, carrier MHz, fleet, unit, FleetSync or FleetSync II, and the two raw words; the log line says the same. A burst whose block check
  fails is counted and dropped (not shown).

## What is not decoded

The description of the format this mode was written from covers only the ID fields. The meaning of the other bits (message type, status number or text,
selective call target, GPS position) is not published there, so none of it is decoded and there is no map: the raw words are shown so that a message type
can be read off them later. Nothing was guessed.

## Tests

`chanvoice_fsync`: round trip of FleetSync and FleetSync II over fleet and unit combinations, every single-bit error of FleetSync II, inverted tones,
noise, clock offset, voice around the burst, rejection of noise and damaged frames. `chanvoice_rx`: two carriers (FleetSync and FleetSync II, one off
frequency) through the whole receiver. `chanvoice_mode`: mode row, engine cases. All on simulated signals made from the same format description; no
recording of a real Kenwood radio was available, so agreement with real radios is not shown.

## Source and licence

Format description: `docs/fleetsync.md` and `internal/radio/fleetsync` of GopherTrunk by Matt Cheramie (Apache-2.0,
https://github.com/MattCheramie/GopherTrunk). The C++ is written fresh from it, no code copied; credited in THIRD_PARTY.md and the in-app credits.
