# GitHub feature requests built in 3.8 (branch feat38-issues)

Each item: what changed, how it was checked. "Simulated" means checked with generated signals or unit tests only; no real recording or radio was used.

| Issue | Change | Checked |
|---|---|---|
| #464 HFDL | Checkbox "Only aircraft with a position" in the aircraft table and the list on the right (pref hfdlOnlyPos). The map and the receiver are unchanged. | Syntax check only (GUI). |
| #449 DRM | Receiver tab: Channel filter off / 10 kHz channel (keeps +-7 kHz) / 20 kHz channel (+-12 kHz), applied on the 48 kHz stream of DRM30 before the OFDM receiver; changing it restarts the search. Default off, so nothing changes unless chosen. | Unit test of the filter on tones at three input rates (pass band 0 dB, stop band -30 dB or better beyond the edge). The effect on decoding of real stations was not measured. |
| #438 COSPAS-SARSAT | The beacon now keeps the frame as received (before BCH repair) and the bit numbers the repair changed; details pane shows both frames, buttons Copy frame / Copy received frame / Copy report; Save results exports every reception with the full frames (CSV/JSON). Bit 1 first, sync bits included, zero padded to a whole hex digit. | test_sarsat_rx extended: received and repaired frames differ by exactly the listed bits for all simulated bursts. Simulated only. |
| #455 mesh, APRS | Table size x1 / x5 / x20 for the Mesh lists (packets, nodes, messages, APRS stations, LoRaWAN devices) and for the Packet (APRS) frame and station lists. Default x1. | Syntax check; not run with a long capture. |
| #421 More tab | A picked More item shows its own name ("Table v") instead of "More: Table". Shared by every mode. | Syntax check. |
| #427 Turkish | The catalog had 3438 of 3770 texts; 332 added (machine translated, listed in unreviewed/tr.txt). | test_i18n passes. |
| #443 TETRA | New More > Talkgroups: every GSSI heard in a call that is not encrypted, kept across restarts in tetra_talkgroups.csv in the data folder, with first/last heard, call count, frequency, slot; Save results (CSV/JSON), Copy GSSI list, Clear. ISSI are not kept. | Syntax check; simulated only (needs a TETRA signal). |
| #453 location | FT8 / WSPR screen: collapsible "My location" with the one program-wide receiver position (same as ADS-B and Passes); it is drawn as "home" on the map. | Syntax check. |
| #375 Chinese | TV player and teletext texts in tv.cpp wrapped in tr() and translated (46 texts, unreviewed). Other screens with untranslated literals remain. | test_i18n passes. |
| #431 DMR | Voice monitor (DMR, P25, NXDN, dPMR, D-STAR, Yaesu, TETRA): new Channels tab listing every talkgroup heard with Mute, Solo and Priority checkboxes; they edit the Filters lists (mute list, allow list with "play only the allow list" on while any solo is set, priority list). | Syntax check. |
| #432 FT8 | FT8/FT4/WSPR decode lists: Save results, Copy CSV, and the text log with Auto-save (a new file each day). A UDP feed was not built. | Syntax check. |
| #436 RDS | Rows for PTY number, TP, TA and music/speech; language (code table of EN 50067 annex J written from memory, not checked against the standard); other networks (EON) now show PTY, TP and TA (groups 14A variant 13 and 14B). TMC, RT+, eRT, ECC, PIN, CT and AF were already there. | test_fm_rds extended with 14A/13 and 14B groups: passes. Simulated groups only. |
| #368 | Meteor: Colour 124 composite and automatic pick when only APID 64, 65 and 67 arrive; Vegetation false colour (channel 2 in red, channel 1 in green and blue; my choice of mapping, the old one is not documented). ADS-B: label text size (70-250 %) and label contents (altitude and speed, squawk and category, registration and operator). DAB transmitter sites: the list is the crowd-sourced OnAir-channels repository (dab-tii/<country>.csv), nothing in the code to change. | Syntax check; display-options header test line added. |
| #466 ISDB-T | TX panel: channel width, FFT mode, guard interval, constellation and code rate of layer B. Defaults unchanged (mode 3, 1/8, 64-QAM 3/4). Area code, PIDs and bitrates come from the transport stream; not exposed. | The generator/receiver chain tests already cover these parameter combinations; the TX panel path was syntax checked only. Whether the reporter's receiver accepts the signal is unknown. |
| #357 / #430 R8600 | The firmware check now accepts the one-byte CPUCS writes (0xE600 = 0 or 1) in the middle of the sequence. | test_icom_r8600 extended. The SHA-256 of a real file was not available. |

## Not built

- #397: needs a download of an external archive (two real SGB recordings); not done without approval.
- #233: the Soapy gain elements, the Soapy device string (Add radio > SoapySDR device string) and the Airspy/SDRplay stages are already in. Stage gains for LimeSDR, bladeRF and USRP were not added: no hardware to check them.
- #242: real second-monitor windows need the ImGui docking branch with multi-viewport; the tree uses master ImGui. Not feasible as a small change.
- #165: already in (long station name first, album art, per-service logos).
- #245, #398: need outside information.
