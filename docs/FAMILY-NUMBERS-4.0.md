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
| Beacon propagation monitor | 885 | 886 | 887 | 885 |
| EM4100 LF RFID (125 kHz) | 886 | 887 | 888 | 886 |
| UHF RFID reader (EPC Gen2) | 887 | 888 | 889 | 887 |
| Qi wireless charging | 888 | 889 | 890 | 888 |
| FM modulation monitor | 889 | 890 | 891 | 889 |
| AM modulation monitor | 890 | 891 | 892 | 890 |
| Power-line noise locator | 891 | 892 | 893 | 891 |
| Radar pulse analyser | 892 | 893 | 894 | 892 |
| GNSS interference detector | 893 | 894 | 895 | 893 |
| Passband signal classifier | 894 | 895 | 896 | 894 |
| VLF MSK transmitter monitor | 895 | 896 | 897 | 895 |

Lane C (feat40-modes2) used families 886 to 895 (`x40::kNumVariants` is 15; the engine switches use `DECT2_X40_ACTIVE_CASES` for the active standards 882 to 897). Free for further modes of this receiver: family 896 (the last number the receiver's active-standard range 882 - 897 allows). Families 897 to 899 stay free. A mode needing numbers above 900 needs the integrator to raise `kNumFamilies` and `kPollSeqSlots` (one deliberate change).
Merge note: the integrator should rerun `python3 tools/gen_modes_list.py`, `tools/gen_web_schema.py`, `tools/gen_web_options.py`, `android/tools/gen_modes.py` and `android/tools/gen_results_map.py`
after merging, because the generated files of this branch and another branch conflict by construction.

## Usable numbers below kNumFamilies = 900 (feat40-modes3, 2026-11-12)

The "next free is above the highest" rule was too strict: there are many gaps below 881. A family F is usable when F is not a mode-table family, F + 1 and F + 2 (active and engine standard) are not taken by any registry entry, and the poll slot F (and F + 1) is not taken by a receiver. A script that follows `tests/app/test_mode_numbers.cpp` (families from `app/ui/mode_table.cpp`, constants from every `*_modes.h`, poll slots from `engine_telemetry.cpp`, explicit `.stdMode =` values) found 406 usable numbers from 100 up. Ranges:

137 ; 158 ; 166-177 ; 214-216 ; 247-257 ; 272-287 ; 312-317 ; 324-337 ; 347-357 ; 362-377 ; 382-397 ; 412-427 ; 441-457 ; 467-477 ; 482-497 ; 504-507 ; 511-527 ; 564-597 ; 607-617 ; 628-637 ; 642-657 ; 663-697 ; 702-727 ; 732-738 ; 743 ; 752-787 ; 792-797 ; 804-817 ; 837 ; 857-867 ; 872-877 ; 896-899

Some of these are blocks another receiver reserved by arithmetic (for example the hamsci families 820 + 3k leave 821 / 822 unused on purpose, the chanvoice block 340-346): before using a range, run `test_mode_numbers` after adding the row. Because 406 numbers are free, **`kNumFamilies` and `kPollSeqSlots` do not need to change** for 20 or more new modes, so no `FAMILY-LIMIT-4.0.md` was written.

### Third block of the shared x40 receiver: families 752 to 777 (this branch, feat40-modes3)

The x40 receiver now maps a variant to a family through `x40::familyOfVariant(v)`: variants 0 - 14 keep 881 + v; variants 15 and up take 752, 753 ... (active standard family + 1, engine standard family + 2, poll slot = family). 752 - 787 (36 numbers) are reserved for this block; the macro `DECT2_X40_ACTIVE_CASES` lists the active standards in use.

| Mode | Family | Active standard | Engine standard | Poll slot |
|---|---|---|---|---|
| Spectrum occupancy monitor | 752 | 753 | 754 | 752 |
| Modulation quality (MER) meter | 753 | 754 | 755 | 753 |
| Carrier frequency accuracy meter | 754 | 755 | 756 | 754 |
| PTT and duty-cycle logger | 755 | 756 | 757 | 755 |
| Intermodulation finder | 756 | 757 | 758 | 756 |
| Spur and image finder | 757 | 758 | 759 | 757 |
| Satellite Doppler curve logger | 758 | 759 | 760 | 758 |
| Frequency-hopper analyser | 759 | 760 | 761 | 759 |
| Persistent carrier (birdie) finder | 760 | 761 | 762 | 760 |
| Burst finder | 761 | 762 | 763 | 761 |
| Fading analyser | 762 | 763 | 764 | 762 |
| SDR input health meter | 763 | 764 | 765 | 763 |
| Phase noise meter | 764 | 765 | 766 | 764 |
| SSB transmitter test meter | 765 | 766 | 767 | 765 |
| FM SINAD meter | 766 | 767 | 768 | 766 |
| FSK parameter analyser | 767 | 768 | 769 | 767 |
| Noise figure (Y-factor) meter | 768 | 769 | 770 | 768 |
| CW keying quality meter | 769 | 770 | 771 | 769 |
| Noise floor logger | 770 | 771 | 772 | 770 |
| Narrowband FM deviation and CTCSS meter | 771 | 772 | 773 | 771 |
| AFSK (TNC) tone tuner | 772 | 773 | 774 | 772 |
| OOK / ASK pulse analyser | 773 | 774 | 775 | 773 |
| Transmitter frequency stability meter | 774 | 775 | 776 | 774 |
| Crest factor (PAPR / CCDF) meter | 775 | 776 | 777 | 775 |
| Occupied bandwidth meter | 776 | 777 | 778 | 776 |
| Spectral kurtosis RFI finder | 777 | 778 | 779 | 777 |
| SDR stream glitch detector | 778 | 779 | 780 | 778 |

Families 779 - 787 stay reserved for further modes of this block. A new variant needs: the enum entry and the active-standard `case` label in `x40_rx.h`, `makeX()` in `x40_int.h` / `x40_dec.cpp`, the tuning row in `x40_dec.cpp`, the name in `x40_rx.cpp`, `x40Tuning(v)` in the registry list of `core/src/engine/modes.cpp`, the table sizes and `makeUi<v>()` in `app/x40/x40_ui.cpp`, the `case` in `app/panels/modeui.cpp` and the mode-table row (`x40::kNumVariants` and the table arrays in `x40_dec.cpp`, `x40_rx.cpp`, `app/x40/x40_ui.cpp` and `app/panels/modeui.cpp` grow with it).

## Numbers taken by feat40-modes4 (2026-11-12)
Every entry is a family F with active standard F + 1, engine standard F + 2 and poll slot F; none needs `kNumFamilies` or `kPollSeqSlots` to change. Tables of the entries: `docs/X40-4.0.md`.

The other rows of this block were entries that showed one type of an existing receiver. They are not in the mode list; the receivers' screens filter by type instead.
Their families and ids are **retired**: `core/include/dect2/engine/retired_modes.h` maps a saved one to its receiver (desktop settings, workspaces, presets, memories, scan lists, Android indexes), so never give one of these numbers or ids to a new mode.

| Block | Families | What |
|---|---|---|
| x40 receiver, fourth block | 779, 780, 781 | ILS marker beacons, FSK modems, SSR interrogations (variants 42 - 44; 782 - 787 stay free for this block) |
| Retired: Sub-GHz sensor families and kinds | 564, 567, 570, 573, 576, 579, 582, 585, 588, 591, 594, 663, 666, 669, 672, 675, 678, 681, 412, 415, 418, 421 | 22 views of Sub-GHz sensors |
| Retired: Radiosonde types | 684, 687, 690, 693, 702, 705, 708, 711, 714, 717 | 10 views of Radiosonde |
| Retired: Wireless M-Bus modes and manufacturers | 720, 723, 726, 732, 735, 743, 797, 467, 470 | 9 views of Wireless M-Bus |
| Retired: CubeSat framings | 792, 795, 804 | 3 views of CubeSat telemetry |
| Retired: AIS station kinds | 738, 807, 810, 813, 816, 857 | 6 views of AIS |
| Retired: NFC protocols | 473, 476, 482 | 3 views of NFC / RFID 13.56 MHz |
| Retired: Time stations | 607, 610, 613, 616, 628, 631, 634, 637, 642, 645, 648 | 11 views of Time signals |
| Retired: GNSS systems | 860, 866, 872, 875 | 4 views of GNSS |
| Retired: Analog TV standards | 424, 427, 441, 444, 447, 450, 453, 456 | 8 views of Analog TV |

The numbers still free are those of the ranges listed above minus the ones in this table, retired ones included (1 for an x40 variant). The integrator resolves any clash with another branch.

## DCR (feat40-dcr, 2026-11-12)
x40 variant 45: family 782, active standard 783, engine standard 784, poll slot 782 (DCR, Japan digital convenience radio, docs/DCR-4.0.md). The x40 variants after SsrInt (44) are variants 45 and up: a merge with another branch that also adds a variant needs the enum order, `kX40Ui[]`, `makeUi<>()` and the `x40Tuning(n)` list settled by hand; `DECT2_X40_ACTIVE_CASES` now ends in 783.

## Rivet HF modes (feat40-rivet, 2026-11-12)
x40 variants 46 - 51 (after Dcr), a block of their own: `x40::familyOfVariant(v)` gives family 370 + (v - Xpa) for them (`kRivetFirst`, `kRivetFamily`), active standard family + 1, engine standard family + 2, poll slot = family. 370 - 375 come from the free range 362 - 377; 376 and 377 stay free (as active / engine standards they are taken by 375). Docs: docs/RIVET-MODES-4.0.md.

| Mode | Variant | Family | Active standard | Engine standard | Poll slot |
|---|---|---|---|---|---|
| XPA (10 / 20 Bd) | 46 | 370 | 371 | 372 | 370 |
| XPA2 | 47 | 371 | 372 | 373 | 371 |
| CROWD36 | 48 | 372 | 373 | 374 | 372 |
| FSK200/500 (F01) | 49 | 373 | 374 | 375 | 373 |
| FSK200/1000 (F06, F06a) | 50 | 374 | 375 | 376 | 374 |
| GW FSK (100 Bd) | 51 | 375 | 376 | 377 | 375 |

Merge note: another branch that adds x40 variants after Dcr (45) needs the enum order, `kX40Ui[]`, `makeUi<>()`, the `x40Tuning(n)` list and the case labels in `app/panels/modeui.cpp` (`&kX40Ui[46 + family - 370]`) settled by hand; the Rivet block keeps its families whatever its variant numbers become, because `kRivetFirst` is the enum value `Xpa`.
