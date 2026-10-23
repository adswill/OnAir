# Mode numbers

Every mode has three numbers that must be unique:

- **family**: the mode's row in the mode list (`app/ui/mode_table.cpp`, `App::family`, `kNumFamilies` in `app/app.h`);
- **engine standard** = family + 2 (`Engine::setStandard`, `ModeTuning::stdMode`, `makeModeSynth`, the TX registry);
- **active standard** = engine standard - 1 = family + 1 (`Engine::activeStandard`, `RxTelemetry::standard`, reset / configure /
  poll / summary switches);

plus, for a receiver that reports telemetry, a **poll slot** (`seqs[N]` in `core/src/engine/engine_telemetry.cpp`, below
`Engine::kPollSeqSlots`). Plugin modes start at family 999 (engine standard 1001+); the Mode Builder row is 19999.

`tests/app/test_mode_numbers.cpp` (ctest `mode_numbers`) fails on a duplicate family, a duplicate engine standard in the registry,
a poll slot used by two receivers, or a number outside `kNumFamilies` / `kPollSeqSlots`. `test_mode_numbers --markdown` prints the
full table of the current tree.

Settings are kept per family under the family number (`app_state.cpp` slot keys) and per protocol entry under its name. None of
the modes renumbered for 3.0 was ever released, so no saved setting moves.

## Allocations of 3.0

Several branches picked numbers on their own; the later ones were moved (integ-30).

| mode | family | engine std | active std | poll slot | branch | note |
|---|---|---|---|---|---|---|
| HF waveform ID | 145 | 147 | 146 | 135 | (0.2.9) | |
| KLW (railway end of train) | 146 | 148 | 147 | 147 | (0.2.9, PR #231) | released, kept |
| TDCS (railway dispatch) | 147 | 149 | 148 | 148 | (0.2.9, PR #231) | released, kept |
| NFC / RFID 13.56 MHz | 148 | 150 | 149 | 141 | mode-nfc-030 | was family 146 |
| SBAS | 150 | 152 | 151 | 150 | mode-sbas-radar-030 | |
| ASTERIX | 151 | 153 | 152 | - | mode-sbas-radar-030 | no radio receiver |
| Argos | 152 | 154 | 153 | 143 | mode-argos-sigfox-030 | was family 148 |
| Sigfox | 153 | 155 | 154 | 144 | mode-argos-sigfox-030 | was family 149 |
| ISDB-S (BS) | 154 | 156 | 155 | 154 | mode-isdbs-030 | |
| Marine VHF voice | 155 | 157 | 156 | 142 | vhf-marine-030 | was family 148, slot 141 |
| Horus balloons | 156 | 158 | 157 | 157 | horus-030 | was family 148 |
| P25 Phase 2 | 180 | 182 | 181 | 181 | mode-p25-edacs-030 | |
| LoRaWAN (all channels) | 200 | 202 | 201 | 160 | mode-lora-030 | |
| LoRa satellites | 201 | 203 | 202 | 161 | mode-lora-030 | |
| DME | 121 | 123 | 122 | 162 | (0.2.9) | poll slot was 110, shared with DGPS: moved in 3.0 |
| PSK demodulator (BPSK / QPSK analyser) | 202 | 204 | 203 | 163 | issue-modes-030 | #282 |
| PACTOR-I (monitor) | 203 | 205 | 204 | 164 | issue-modes-030 | #216 |
| Plugins | 999 | 1001 | - | - | | |
| Mode Builder | 19999 | - | - | - | mode-builder-030 | |
| custom modes (Mode Builder) | 20000+ | family + 2 | - | - | mode-builder-030 | kCustomFamilyBase, saved by number in modes/registered.txt |

Protocol entries (a row with a subId, such as FT8 in HF digital or POCSAG in pagers) share their family on purpose; the test checks
that each (family, subId) pair is unique instead.

`kNumFamilies` = 204, `kPollSeqSlots` = 182.

A new mode takes the next free family above the highest one in this table and the next free poll slot, and adds its row here.
