# TETRAPOL in 3.8

Issues #408, #333 and #371: users of a real TETRAPOL network saw only the scrambling constant in the cell table.

## Why real signals showed no information

The user's recording from #408 (SDR# 8-bit IQ, 250 kHz at 383.4125 MHz, 71 s) holds five TETRAPOL carriers. The receiver of 3.7 locked all of them: frame sync, inverted polarity, UHF coding and the scrambling constant were found, and 94 to 100 % of the frames passed their CRC on the stronger carriers. The frame layer was not the problem. The data link and what was shown were:

- The user tunes traffic channels. Their signalling is sent as LSDUs (UI_VCH, UI_CD; PAS 0001-3-4), not as TSDUs. 3.7 read them as TSDUs, found nothing it knew, and showed nothing. The talking party (TP_ADDRESS), the voice channel state and D_GROUP_IDLE were never shown.
- This network sends the encryption state of a call only on the control channel (D_GROUP_ACTIVATION). A traffic channel alone always said "encryption unknown".
- On the control channel only D_SYSTEM_INFO and D_GROUP_ACTIVATION were read, and the group and channel numbers of encrypted calls were hidden. All calls on this network are encrypted (key type 4, index 14), so the call table stayed empty.
- The control channel's logical channels (BCH, PCH, RCH, SDCH) were not separated by their superframe position, and fill frames were counted as broken blocks.
- 0.2.9 showed "traffic information" because its decoder accepted random frames: a made-up interleaver and message layout behind a 5-bit CRC tried with 128 scrambling constants. That output was not real data. The 3.0 decoder that replaced it follows PAS 0001-2.

## What changed

- **Logical channels.** The control channel's frame number comes from the BCH D_SYSTEM_INFO (cell_state bch bit; frames 0-3 and 100-103). After that, frames 98-99 (and 48-49 with mux type 1) go to the PCH, frames 14, 39, 64 and 89 to the RCH, and the rest to the SDCH. On a traffic channel, asb[0] = 1 frames go to the VCH and the rest to the SCH. Fill frames (PAS 0001-3-3 7.4.1.9) are counted, not shown as errors.
- **Messages.** Every TSDU and LSDU is one line in a new Messages table, with repeats counted on one row. Fields are decoded for D_SYSTEM_INFO (also cell BN, mux type, U scramble), D_NEIGHBOURING_CELL (channels, BN, cell ids), D_GROUP_ACTIVATION and D_GROUP_PAGING (activation type and hook, group, coverage, channel, U/D scramble, key reference, TTI), D_GROUP_LIST, D_GROUP_COMPOSITION, D_CALL_START, D_CALL_CONNECT, D_CONNECT_DCH, D_CALL_SETUP, D_EMERGENCY_NOTIFICATION (caller, cell), D_ADDITIONAL_PARTICIPANTS, D_GROUP_REJECT, D_ECH_OVERLOAD_ID, D_DATAGRAM_NOTIFY, D_DATAGRAM (data shown only when the key reference is clear), short data and status (bytes as sent), D_REGISTRATION_ACK/NAK, and release, reject and idle causes. All other downlink codes of PAS 0001-3-2 4.5 are listed by name with their bytes, then PCH (activation bitmap, paged addresses), RCH (ACK addresses, NACK noise or collision), UI_VCH (D_CHANNEL_FREE, D_START_SPEECH, ...) and UI_CD (TP_ADDRESS, TP_ALIAS).
- **Calls.** The call table shows the group and the talking party. A traffic channel takes its group and encryption state from a D_GROUP_ACTIVATION heard on the control channel for the same downlink scrambling constant. That list now survives a retune. Encrypted always counts. Clear counts only when announced in the last 30 s. Encrypted voice is still never passed to the codec.
- **Announced group calls** show group, channel and D scramble for every call. Signalling is sent in the clear even when the voice is encrypted.
- **Neighbour cells** table from D_NEIGHBOURING_CELL.
- **Sensitivity.** The circular code is now decoded with soft decisions. Frames are accepted up to 20 coded-bit errors for data and 6 for voice once the scrambling constant is locked. The search for the constant keeps the strict 10 / 3 limits. Synthetic noise sweep, frames passing: 10 dB 50 % → 97 %, 12 dB 75 % → 100 %, 8 dB 21 % → 80 %. On the #408 recording, the weak carrier 100 kHz below centre went from 733 to 1589 good frames and from 704 to 1344 messages that pass the HDLC FCS.
- **Panel.** Polarity and band coding stay automatic. Setting them by hand moved under Advanced. The status line says control channel (with the superframe position), traffic channel, or idle carrier (fill frames only).
- **Test signal.** The control channel generator sends a real 200-frame superframe: BCH, PCH paging the clear group, RCH ACKs, and on the SDCH activations, neighbour cells and fill frames. Traffic channel scenes add TP_ADDRESS, D_START_SPEECH and D_CHANNEL_FREE.

