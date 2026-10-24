# Trunking follower (P25, DMR Tier III)

Receive only. OnAir decodes a trunked system's control channel and can follow the voice calls it announces. Open the **P25** tab, **Follow**, and turn on **Follow calls**.

## What it does

- **System model.** From the control channel's TSBKs (TIA-102.AABC): WACN, system, RFSS, site, NAC, the channel identifier table (IDEN_UP, IDEN_UP_VU, IDEN_UP_TDMA), adjacent sites, secondary control channels (SCCB), and the control-channel candidates to try when it is lost.
- **Calls.** Group voice grants (new and update messages, including the explicit update), unit-to-unit grants. Each becomes a call-log entry with talkgroup, source unit, frequency, duration, and an encrypted flag.
- **Voice following.**
  - *Inside the capture*: with whole-span decoding off, a Phase 1 call gets a carrier decoder when it is granted, on its channel only, and the decoder goes when the call ends. Nothing is decoded on channels without a followed call. Up to N calls (1 to 4, default 2) are followed at once. A Phase 2 call runs as the second mode (see below). With whole-span decoding on, every carrier in the capture is decoded as before, and the sound moves to the followed call. When all are busy a higher-priority talkgroup preempts the lowest one.
  - *Single radio* (only with **retune the radio** on, which is off by default: leaving the control channel blinds the follower to every other grant): the radio leaves the control channel for the call, and returns when the call ends (no voice for the hang time, or a terminator). It does not hop away from a call that is already followed, and never hops when "retune the radio" is off. In both cases the entry says **needs a wider capture or a second radio** and counts as not followed.
  - If the control channel is lost the radio tries the known candidates in turn.
- **Talkgroup list.** Name, tag, category, priority (1 highest, 0 none), lockout, listen, scan list. Lockout talkgroups are logged but not followed. With **scan list only** only listed talkgroups with Listen on (and in an enabled list, when any list is enabled) are followed. The list is saved to `trunk_talkgroups.csv` in the OnAir data folder.
- **Call log** with CSV export, **now talking** table, per-call recording (WAV, gated) when the vocoder add-on is present.

## Encrypted calls

P25 service options bit `0x40`, or a talkgroup you marked with an `E` mode in the list, and DMR privacy flags: the call is logged as *encrypted* and is never followed, recorded, decoded or attacked. IDs and talkgroups are still shown.

## Voice

Phase 1 and Phase 2 calls share the **P25** page of the Voice monitor: one list, one call played at a time, the same allow / mute / priority lists and recordings. A followed Phase 2 call is listed with the talkgroup and unit from its grant (the voice channel itself does not name them), and an encrypted one as encrypted.

Voice sound needs the user-installed vocoder add-on (see [CODECS.md](CODECS.md)). Without it everything else works; recordings are skipped because there is no sound to save.

## CSV columns

Import reads the header row, case-insensitive, any column order: `Decimal` (also `DEC`, `TGID`, `Talkgroup`), `Hex`, `Alpha Tag` (`Name`), `Mode` (D, DE, T, A), `Description`, `Tag`, `Category` (`Group`), and the OnAir extras `Priority`, `Lockout`, `List`, `Listen`. The usual RadioReference-style export (Decimal, Hex, Alpha Tag, Mode, Description, Tag, Category) imports as it is; without a header that order is assumed. Quoted fields, doubled quotes, LF/CRLF and a UTF-8 BOM are accepted. A row without a valid number (1 to 65535) is skipped and counted. OnAir downloads no data: you bring the file.

## Honest scope

- **P25 Phase 1 control channel**: implemented and tested with synthetic signals (through the real C4FM receiver, and as message logic), and decoded from a recording of the NSW PSN (WACN BEE00, system 2D1): site, adjacent sites, identifiers and grants.
- **P25 Phase 2**: the control channel is Phase 1 (TSBKs); IDEN_UP_TDMA is parsed (only channel type 3, 2 slots per carrier, is placed; other types are shown as not placed). A Phase 2 grant gives the carrier and the slot. The follower opens the P25 Phase 2 receiver on it and seeds the descrambler with the WACN, system and NAC from this control channel. Phase 2 bursts are scrambled with that seed, so the Phase 2 mode on its own decodes nothing until the seed is typed in.
  - *Wide capture*: one Phase 2 call at a time runs as the **second mode** beside the control channel (a "Second: P25 Phase 2" tab appears). A second Phase 2 grant inside the span is logged as busy unless it has a higher priority.
  - *Single radio*: the radio hops to the carrier and the engine switches to the Phase 2 receiver without restarting the radio. Voice on the slot keeps the call open, and the radio goes back to the control channel (and to Phase 1) after the hang time.
  - Encryption comes from the voice channel's encryption sync (ESS). Grants are no guide: the NSW PSN grants AES calls without the encryption bit. A call the ESS calls encrypted is marked encrypted and left. Voice heard before the first ESS (up to one 360 ms superframe) is held, then played or dropped.
  - The burst layer (ISCH, DUID, scrambling, voice and ESS positions) follows the decoder of XMC_SDR, and the superframe layout was read off a recording of the NSW PSN. On that recording the descrambler, ISCH and ESS give the same counts as XMC_SDR, with clear speech on the clear slot. The SACCH / FACCH MAC PDUs are still OnAir's own layout and are not expected to decode on a real channel. The voice channel therefore does not name the talker: the call log has it from the grant.
  - Followed Phase 1 hops are now kept open by their own voice too (before this they ended after the hang time, because no grant repeat reaches a hopped radio).
- **Message layouts** are written from the field layouts as understood from TIA-102.AABC, which is not available offline here; the explicit update (0x03) and SCCB (0x39) layouts in particular need a check against the document.
- **DMR Tier III**: the follow logic is shared, but the CSBK opcodes (`DmrOpcodes`: 0x30 / 0x31 / 0x32 for talkgroup / private / broadcast grants) and the grant field layout are **unverified against ETSI TS 102 361-4** and are kept in one table. The grants carry logical channel numbers only, so you must supply the LCN to frequency table (`DmrModel::setChannel`); there is no UI for it yet and the UI feeds only P25. Connect Plus and Capacity Plus are manufacturer protocols and are not supported.
- **Site map**: the control channel carries no coordinates; `SystemModel::setSitePosition` takes them from a user table, but there is no map view yet.
- **Transcription hook**: not wired. The call log entries carry the recording path for it.
- Only the selected carrier's sound plays, so two simultaneous followed calls are decoded and logged but one is heard (the highest priority).
- The Follow tab's text is not translated yet (like the rest of the P25 tab).

## Tests

`test_p25_trunk` (system model, follow state machine, priority, lockout, scan lists, rotating control channel, CSV, encrypted handling, 20000 random messages and junk CSV), `test_p25_trunk_rf` (the synthetic control channel through the receiver into the controller), `test_p25p2_proto` / `test_p25p2_rx` (the Phase 2 burst layer, the scrambling against XMC_SDR's sequence, the ESS, scrambled and late-entry calls). `tools/p25p2tool` runs recordings: `p25p2tool cc <file> <rate>` lists a control channel's site, identifiers and grants, and `p25p2tool p2 <file> <rate> --wacn X --sys X --nac X --slot N --wav out.wav` decodes a Phase 2 voice channel.
