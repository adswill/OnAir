# Sensor-family entries (4.0, branch feat40-modes4)

Twenty-two mode-list entries that are **views onto the Sub-GHz sensors receiver** (`core/src/sensors`, the rtl_433 decoders in `third_party/rtl_433`): the same pulse detector, slicers and device decoders, a device list that shows one family only.
The Sub-GHz sensors entry is unchanged and still lists every sensor and has the unknown signal analyser. No decoder was added or changed: what each entry decodes is what rtl_433's decoders of that family decode (rtl_433 is GPL-2.0-or-later, see `THIRD_PARTY.md`).

| Mode | Family | Active std | Engine std | Poll slot | Shows models containing | Default MHz |
|---|---|---|---|---|---|---|
| Oregon Scientific sensors | 564 | 565 | 566 | 564 | oregon | 433.92 |
| Acurite sensors | 567 | 568 | 569 | 567 | acurite | 433.92 |
| LaCrosse sensors | 570 | 571 | 572 | 570 | lacrosse | 433.92 |
| Fine Offset / Ecowitt sensors | 573 | 574 | 575 | 573 | fineoffset, fine offset, ecowitt | 868.3 |
| Bresser sensors | 576 | 577 | 578 | 576 | bresser | 868.3 |
| Hideki sensors | 579 | 580 | 581 | 579 | hideki | 433.92 |
| TFA Dostmann sensors | 582 | 583 | 584 | 582 | tfa, klimalogg | 433.92 |
| Nexus / Prologue sensors | 585 | 586 | 587 | 585 | nexus, prologue | 433.92 |
| Alecto / Esperanza sensors | 588 | 589 | 590 | 588 | alecto, esperanza | 433.92 |
| Ambient Weather sensors | 591 | 592 | 593 | 591 | ambient | 915 |
| Wireless alarm sensors | 594 | 595 | 596 | 594 | honeywell, visonic, dsc, interlogix | 345 |
| X10 RF | 663 | 664 | 665 | 663 | x10 | 433.92 |
| Itron ERT meters | 666 | 667 | 668 | 666 | ert-scm, scmplus, idm (the models ERT-SCM, SCMplus, IDM and NETIDM) | 915 |
| Neptune R900 water meters | 669 | 670 | 671 | 669 | neptune | 915 |
| TPMS: Schrader | 672 | 673 | 674 | 672 | schrader, schraeder | 433.92 |
| TPMS: Renault | 675 | 676 | 677 | 675 | renault | 433.92 |
| TPMS: Toyota | 678 | 679 | 680 | 678 | toyota, pmv-107j | 433.92 |
| TPMS: Citroen / Peugeot / Fiat | 681 | 682 | 683 | 681 | citroen, peugeot, fiat, mitsubishi | 433.92 |

Four more entries show a **kind** of sensor of every brand (the kind the Sub-GHz sensors list assigns from a message's fields, `kindOf` in `sensors_rx.cpp`, written `kind:...` in the patterns): Weather stations, all brands (412), TPMS: all makes (415), Utility and energy meters (418), Remotes, doorbells and switches (421); defaults 433.92 MHz.
Rolling-code remotes are listed as the frames they send; nothing is opened or replayed.

How it works: `SensorsReceiver::setView(patterns)` keeps a message only when the decoder's `model` field contains one of the '|' separated lower-case parts. The engine sets it when the radio starts
(`engine_start.cpp`), from the table `sensorsViews()` in `sensors_rx.cpp`. The user's protocol selection of the Sub-GHz sensors screen still applies. The unknown signal analyser is silent in a view. The screens are the Sub-GHz sensors panel.
`DECT2_SENSVIEW_ACTIVE_CASES` (sensors_rx.h) lists the active standards for the engine switches. Numbers come from the free ranges of `docs/FAMILY-NUMBERS-4.0.md` (564 - 597 and 663 - 697); the integrator resolves any clash with another branch.

## Tests (`tests/sensors/test_sensors_views.cpp`, run on this branch with a hand-built binary)
The 28 real pulse trains of `tests/sensors/families.txt` (one recording of rtl_433's own test set per family: Acurite, Alecto, Ambient Weather, Bresser, Fine Offset, Hideki, security sensors, LaCrosse, Nexus / Prologue, Oregon, ERT, TFA, Schrader, X10)
are rebuilt as IQ with three impairment sets each (carrier offset 0 / +18 kHz / -27 kHz, pulse timing x1.00 / x1.04 / x0.96) and run through every one of the 22 views.
- A message appears in a view exactly when the receiver without a view decodes it and its model belongs to the view's family: 152 recording-and-impairment runs in their own view (the kind entries count too), 148 decoded (as without a view), 0 lost, 0 leaks into another view.
- Carrier to noise in the sample bandwidth, recordings decoded in their own view (OOK recordings, 250 kHz or 1 MHz input): 30 / 20 / 14 / 10 dB: 20 of 21; 6 dB: 17 of 21; 3 and 0 dB: 0 of 21.
- 20 s of noise and a plain carrier in every view: no message, nothing in the unknown list.
- Numbers, lookups and tunings of the 22 views are unique and consistent.

## What was not checked
- **Simulated signals only**, built from recorded pulse trains; no over-the-air capture.
- Neptune R900, Renault, Toyota and Citroen TPMS have no recording in `families.txt`: only their decoder names and the view patterns were checked (the decoders exist and a view takes them), not a decode.
- The mode panels, the web and Android lists were generated and read, not run in the running application.
