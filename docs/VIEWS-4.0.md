# Radiosonde, wireless M-Bus and satellite framing entries (4.0, branch feat40-modes4)

Forty-seven more mode-list entries that are **views onto an existing receiver**, in the way 3.9 made entries for the CubeSat framings. The receiver is the same (same band search, same decoders, same screen); the entry lists one type only.
No decoder was added or changed. The sensor-family entries of the same branch are in `SENSORVIEWS-4.0.md`.

## Radiosonde types (receiver `SondeReceiver`, `setKindMask`)
| Mode | Family | Active std | Engine std | Poll slot | Sonde type |
|---|---|---|---|---|---|
| Radiosonde: Vaisala RS41 | 684 | 685 | 686 | 684 | RS41 |
| Radiosonde: Vaisala RS92 | 687 | 688 | 689 | 687 | RS92 |
| Radiosonde: Graw DFM | 690 | 691 | 692 | 690 | DFM-06 / 09 / 17 |
| Radiosonde: Meteomodem M10 | 693 | 694 | 695 | 693 | M10 / M10+ |
| Radiosonde: Meteomodem M20 | 702 | 703 | 704 | 702 | M20 |
| Radiosonde: Lockheed Martin LMS6 | 705 | 706 | 707 | 705 | LMS6 (1680 MHz) |
| Radiosonde: InterMet iMet-4 | 708 | 709 | 710 | 708 | iMet-1 / iMet-4 |
| Radiosonde: InterMet iMet-54 | 711 | 712 | 713 | 711 | iMet-54 |
| Radiosonde: Russian MRZ | 714 | 715 | 716 | 714 | MRZ (MP3-H1) |
| Radiosonde: Meisei iMS-100 | 717 | 718 | 719 | 717 | iMS-100 |

`telemetry()` keeps the sondes whose `SondeKind` bit is set in the mask and recomputes the frame counts from them; the carrier list is not filtered. Uploads and the saved results still see every sonde.
Test: `tests/sonde/test_sonde_views.cpp` (simulated flights of `sonde_gen`: five types together at 28 dB with a +2.5 kHz offset and at 24 dB with -4 kHz and 8-bit samples; iMet, iMet-54, MRZ and iMS-100 one at a time with offsets of 0 / +20 / -20 kHz; noise only):
a view lists exactly the sondes of its type that the receiver without a view lists, the counts are the sums over them, and a view of a type that is not on the air is empty. **Simulated signals only**; no recording of iMet, iMet-54, MRZ, iMS-100 or LMS6 exists in the repository, LMS6 is checked for the table and the mask only.
CI result: see the pathway entry.

## Wireless M-Bus modes (receiver `WmbusLiveReceiver`, `setPhyMask`)
| Mode | Family | Active std | Engine std | Poll slot | Modes of `wmbus::Phy` | Default MHz |
|---|---|---|---|---|---|---|
| Wireless M-Bus S1 | 720 | 721 | 722 | 720 | S1 | 868.625 (868 MHz plan) |
| Wireless M-Bus T1 | 723 | 724 | 725 | 723 | T1 | 868.625 |
| Wireless M-Bus C1 | 726 | 727 | 728 | 726 | C1A, C1B | 868.625 |
| Wireless M-Bus R2 | 732 | 733 | 734 | 732 | R2 | 868.625 |
| Wireless M-Bus N1 (Wize, 169 MHz) | 735 | 736 | 737 | 735 | N1, N24 | 169.4375 (169 MHz plan) |

The receiver still searches every channel; `telemetry()` lists the meters and frame log lines of the selected modes and recounts. Test: `tests/wmbus/test_wmbus_views.cpp` (the simulated meters of `wmbus_gen` on the 868 MHz plan clean, with -12 kHz at 25 dB and +9 kHz at 20 dB, and on the 169 MHz plan clean and with +3 kHz at 22 dB; noise only):
a view lists exactly the meters and log lines of its modes that the receiver without a view lists, the views together list every meter, and noise gives nothing. **Simulated signals only.**

## Satellite framings (receiver `CubesatReceiver`, `setFramingMask`)
Three more of the framings that only ran inside CubeSat telemetry: AX.25 G3RUH 9600 (family 792), CCSDS Reed-Solomon (795), CCSDS concatenated, Viterbi + Reed-Solomon (804). Active standard family + 1, engine standard family + 2, poll slot = family.
`tests/cubesat/test_cubesat_views.cpp` now expects nine views and runs the engine entry of each (its own generated signal, decoded as that framing and no other). **Generated signals only.**

Numbers come from the free ranges of `docs/FAMILY-NUMBERS-4.0.md` (663 - 697, 702 - 727, 732 - 738, 792 - 797, 804 - 817); the integrator resolves any clash with another branch.

## AIS station kinds (receiver `AisReceiver`, `setStationView`)
| Mode | Family | Active std | Engine std | Poll slot | Stations shown |
|---|---|---|---|---|---|
| AIS Class A vessels | 738 | 739 | 740 | 738 | Class A |
| AIS Class B vessels | 807 | 808 | 809 | 807 | Class B |
| AIS base stations | 810 | 811 | 812 | 810 | base stations (message 4) |
| AIS aids to navigation (AtoN) | 813 | 814 | 815 | 813 | aids to navigation (message 21) |
| AIS search and rescue aircraft | 816 | 817 | 818 | 816 | SAR aircraft (message 9) |
| AIS-SART, MOB and EPIRB-AIS | 857 | 858 | 859 | 857 | MMSI 970xxxxxx, 972xxxxxx, 974xxxxxx |

