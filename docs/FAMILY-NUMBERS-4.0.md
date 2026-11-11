# Family numbers used by the 4.0 modes lane (feat40-modes1)

The brief gave families 900 to 999, but `kNumFamilies` is 900 and `kPollSeqSlots` is 901 (the poll slot of a mode is its family number), and 999 is the Plugins row.
This branch therefore uses the free block **881 to 899**, with the same layout as the aviation, rail and road receiver: one shared receiver (`X40Receiver`) and one
number per mode: family F, active standard F + 1, engine standard F + 2, poll slot F. Neither constant changed.

| Mode | Family | Active standard | Engine standard | Poll slot |
|---|---|---|---|---|
| RC model PPM (72 MHz) | 881 | 882 | 883 | 881 |
| UVB-76 buzzer monitor | 882 | 883 | 884 | 882 |
| GRAVES meteor echoes | 883 | 884 | 885 | 883 |
| Radio Jove bursts | 884 | 885 | 886 | 884 |

Free for further modes of this receiver: families 885 to 896 (`x40::kNumVariants` is 4; the active-standard range 882 to 897 is already routed in `Engine::feedReceiver`).
Families 897 to 899 stay free. A mode needing numbers above 900 needs the integrator to raise `kNumFamilies` and `kPollSeqSlots` (one deliberate change).
Merge note: the integrator should rerun `python3 tools/gen_modes_list.py`, `tools/gen_web_schema.py`, `tools/gen_web_options.py`, `android/tools/gen_modes.py` and `android/tools/gen_results_map.py`
after merging, because the generated files of this branch and another branch conflict by construction.