## Licence audit (sources of every table and layout)

tetrapol-kit is GPL-2.0 and OnAir is GPL-3, so nothing may be derived from it. Every table, constant and layout in core/src/tetrapol was checked against the public PAS documents from the TETRAPOL Forum download page, and the section is cited next to it in the code. The documents are PAS 0001-2 v3.0.0 (radio air interface), 0001-3-2 v2.3.9 (messages), 0001-3-3 v2.0.6 (transport and link) and 0001-3-4 v1.3.0 (circuit mode). tetrapol-kit is now credited as a behaviour reference only (THIRD_PARTY.md, the in-app credits, tools/credits_data.tsv, docs/credits-for-website.md).

- **From PAS 0001-2, and matching:**
  - the frame header, 0 1 1 0 0 0 1 0 (6.1.5.2)
  - the voice and data bit order (6.1, 6.2)
  - CRC-3, I + D + D^3, complemented (6.1.1)
  - CRC-5, I + D^2 + D^5 (6.2.1)
  - the rate-1/2 code, cyclic on the protected 26 bits and zero-tailed on the second 50 (6.1.2, 6.2.2)
  - the VHF interleaving formula (6.1.3.1)
  - both UHF K tables, compared entry by entry by a script (6.1.4.1, 6.2.4.1)
  - PRE_COD, all 47 positions (6.1.4.2)
  - the scrambling sequence (6.1.5.1)
  - the modulator differential (7.2) and the downlink spectral inversion (7.3)
  - 8 kbit/s and BT 0.25 (7.1)
  - frames numbered 0 to 199 (9.1)
- **From PAS 0001-3-3:**
  - the superframe positions of BCH, PCH and RCH, and mux type 2 (5.1.3)
  - ASB bit X on traffic channels (5.2, 6.1.1)
  - the FN values 00 / 01 11 / 01 10 11 .. 10 01 and the parity block (6.2)
  - the FCS (7.4.1.1)
  - the address Z / Y / X and the "all STs" and "no ST" values (7.4.1.4)
  - fill frames (7.4.1.9)
  - the RCH and PCH frames (7.4.2, 7.4.3)
  - the UI, UI_VCH, UI_CD and UI_P0 command codes (7.5.4, table 9)
  - the DU header and when a length octet is present (9.5.1)
  - the DT / DTE TPDU codes (8.3.1)
- **From PAS 0001-3-2:**
  - all CODOP values (4.5)
  - the D_SYSTEM_INFO normal and BSC-disconnected layouts (figures 78 and 79)
  - the D_GROUP_ACTIVATION and D_NEIGHBOURING_CELL layouts (figures 46 and 61)
  - the element lists of every message decoded (clause 4.4)
  - CELL_ID, CELL_STATE, CELL_CONFIG, ACTIVATION_MODE, ADDRESS (RFSI R1 R2 R3 F G1 G2 I1 I2 I3), REFERENCE_LIST, TYPE_NB, CCR_CONFIG and the IEI codes (5.3)
  - KEY_REFERENCE (5.3.41)