`telemetry()` keeps the vessels of the class (or the device MMSIs) and, with them, the message records, application messages, alerts, anomalies and sentences of those stations; the counts are recomputed from the vessels' message counts.
Test `tests/ais/test_ais_views.cpp`: the simulated port of `ais_gen` (30 s, clean at 34 dB and with +3 kHz at 26 dB) through one receiver per view and one without: each view lists exactly the stations of its class that the receiver without a view lists (19 stations in all, every class present),
bursts of position reports of ships and of MMSI 970123456 / 972123456 / 974123456 (carrier errors of +-800 Hz, 28 dB): the device view lists exactly the three devices; noise only: nothing. **Simulated signals only**, no recording of an AIS-SART was available.

## Time stations (receiver `TimeReceiver`, station fixed by the entry)
Families 607, 610, 613, 616, 628, 631, 634, 637, 642, 645, 648 (active standard family + 1, engine standard family + 2, poll slot = family): DCF77, MSF, WWVB, JJY 40 kHz, JJY 60 kHz, WWV / WWVH, BPM, BPC, HLA, BPM coded and RWM; CHU is not offered (removed in the screen, #150).
The engine fixes the station when the entry starts (`setStation`), the Time signals screen keeps it (`time_ui.cpp`), and the default frequency is the station's. The clock sync section stays in Time signals.
Test `tests/time/test_time_views.cpp`: the test signal of each station (175 s at 64 ksps, 30 dB, carrier errors of +6 / -4 Hz, sample clock errors of +8 / -12 ppm, started part way into a minute) into a receiver fixed to it: the station is right, and for DCF77, MSF, WWVB, JJY 40, JJY 60, WWV and BPM the decoded time is right with no rejected minute;
BPC, HLA, BPM coded and RWM are checked for the station only (their decoding is covered by `test_time_burst` and `test_time_rwm`). A DCF77 signal in the MSF entry, WWVB in the DCF77 entry and JJY 60 in the WWVB entry give no time; noise alone gives none in any entry. **Simulated signals only.**

## Satellite systems (receiver `GnssReceiver`, `setSystems`)
Galileo is not offered on its own: the receiver acquires Galileo E1-B in the GPS band with the help of a GPS lock (its search window comes from the GPS frequency error), so a Galileo-only entry would find nothing.
| Mode | Family | Active std | Engine std | Poll slot | System | Default MHz |
|---|---|---|---|---|---|---|
| GPS L1 C/A | 860 | 861 | 862 | 860 | GPS | 1575.42 |
| QZSS L1 | 866 | 867 | 868 | 866 | QZSS | 1575.42 |
| GLONASS L1 | 872 | 873 | 874 | 872 | GLONASS | 1602.0 (10 Msps) |
| BeiDou B1I | 875 | 876 | 877 | 875 | BeiDou | 1561.098 |

The engine passes `gnssSystemBit(system)` to `setSystems` when the entry starts (all systems for GNSS itself). The screen is the GNSS screen; the drive-navigation, map and clock-sync parts stay in GNSS.
Test `tests/gnss/test_gnss_views.cpp`: a simulated L1 sky with GPS, Galileo and QZSS satellites (4 Msps, 8-bit samples, 45 s) through a receiver wanting every system and one for the GPS and one for the QZSS view: a view locks none of the other systems and enough of its own;
the tables of all four (GLONASS at 10 Msps, BeiDou at 1561.098 MHz) are checked; GLONASS and BeiDou use the same mask and their signals are covered by the receiver's own tests. **Simulated signals only.**

## Analog TV standards (receiver `AtvReceiver`, `setStandard`)
| Mode | Family | Active std | Engine std | Poll slot | System, colour |
|---|---|---|---|---|---|
| Analog TV: PAL B/G | 424 | 425 | 426 | 424 | B/G (8 MHz channel), PAL |
| Analog TV: PAL I | 427 | 428 | 429 | 427 | I, PAL |
| Analog TV: PAL D/K | 441 | 442 | 443 | 441 | D/K, PAL |
| Analog TV: SECAM D/K | 444 | 445 | 446 | 444 | D/K, SECAM |
| Analog TV: SECAM L | 447 | 448 | 449 | 447 | L, SECAM |
| Analog TV: NTSC M | 450 | 451 | 452 | 450 | M, NTSC |
| Analog TV: PAL M | 453 | 454 | 455 | 453 | M, PAL |
| Analog TV: PAL N | 456 | 457 | 458 | 456 | N, PAL |

The engine tells the receiver the system and the colour system when the entry starts (`setStandard`); the Analog TV screen keeps them (`atv_ui.cpp` pushes the entry's standard instead of the user's hint), and the main screen's picture view works for the entries too (`draw_ui.cpp`). The default frequency is that of Analog TV (600 MHz).
Test `tests/atv/test_atv_views.cpp`: the test signal of each standard (colour bars, 40 dB, 10 Msps, 8-bit samples, carrier error 0 and +-50 kHz) into a receiver told what the entry tells it: line and field lock, the standard named right, the lines and the field rate of the standard, the offset found, the decoded colour bars within 0.14 of the EBU bars.
**Simulated signals only**; the behaviour of a forced standard on a signal of another standard is not tested.
