# Voice trunking modes (3.8)

Family block 320-339. Everything here was validated on generated signals only; no real recording of any of these systems was available.

| Item | Result | Family |
|---|---|---|
| Motorola Type II (SmartNet, SmartZone) | Control channel decoded, calls followed inside the capture | 320 |
| TETRA Direct Mode | DSB / DNB decoded, calls listed, clear speech to the user-installed codec | 321 |
| OpenSky | Identifier only: the air interface is not public and the traffic is encrypted | 322 |
| ProVoice on EDACS | Added to the EDACS follower, through the user-installed IMBE plug-in | (EDACS, 130) |
| Hytera XPT | Site status, adjacent sites and call set-up read inside the DMR receiver; no call following | (DMR) |
| Tait trunking | Not built: no public description of a Tait-specific trunking protocol | - |

## Motorola Type II (`core/src/smartnet`, `app/smartnet`)

3600 bit/s FSK, 84 bit OSWs: sync 0xAC, 76 bits interleaved (19 x 4) from 38 (information, parity) pairs, CRC-10, and the masks on address and command. The layout, the CRC, the band plans and the meaning of the messages follow OP25 and trunk-recorder (GPL-3); Motorola publishes no description.

- One click: the band plan follows from the tuned frequency and the control channel broadcast, and SmartZone is reported when the site says it is wide area. VHF / UHF (OBT) sites need a custom plan under Advanced.
- Following uses the trunk pool (`dect2/trunkpool`) with one wide capture as its radio (`trunkpool/capture_voice.h`): each call is a slice of the capture, analog as FM, digital (ASTRO) through the P25 receiver and the user-installed IMBE plug-in. Calls outside the capture are listed and not followed, with the reason.
- Encrypted calls are logged, not followed and not decoded. Unit-to-unit calls are logged and followed only when "Also follow unit-to-unit calls" is on. Interconnect (telephone) calls are logged only.

Measured on the generator (`tests/smartnet/test_smartnet_rx.cpp`): 256 of 256 good OSWs in 6 s at offsets of -1500 and +900 Hz, at clock errors of +150 and -300 ppm, inverted, with 5 % corrupted words the rest were still read (240), 0.6, 0.48 and 1.92 Msps, with a noise sweep up to 0.16 per component against an amplitude of 0.5. Calls followed with the tone recovered in every case. Pure noise never produced a word.

Limits: not run against a real SmartNet or SmartZone site. Only the first OBT band plan layout (base, spacing, offset) is exposed in the panel; the mid and high ranges are in the core only. Multi-radio following (a second radio retuned to calls outside the capture) needs an app adapter that only P25 has today.

## TETRA Direct Mode (`core/src/tetradmo`, `app/tetradmo`)

Written from ETSI EN 300 396-2 and -3 V1.4.1. DSB found by differential correlation on the 38 bit training sequence; SCH/S and SCH/H decoded with the shared TETRA coding; DNB read with the slot clock of the DSB; SCH/F and STCH signalling parsed; TCH bursts go to the existing clear-speech path (`processTrafficBurst`). The DM colour code comes from the MNI and source address of the DMAC-SYNC that set up the call.

Encrypted calls: `air interface encryption state != 0` or the end-to-end flag stop the traffic before it reaches the codec. Nothing is decrypted, and no call is decoded before a clear DM-SETUP has been heard for it.

Measured (`tests/tetradmo/test_tetradmo_rx.cpp`): all 40 traffic bursts delivered at 72 and 240 ksps and at 2.4 Msps, at -1800 and +800 Hz, at +100 ppm, via a repeater and for an individual call; 40 of 40 with noise 0.05. 35 to 39 of the 40 bursts arrive with 8 or fewer wrong bits (the rest have about 10; the cause was not traced). The speech itself is not tested here: there is no codec in the test.

## ProVoice on EDACS (`core/src/edacs/provoice.cpp`)

The 768 bit blocks are found on their sync word; the four IMBE 7100x4400 frames are corrected, rewritten to 7200x4400 and re-coded for the plug-in format. Without the plug-in the call is listed as ProVoice with a note saying what to install.

Measured (`tests/edacs/test_edacs_provoice.cpp`): against a stand-in plug-in, 156 frames reach it and 144 are bit-exact in a 3 s call, at two noise and offset settings. No IMBE speech has been heard from this path: the IMBE codec is not in the repository.

## Hytera XPT (`core/src/dmr/dmr_xpt.cpp`)

Recognised inside the DMR receiver from CSBK FID 0x68 (opcodes 0x0A site status and 0x0B adjacent sites) and the full LC FLCO 0x09 call set-up; shown in the DMR panel. DSD-FME calls this work experimental, and no maker document exists. The status table and call set-ups are read and shown; the following of calls is not built. To hear the calls, use the whole-span DMR mode with a capture that covers the site.

Measured (`tests/dmr/test_dmr_xpt.cpp`): the generator's blocks are read at 35, 18 and 10 dB SNR.

## OpenSky (`core/src/opensky`)

OpenSky has no public air interface. The mode measures the symbol rate, the frequency levels, the occupied width and the burst pattern, and says whether the signal fits (9600 baud +- 300, four levels). Nothing is decoded. The match is by those measurements only, so a signal of the same shape from another system would also match.

## Tait

Tait's trunking products (TaitNet, TN9300) are built on MPT 1327 and DMR Tier III, which the existing modes cover. No description of a Tait-specific control channel is public, so no mode was added.

## Licences and credits

OP25, trunk-recorder (GPL-3), DSD, mbelib (ISC) and DSD-FME are listed in `tools/credits_data.tsv` and in the in-app credits. Codecs stay user-installed.
