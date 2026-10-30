# Trunking follower (P25, DMR Tier III)

Receive only. OnAir decodes a trunked system's control channel and can follow the voice calls it announces. Open the **P25** tab, **Follow**, and turn on **Follow calls**.

## What it does

- **System model.** From the control channel's TSBKs (TIA-102.AABC): WACN, system, RFSS, site, NAC, the channel identifier table (IDEN_UP, IDEN_UP_VU, IDEN_UP_TDMA), adjacent sites, secondary control channels (SCCB), and the control-channel candidates to try when it is lost.
- **Calls.** Group voice grants (new and update messages, including the explicit update), unit-to-unit grants. Each becomes a call-log entry with talkgroup, source unit, frequency, duration, and an encrypted flag.
- **Voice following.**
  - *Inside the capture*: with whole-span decoding off, a Phase 1 call gets a carrier decoder when it is granted, on its channel only, and the decoder goes when the call ends. Nothing is decoded on channels without a followed call. Up to N calls (1 to 4, default 2) are followed at once. A Phase 2 call runs as the second mode (see below). With whole-span decoding on, every carrier in the capture is decoded as before, and the sound moves to the followed call. When all are busy a higher-priority talkgroup preempts the lowest one.
  - *Single radio* (only with **retune the radio** on, which is off by default: leaving the control channel blinds the follower to every other grant): the radio leaves the control channel for the call, and returns when the call ends (no voice for the hang time, or a terminator). It does not hop away from a call that is already followed, and never hops when "retune the radio" is off. In both cases the entry says **needs a wider capture or a second radio** and counts as not followed.
  - *Voice radio* (two radios, issue #349): open a second receiver (RX 2, see multi-sdr.md), put it in **P25** or **P25 Phase 2**, and leave the **Voice radio** picker on the Follow tab on **Auto** (or pick that receiver; **Off** keeps one radio). Calls outside the control radio's capture go to it: it tunes to the granted channel (Phase 1 or Phase 2 with the slot and the site's WACN / system / NAC), plays the call, and goes back to the mode, frequency and sound it had when the call is over. The control radio never leaves the control channel and keeps decoding every grant. One call at a time on the voice radio: a second grant meanwhile is logged as not followed (**voice radio is on another call**) unless its talkgroup has a higher priority, which takes the radio over. A call ends by its terminator or after the hang time without voice (grant repeats on the control channel do not keep it open: only what the voice radio hears does). Encrypted calls (grant flag, talkgroup marked encrypted, or the voice channel's own encryption sync) are left at once. If the second radio is closed or stopped, or a call was never heard within 4 s (the retune failed, no signal), the call is logged as missed with the reason and the radio is left alone for 3 s before the next try; the control radio is not touched. Grants inside the control radio's capture still use the single-radio path. If no receiver can be used the row says why, and a button makes a second receiver a P25 receiver in one click. Only P25 Phase 1 and Phase 2 can be the voice radio; the other receiver's mode is restored after each call. Needs a test with two real radios (see the PATHWAY entry for what was tried).
  - If the control channel is lost the radio tries this site's own control channels in turn (its announced one and the SCCB secondaries; a neighbour's channel is placed with this site's identifier table, so neighbours are not tried). The radio stays 12 s on each: a retune restarts the receiver's acquisition, and a shorter dwell kept it from ever locking (issue #348).
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
  - The burst layer (ISCH, DUID, scrambling, voice and ESS positions) follows the decoder of XMC_SDR, and the superframe layout was read off a recording of the NSW PSN. On that recording the descrambler, ISCH and ESS give the same counts as XMC_SDR, with clear speech on the clear slot. The SACCH / FACCH MAC PDUs are decoded with the coding of the standard (a shortened and punctured RS(63,35) over GF(64) with erasure decoding, CRC-12, SACCH 21 bytes from all 156 payload dibits, FACCH 18 bytes from four segments of the burst), so the voice channel names the talker and talkgroup (MAC_PTT, MAC_END_PTT, MAC_ACTIVE user messages) without the grant. This was written from the public behaviour of OP25's p25p2_tdma.cc (structure only, no code taken) and checked against OnAir's own encoder; the TIA-102.BBAC-A text was not available and no recording with a real SACCH / FACCH was, so it needs a test on a real Phase 2 channel (`macAir` in the telemetry counts the PDUs decoded this way). OnAir's earlier private layout still decodes as a fallback. Talker alias is manufacturer specific and not decoded.
  - Followed Phase 1 hops are now kept open by their own voice too (before this they ended after the hang time, because no grant repeat reaches a hopped radio).
- **Message layouts** are written from the field layouts as understood from TIA-102.AABC, which is not available offline here; the explicit update (0x03) and SCCB (0x39) layouts in particular need a check against the document.
- **DMR Tier III**: the follow logic is shared, but the CSBK opcodes (`DmrOpcodes`: 0x30 / 0x31 / 0x32 for talkgroup / private / broadcast grants) and the grant field layout are **unverified against ETSI TS 102 361-4** and are kept in one table. The grants carry logical channel numbers only, so you must supply the LCN to frequency table (`DmrModel::setChannel`); there is no UI for it yet and the UI feeds only P25. Connect Plus and Capacity Plus are manufacturer protocols and are not supported.
- **Site map**: the control channel carries no coordinates; `SystemModel::setSitePosition` takes them from a user table, but there is no map view yet.
- **Transcription hook**: not wired. The call log entries carry the recording path for it.
- Only the selected carrier's sound plays, so two simultaneous followed calls are decoded and logged but one is heard (the highest priority).
- The Follow tab's text is not translated yet (like the rest of the P25 tab).

## Impairment tests

`test_p25_impair` (Phase 1 control channel + controller) and `test_p25p2_impair` (Phase 2 voice channel) run carrier offsets, off-centre channels, noise sweeps, simulcast echoes, fading, interference bursts, drop-outs and clock error through the real receivers; `P25_IMPAIR_TABLE=1` / `P25P2_IMPAIR_TABLE=1` prints the numbers. The Phase 2 receiver locks on a carrier up to 6 kHz off (before: about 700 Hz): the sync word is searched at +-2.4 kHz in 600 Hz steps, a spectrum look beyond that moves the front end (the symbol-rate detector aliases at 3 kHz), and the offset found is taken out at the front end. `test_p25_simulcast` sends the control channel from two transmitters with a delay (C4FM and a synthetic CQPSK): C4FM is fine up to 40 µs at equal strength, a synthetic CQPSK is not (see PATHWAY); real LSM signals need recordings. Synthetic signals only: simulcast (CQPSK) control channels and real fading need recordings.

## Tests

`test_p25_trunk` (system model, follow state machine, the voice radio with a fake second receiver (hand-off, preempt, hang, encrypted, failures), priority, lockout, scan lists, rotating control channel, CSV, encrypted handling, 20000 random messages and junk CSV), `test_p25_trunk_rf` (the synthetic control channel through the receiver into the controller), `test_p25p2_proto` / `test_p25p2_rx` (the Phase 2 burst layer, the scrambling against XMC_SDR's sequence, the ESS, scrambled and late-entry calls). `tools/p25p2tool` runs recordings: `p25p2tool cc <file> <rate>` lists a control channel's site, identifiers and grants, and `p25p2tool p2 <file> <rate> --wacn X --sys X --nac X --slot N --wav out.wav` decodes a Phase 2 voice channel.

## MPT 1327 and TETRAPOL (French networks)

**MPT 1327 framing.** The control channel receiver ("Framing" box on the MPT1327 screen) has three modes. *Standard*: a codeword starts only after the
SYNC word 0xC4D7 (or its complement). *Ignore sync word*: the sync word is not checked; any position where the last 64 bits have valid check bits is
taken as the end of a codeword, and while codewords stay good the next ones are read on the 64 bit grid (this is how TrunkView handles French
networks, whose sync word is not the standard one). A failed codeword drops back to the search. Without a sync word the tone polarity cannot be read from it (a codeword and its complement are both valid), so it is voted from bit 1: the mapping that gives more address codewords wins. *Automatic* (default): standard framing, and the
tolerant framing is switched on when no exact sync word has been seen after 8 s of audio. Cost of the tolerant mode: a random 64 bit pattern passes the
check about once in 130000 positions, so a false codeword is possible on noise; the control channel system identity is then read from bits 2 to 16 of a
codeword that ends in a sync-like word (4 bits wrong allowed). Test: `test_mpt1327` (foreign sync word).

**TETRAPOL.** The channel spacing box offers 10 kHz (France) and 12.5 kHz (e.g. Switzerland), and the arrows step by one channel. "Follow clear calls to
their traffic channel": the control channel announces a group call with a channel NUMBER (D_GROUP_ACTIVATION); OnAir shows the number only for clear calls
and follows only those. Enter the channel number of the control channel you are tuned to; the traffic channel is then at control frequency + (granted number -
control number) x spacing, the radio retunes there (it does not follow inside a wide capture) and returns to the control channel when the call has been
quiet for 6 s (8 s if nothing was heard). A network with a duplex gap or a non-contiguous plan needs the right control channel number or will land on a wrong
frequency. Voice: calls on French networks (and the Swiss ones) are normally encrypted, and encrypted calls are never followed, decoded or decrypted. A clear
call needs the user-installed TETRAPOL codec plug-in (RP-CELP is patented and is not shipped, see CODECS.md).