- **From PAS 0001-3-4:** the UI_VCH LSDU codes and layouts (5.4.2, 5.4.3), and the UI_CD codes TP_ADDRESS, TP_ALIAS and AMBIENCE_LISTENING (6.4).
- **Changed because the spec differs from what was there before:**
  - A clear call is key type 0 or 15 with key index 0. Before, any key index 0 counted as clear, including key type 5, the default OAK, which is a key. The test signal's clear key is now 0x00.
  - D_GROUP_REJECT's cause is octet 5.
  - D_DATAGRAM's message reference is read high octet first, and its "port" field (not in the spec) was removed.
  - D_REGISTRATION_ACK no longer reads a "group" field.
  - D_CALL_ALERT carries the code only.
  - The BSC-disconnected D_SYSTEM_INFO cell id now follows figure 79.
  - ACTIVATION_MODE is shown as type and hook.
  - The connection-oriented TPDU path reads DT and DTE only. The CR, CC and FCR header widths are in figures not checked.
  - D_CRISIS_NOTIFICATION (0x5d) and D_PRIORITY_GRP_ACTIVATION (0x85) were removed, because the public 4.5 table does not have them.
  - 0x97, 0x98 and 0x99, shown before as "not in the public PAS", are D_DDCH_DESCRIPTION, D_TKG_PRIO_LIST and D_GROUP_MASTER (4.5).
- **Removed from the tests:** the tetrapol-kit D_SYSTEM_INFO test frame. The fill-frame bytes of the test signal are one phase heard on the #408 recording. The spec's generator is given, but its figure 15 (the byte order) was not read.
- **Not confirmed from the spec text, confirmed on the real recording only:**
  - the bit order of the FN field within b0 / b1
  - the in-octet bit positions of CELL_STATE BCH (bit 5) and MUX_TYPE (bits 3..5 of CELL_CONFIG)
  - the leading four pad bits of SUPERFRAME_CPT and CHANNEL_ID in D_NEIGHBOURING_CELL
  - the RCH A bit
  - the TP_ADDRESS modifier / signature order

  These come from figures, so they were checked against real frames instead.
- **Not confirmed at all:** the D_ECH_OVERLOAD_ID cell-id padding, and the D_GROUP_LIST entry layouts beyond the figure 50 text. The open channel, talk group and emergency entry sizes match the figure text, but there is no real open channel or emergency entry to check them.

A GitHub search found no osmo-tetrapol or gr-tetrapol.

## Validated on real signals and on simulated ones

Real (the #408 recording, run through the receiver offline; not in the repository):
- Control channel at +100 kHz: 3092 frames, none bad. D_SYSTEM_INFO country 3, system 54, BN 29, cell 43, LA 0. Eight neighbour cells. Three encrypted group calls (groups 311, 410, 129 with channels and D scrambles). PCH paging the active groups. RCH NACKs. D_GROUP_LIST. Superframe position locked.
- Traffic channel at the centre (group 311, D scramble 105 then 106): calls, talking parties (two radios), D_GROUP_IDLE, D_CHANNEL_FREE, D_FORCED_LISTENING, D_START_SPEECH. With the control channel heard first, the calls show "encrypted, group 311".
- Two more traffic channels (+50 kHz, -100 kHz) and an idle carrier (-25 kHz, fill frames only).
- Nine real frames are in tests/tetrapol/test_tetrapol_rx.cpp as fixed test vectors (infrastructure messages only, no radio addresses).

Simulated only:
- VHF coding, normal polarity, D_CALL_START / D_CALL_CONNECT key references on the traffic channel, clear calls and the codec path, PCH/RCH ACK layouts, mux type 1, disconnected-mode D_SYSTEM_INFO, and every message type above that the recording does not contain (datagrams, short data, emergency, registration).
- Impairment tests: noise sweep 6-20 dB, carrier offset ±900 Hz and ±3.1 kHz, clock ±60 ppm, drift -2.5 → +2.5 kHz over 8 s (100 % frames at 20 dB), an adjacent TETRAPOL carrier 12.5 kHz away at the same level (100 %), both polarities and band codings automatic and by hand, input rates 64 k to 2.4 M, retunes, noise only (no frames).

## Not done / open

- Channel numbers to frequencies: on the #408 network, channel 2801 is 383.4125 MHz and channel 2893 is 383.3125 MHz, which is not a simple raster. "Follow clear calls" still assumes one, so it needs the network's plan.
- Segmented UI TPDUs and connection-oriented TPDUs split over several HDLC frames are not reassembled. Their first frame is shown.
- D_DDCH_DESCRIPTION, D_TKG_PRIO_LIST and D_GROUP_MASTER are named but their fields are shown as bytes.
- The GUI was not run (no app build allowed in this session). app/tetrapol/tetrapol_ui.cpp is syntax-checked only.
- test_tetrapol_engine was not run locally: it needs the engine built with the new telemetry layout. CI runs it.
