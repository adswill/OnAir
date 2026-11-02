# Multi-radio trunking (3.6)

Receive only. One radio sits on the control channel of a trunked system; as grants arrive the pool tunes the other open receivers to the voice channels, tracks each call, and keeps a log and a file per call. View > Multi-radio trunking. Core: `core/src/trunkpool/`, panel: `app/trunkpool/`, tests: `tests/trunkpool/`.

## Use

1. Open a P25 receiver on the control channel (RX 1), open more receivers (RX 2, RX 3 ... with + at the bottom) on other radios.
2. View > Multi-radio trunking, press **Auto-assign devices**. The P25 control radio stays on the control channel, every other running receiver becomes a voice radio.
3. The device table shows each radio's role (control, voice, idle, lost), the talkgroup and frequency it is on, and how many calls it took. The call log lists every call, followed or not, with the reason.

**One radio.** A grant inside the radio's capture is followed as a slice of it (the P25 follower decodes it, no retune). A grant outside the capture is not followed; the status line says "Control channel only: outside the capture ... add a second radio or widen the capture", once per channel in the message line.

Advanced (not shown in Easy mode): hang time, retune wait, priority pre-emption, record switch, follow unlisted talkgroups, talkgroup / radio-ID rules (allow, deny, priority), channel plan import, call log export. Turning the pool on hands the P25 follower's own voice radio and single-radio retune over to the pool (they are restored when it is turned off).

## Pool logic (TrunkPool)

- **Devices.** Any `VoiceReceiver`: present, canControl, capture centre and span, slices, tune, state, hearing voice, terminator, encrypted, release, audio. The app's adapter wraps RX n; the tests use fakes.
- **Allocation.** (1) a radio whose capture already holds the channel and has a free slice, (2) an idle voice radio with the capture centre nearest the channel (a radio that refuses the tune is skipped), (3) pre-emption: a call of strictly lower priority gives up its radio (emergency > talkgroup priority 1..10 > unlisted), (4) otherwise the call is logged "busy" or "outside the capture"; the repeat of its grant is a new chance in the same log row.
- **Call states.** tuning -> active (voice heard) -> hold (no voice) -> ended: hang time with no voice and no grant repeat, terminator, encrypted heard on the voice channel, no signal within the retune wait (the talkgroup is not tried for a few seconds), call too long (30 min), moved to another channel (followed again), pre-empted, device lost, radio switched off. A late grant for a call that just ended does not reopen it.
- **Device loss.** A radio that disappears ends its call ("device lost") and is marked lost; the next grant repeat goes to another radio. If the control radio disappears an idle capable radio is tuned to the control channel and promoted.
- **Retune latency** is measured per call (request to voice heard) and shown as mean and maximum.
- **Access rules.** Talkgroup and radio-ID ranges, allow / deny, priority 1..10, record flag. A radio-ID deny beats everything; later talkgroup rules win; the default is allow all or allow only listed. CSV: `Kind,From,To,Action,Priority,Record,Label`, or the RadioReference talkgroup export (Decimal, Alpha Tag, Mode: an E mode is denied).
- **Encrypted calls** (grant bit, or heard on the voice channel) are logged and never allocated, decoded or recorded. Decryption only with user-imported keys, which this feature does not touch.

## Recording

One 16 bit mono WAV and one JSON per call in `recordings/trunk/`: `<system>_tg<TG>_<YYYYMMDD-HHMMSS>_<id>.wav` + `.json` (talkgroup, source, frequency, slot, duration, audio seconds, setup seconds, device, end reason). A call with no sound leaves no WAV; the JSON is still written. Sound only exists when the voice codec the user installed is present (CODECS.md). In the app adapter there is no per-receiver audio tap yet (the engine's recorder and tap are single owner), so app calls get the JSON only; the recorder itself is tested with fake audio.

## Channel plan CSV

```
system,NSW PSN,0xBEE00,0x2D1,0x293
iden,1,851006250,12500,-45000000,12500        # id, base, spacing, tx offset, bandwidth[, slots]   (iden_up also accepted; MHz / kHz accepted)
chan,5,851.2                                  # logical channel number (DMR LCN, EDACS, LTR, MPT 1327)
```
or a table with a `Channel,Frequency` header. P25 grants use the plan until the site's own identifier table is heard.

## Protocols

| Protocol | Grant feed | State |
|---|---|---|
| P25 Phase 1 / 2 | live from the P25 follower (`Controller::grantTap`) | wired in the app; voice radios decode P25 Phase 1 / Phase 2 |
| DMR Tier III | `grantFromTrunk` (the existing model, opcodes unverified) | logic only, no live CSBK feed or voice radio in the app |
| DMR Capacity Plus / Connect Plus | `grantFromLcn` with the plan | logic only |
| TETRA | `grantTetra` (carrier, offset code) | logic only |
| MPT 1327 | `grantMpt1327` (channel number, spacing) | logic only |
| EDACS, LTR | `grantFromLcn` | logic only |

Remaining: app feeds and voice adapters for the non-P25 protocols, a per-receiver audio tap for recordings, wide voice radios as slice hosts in the app (the core supports them), a site map.

## Tests

`test_trunkpool_pool`: synthetic multi-channel harness (fake control channel, fake radios with retune latency, failure, unplug), allocation, retune latency, hang / terminator, busy and pick-up, priority and emergency pre-emption, access lists, encrypted, device loss, control promotion, single radio wide and narrow, no-signal back-off, recording files, CSV plan and rules, adapters, 4000 random steps.

Not verified: no real multi-SDR rig. The app adapter and window were written while builds were on hold (see the PATHWAY entry).
