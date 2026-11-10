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
- **Messages.** Every TSDU and LSDU is one line in a new Messages table, with repeats counted on one row. Fields are decoded for D_SYSTEM_INFO (also cell BN, mux type, U scramble), D_NEIGHBOURING_CELL (channels, BN, cell ids), D_GROUP_ACTIVATION, D_GROUP_PAGING and D_PRIORITY_GRP_ACTIVATION (group, coverage, channel, U/D scramble, key reference, TTI), D_GROUP_LIST, D_GROUP_COMPOSITION, D_CALL_START, D_CALL_CONNECT, D_CONNECT_DCH, D_CALL_SETUP, D_CALL_ALERT, D_EMERGENCY_NOTIFICATION, D_CRISIS_NOTIFICATION, D_ADDITIONAL_PARTICIPANTS, D_GROUP_REJECT, D_ECH_OVERLOAD_ID, D_DATAGRAM_NOTIFY, D_DATAGRAM (data shown only when the key reference is clear), short data and status (bytes as sent), D_REGISTRATION_ACK/NAK, and release, reject and idle causes. The rest are listed by name, then PCH (activation bitmap, paged addresses), RCH (ACK addresses, NACK noise or collision), UI_VCH (D_CHANNEL_FREE, D_START_SPEECH, ...) and UI_CD (TP_ADDRESS, TP_ALIAS). Codes the public PAS does not list (0x97, 0x98 and 0x99 on this network) are shown as their bytes.
- **Calls.** The call table shows the group and the talking party. A traffic channel takes its group and encryption state from a D_GROUP_ACTIVATION heard on the control channel for the same downlink scrambling constant. That list now survives a retune. Encrypted always counts. Clear counts only when announced in the last 30 s. Encrypted voice is still never passed to the codec.
- **Announced group calls** show group, channel and D scramble for every call. Signalling is sent in the clear even when the voice is encrypted.
- **Neighbour cells** table from D_NEIGHBOURING_CELL.
- **Sensitivity.** The circular code is now decoded with soft decisions. Frames are accepted up to 20 coded-bit errors for data and 6 for voice once the scrambling constant is locked. The search for the constant keeps the strict 10 / 3 limits. Synthetic noise sweep, frames passing: 10 dB 50 % → 97 %, 12 dB 75 % → 100 %, 8 dB 21 % → 80 %. On the #408 recording, the weak carrier 100 kHz below centre went from 733 to 1589 good frames and from 704 to 1344 messages that pass the HDLC FCS.
- **Panel.** Polarity and band coding stay automatic. Setting them by hand moved under Advanced. The status line says control channel (with the superframe position), traffic channel, or idle carrier (fill frames only).
- **Test signal.** The control channel generator sends a real 200-frame superframe: BCH, PCH paging the clear group, RCH ACKs, and on the SDCH activations, neighbour cells and fill frames. Traffic channel scenes add TP_ADDRESS, D_START_SPEECH and D_CHANNEL_FREE.

Audit against PAS 0001-2 / 0001-3 and tetrapol-kit (GPL-2; read as a reference, no code copied, credits updated): symbol rate 8 kbit/s, differential decoding, frame sync on the 0x46 header, the 127-bit scrambling sequence, VHF and UHF interleaving, UHF precoding, CRC-3 and CRC-5, data block frame numbers, FCS and the TPDU UI header all match. A GitHub search found no osmo-tetrapol or gr-tetrapol. tetrapol_dump is part of tetrapol-kit, which was read.

## Validated on real signals and on simulated ones

Real (the #408 recording, run through the receiver offline; not in the repository):
- Control channel at +100 kHz: 3092 frames, none bad. D_SYSTEM_INFO country 3, system 54, BN 29, cell 43, LA 0. Eight neighbour cells. Three encrypted group calls (groups 311, 410, 129 with channels and D scrambles). PCH paging the active groups. RCH NACKs. D_GROUP_LIST. Superframe position locked.
- Traffic channel at the centre (group 311, D scramble 105 then 106): calls, talking parties (two radios), D_GROUP_IDLE, D_CHANNEL_FREE, D_FORCED_LISTENING, D_START_SPEECH. With the control channel heard first, the calls show "encrypted, group 311".
- Two more traffic channels (+50 kHz, -100 kHz) and an idle carrier (-25 kHz, fill frames only).
- Nine real frames are in tests/tetrapol/test_tetrapol_rx.cpp as fixed test vectors (infrastructure messages only, no radio addresses), with the published D_SYSTEM_INFO frame of tetrapol-kit.

Simulated only:
- VHF coding, normal polarity, D_CALL_START / D_CALL_CONNECT key references on the traffic channel, clear calls and the codec path, PCH/RCH ACK layouts, mux type 1, disconnected-mode D_SYSTEM_INFO, and every message type above that the recording does not contain (datagrams, short data, emergency, registration).
- Impairment tests: noise sweep 6-20 dB, carrier offset ±900 Hz and ±3.1 kHz, clock ±60 ppm, drift -2.5 → +2.5 kHz over 8 s (100 % frames at 20 dB), an adjacent TETRAPOL carrier 12.5 kHz away at the same level (100 %), both polarities and band codings automatic and by hand, input rates 64 k to 2.4 M, retunes, noise only (no frames).

## Not done / open

- Channel numbers to frequencies: on the #408 network, channel 2801 is 383.4125 MHz and channel 2893 is 383.3125 MHz, which is not a simple raster. "Follow clear calls" still assumes one, so it needs the network's plan.
- Segmented UI TPDUs and connection-oriented TPDUs split over several HDLC frames are not reassembled. Their first frame is shown.
- Messages 0x97, 0x98 and 0x99 are not in the public PAS. They are shown as bytes.
- The GUI was not run (no app build allowed in this session). app/tetrapol/tetrapol_ui.cpp is syntax-checked only.
- test_tetrapol_engine was not run locally: it needs the engine built with the new telemetry layout. CI runs it.
