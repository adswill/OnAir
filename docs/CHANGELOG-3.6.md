# OnAir 3.6

3.6 follows 3.5.0. This list is written from the git log since the v3.5.0 tag and from docs/PATHWAY.md. It names only what the log shows. The mode count (178, was 158 in the README before this update) is generated from the mode list in the app by tools/release/gen_changelog.py.

**Measured** numbers come from OnAir's own test signals, not from over-the-air recordings, unless a line says so.

## New modes

- **GK-2A** LRIT (family 230) and HRIT (231), with DES-ECB and a bundled JPEG 2000 decoder (OpenJPEG 2.5.3) for the pictures.
- **FengYun**: three mode entries, FY-4 LRIT with satellite choice (232), FY-4A HRIT (233) and FY-2 S-VISSR (234); FY-3 AHRPT (235) and FY-3 MPT (236).
- **PACTOR-II, PACTOR-III, PACTOR-IV** (physical layer) and Multitone framing.
- **SITOR-B** receiver with the M.476 table, replacing the earlier placeholder row.
- **SELCAL** as its own mode with a call log; CCIR / ZVEI / EEA / EIA / DZVEI selective calls next to POCSAG and FLEX in the pager modes.
- **Inmarsat Aero C voice** in the Voice tab (aeroambe codec plug-in, installed by you); Iridium voice finished.
- Modes that were labelled work in progress are finished and lose the label: eLoran, DME, DGPS beacons, Yaesu Fusion, MPT1327, EDACS, LTR.

## Decoder quality and sensitivity

- Sensitivity measured against theory in three passes, with harness modes for HFDL, NAVTEX, DSC VHF, CDR, DVB-C, ATSC 3.0, DRM 16-QAM, DRM+, airband and cell; tables are in the docs per mode.
- ATSC 3.0: channel estimate from the pilot comb with noise taps dropped (AWGN 4 to 2 dB, TU6 16 to 12 dB).
- DVB-S2: fix for a false header lock at very low Es/N0; VL-SNR hunt first on weak two-header chains.
- Digital voice: frequency capture for narrow-band voice modes and "why is it not decoding" hints; P25 fine capture only beyond 3.6 kHz.
- HFDL: why-bad counters in the Receiver tab. ACARS timing loop 1 dB better.
- eLoran tuning-error tolerance to +-250 Hz; DGPS parity and full RTCM messages; MPT1327 per the 1997 standard with single-bit repair; EDACS real BCH; LTR check bits verified.
- DRM: xHE-AAC config extension elements fixed.
- Auto gain rebuilt: index-table radios, ADC-limited start, SDRplay split, per-mode targets, spur notch.

## Ease of use

- **Simple / Advanced view per mode**, rolled out to ADS-B, AIS, ACARS, VDL2, pagers, APRS, LoRa, TETRA, P25, NXDN, D-STAR, dPMR, M17, radiosondes, Inmarsat-C and Aero, Iridium, Orbcomm and the weather-satellite modes.
- **Scan and Identify**: one button in the Simple view.
- **First-run setup v3** with regional presets, antenna hints, a codec screen and an easy mode.
- Spectrum zoom keeps the marker; target mode keeps the marker centred; double click tunes.
- Discord button in the menu bar and the Help menu.
- WEFAX picture rotation; Linux AppImage release asset with in-place update; the Linux .deb is downloaded, checked and the install command shown.

## Wide capture, trunking and voice watch

- **Everything in view**: several signals decoded from one wide capture, with TETRA and Inmarsat span decoders.
- **Multi-radio trunking**: a pool controller, channel plan and access CSV, per-call recorder, grant taps for P25, DMR, TETRA, MPT 1327, EDACS and LTR, and a trunking window.
- **Voice watch list** across modes and **pager keyword alerts**.

## Web, relay and AI

- **OnAir Web** redesigned as a port of the app look, with a settings panel, translations, offline map, AGC / gain and a memory overlay.
- **Share my radio**: outbound relay client, listener list with kick and block, invites, bans, and the relay service itself (relay/relay.js, one dependency-free Node file).
- Map: icon sprite, grouped layers, filters, themes, shortcuts; ham contacts no longer reach the map as zero.
- **MCP server and an AI assistant panel**: assistants can drive OnAir through MCP (stdio and HTTP).

## Recording, replay and export

- Record and replay for any mode (16-bit recording, SigMF ci16); audit in docs/RECORD-REPLAY.md.
- Shared export bar (CSV, KML, GeoJSON, copy) for more modes; search results as CSV (audit in docs/EXPORT-AUDIT.md).

## Bandwidth and sample rate

- Custom bandwidth and sample rate in every mode through one shared path; the default is unchanged and limits only warn (audit in docs/BANDWIDTH-AUDIT.md).

## SDK

- Plug-ins and scripts page in Settings; plug-in SDK audit in docs/PLUGIN-SDK.md.
- Mesh transmit from the user's own node identity.

## DSC and SELCAL

- DSC radio-level and bit-level robustness sweeps (offsets, mistuning, noise, fading, 250 ksps to 20 Msps, clock error, clipping).
- SELCAL mode, see above.

## Fixes and pull requests

- #409, #410, #418: H.264 hardware-decode fallback, general-receiver wheel tune, FM scan hop timeout; issues #408, #411, #412 handled.
- Pager keyword patterns with nested quantifiers are refused (they hung the regex engine).
- Pull requests: Inmarsat satellite naming, shared thread pool and carrier width 10 dB under peak (#413, #415, #416, BrunoKlu); HydraSDR RFOne driver (#417); Aero voice parts of #414.
- Windows / test carry-over failures from 3.5 fixed; web policy classes SITOR and PACTOR as blocked.

## CI

- Timing tests made deterministic; MinGW `near` macro build fix; a real-signal test library (ADS-B, DTMB, DSC; docs/REAL-SIGNALS.md); MCP protocol tests; 15 more web client JS tests.
