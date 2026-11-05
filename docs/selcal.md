# SELCAL (aircraft tone call)

Receive only. The mode **SELCAL (aircraft tone call)** (ARINC 714) logs the selective call that alerts an aircraft's crew on an HF or VHF voice channel. It is its own mode, separate from the tone decoders (CTCSS, DCS, DTMF, selcall), which still decode SELCAL too.

## Signal
Two pairs of tones, each pair one second long with a gap of about 0.2 s. 16 tones from 312.6 Hz to 1479.1 Hz, named by the letters A B C D E F G H J K L M P Q R S. A code is four letters, written `AM-CP`. Oceanic and remote-area aircraft are called this way on the HF air-ground channels (USB), and on VHF airband (AM).

## Window
- Calls: time (UTC), code, the four tone frequencies, the dial frequency, the level and the aircraft.
- Codes: one row per code with the last time heard and the count.
- The channel is demodulated as USB below 30 MHz and AM above in Auto; Auto, FM, USB, LSB and AM can be chosen. A mistuned SSB radio is followed within 25 Hz.
- Listen plays the channel audio.

## Position and the aircraft
A SELCAL code identifies an airframe (assigned to its registration by the operator) and says nothing about where it is, so there is no map entry. To see which aircraft was called, put a file `selcal_aircraft.tsv` in the data folder, one `CODE<TAB>registration` per line (`#` for comments). Nothing is downloaded.

## Detector
100 ms Hann windows, two tones in balance, each pair at least 0.55 s, the second pair within 0.8 s of the first (see tones.md). Nothing else runs in this mode (no CTCSS, DCS, DTMF or five-tone decoding).

## Test signal
The built-in test signal is a loop of 22 s: speech, then AM-CP, DH-KS, AM-CP. Only synthetic signals have been tested; a recording from the air is welcome.
