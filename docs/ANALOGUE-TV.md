# Analogue TV

The mode **Analog TV** receives broadcast analogue television: the vision carrier is found by itself, the picture is locked and decoded (PAL, SECAM, NTSC,
also monochrome), and the sound is played. It is receive only; the test signal generator (Tools, test signal) makes the same systems for trying it out.
FPV video links and NBTV / mechanical television are settings of the same receiver (see `fpv.md`).

## Tuning

Tune the **centre of the channel**. A channel is 6, 7 or 8 MHz wide and the receiver needs at least 8 Msps (an Airspy R2 at 2.5 Msps is too slow for
it, use its 10 Msps setting). If you tune the **vision carrier** itself (channel tables list it, for example 471.25 MHz for E21), that works too: when no carrier is
found away from the centre, a carrier at the very centre with a sound carrier next to it is taken. At 10 Msps the sound carrier is then beyond the edge of the band (the picture
still locks); 16 to 20 Msps gets the sound too.

## Systems

B, G, H, I, D/K, K', L, L' (625 lines), M and N (525 / 625 lines with NTSC, PAL or SECAM colour), and the early 405-line (A), 819-line (E) and C systems.
The system, the colour system and the sound carrier are found by themselves; the settings under **Decoder** only override that.

## Sound

| System | Carriers | Notes |
|---|---|---|
| FM / AM mono | the sound carrier (4.5, 5.5, 6.0, 6.5 MHz FM; A, C, E, L AM) | always |
| A2 (Zweikanalton) | second FM carrier 242.1875 kHz above the first, B/G and D/K | pilot 54.6875 kHz on the second carrier: not modulated = mono, 117.5 Hz = stereo, 274.1 Hz = two languages. Stereo: first carrier L+R, second carrier R. |
| NICAM-728 | DQPSK, 364 ksymbol/s, 5.85 MHz above the vision carrier (system I: 6.552 MHz) | stereo, two mono channels, or one mono channel plus data; ETSI EN 300 163. |

The receiver looks for A2 and NICAM by itself and uses them when they are valid; **Sound** in the Decoder settings can turn that off (FM / AM carrier only).
With two languages (A2 two-language, NICAM dual mono) **Two languages** picks the first or the second. The list on the right says what the sound comes
from ("NICAM stereo", "A2 stereo", "FM"). Not in: BTSC / MTS (M system stereo needs the licensed dbx-TV noise reduction: the mono L+R part plays as before),
EIAJ FM-FM, Sound-in-Syncs.

The test signal has a **second sound** option (More... in the test signal panel): A2 stereo, A2 two languages, A2 mono with its pilot, NICAM stereo,
NICAM two languages. The tones are 1 kHz (left, first language) and 1.5 kHz (right, second language).

## No picture: snow

**Show snow when there is no picture** (Decoder settings, off by default): without a picture signal the noise of the channel is drawn on a free-running
raster, as a TV does with no station, instead of a black picture. A weak carrier shows up in it before the picture locks. The words "no signal" move to a corner.
The spectrum and the waterfall of the main window always show the noise; they are not hidden by this mode (set their range with Auto on the waterfall).

## Tests

`test_atv_sound2` (A2 and NICAM alone and in a whole TV signal, with carrier offsets, noise, sample rates 8 to 20 Msps, system I), `test_atv_snow` (snow, vision
carrier at the centre), `test_atv_radio_sweep` (radio rates, tuning errors, mirrored spectrum, low level).

## Issue 73: more analogue systems, status

| Item | Status |
|---|---|
| D / D2-MAC over FM | done (MAC TV mode) |
| A-MAC, B-MAC, C-MAC, E-MAC, D / D2-MAC in AM (cable) | not now: C-MAC has its data as PSK in the carrier, B-MAC is a different multilevel format; no public test signals |
| HD-MAC, HDB-MAC | not now: needs the D2-MAC decoder plus the HD assistance and motion decoding of ETS 300 352; no signals to test with |
| MUSE (E, T, narrow, 9) | not now: sub-Nyquist sampling with motion compensation, no free full specification and no test signals |
| NTSC-FM, PAL-FM, SECAM-FM (satellite) | partly: the FM video decoder of FPV video receives FM video at 16 Msps and above; satellite audio subcarriers and de-emphasis, energy dispersal are not done |
| Extended PAL | not now: needs an exact description (PALplus helper signal?) |
| NICAM-728 | added now |
| A2 (Zweikanalton) | added now |
| Sound-in-Syncs | not now: the pulse code is not in a specification at hand |
| MTS / BTSC | not now: stereo and SAP need dbx-TV (licensed); L+R mono already plays |
| EIAJ FM-FM | not now: the numbers of the specification are not at hand |
| PALplus, ClearVision, ACTV | not now: helper signal decoders, no signals |
| Mechanical and early systems 24 to 240 lines | done (NBTV / mechanical setting: any line count 24 to 240 and picture rate) |
| 300, 343, 375, 441, 455, 567 lines | not now: electronic systems with MHz of video bandwidth, not the audio-band NBTV scheme |
| SECAM-I, NTSC-I, OSKM (NTSC D/K) | done (system and colour system are chosen independently) |
| NTSC-A (405), SECAM-E (819), NIIR | not now: 405 and 819 are monochrome in the system table |
