# Ham science modes (3.8)

Nine receive-only modes, families 460 - 465 and 500 - 502 (docs/MODE-NUMBERS.md), one receiver class in `core/src/hamsci`, one screens file `app/hamsci/hamsci_ui.cpp`. Auto is on by default; the manual settings are under Advanced.

| Mode | What it shows | Test |
|---|---|---|
| QRSS / DFCW grabber | slow waterfall, keyed text from QRSS and DFCW element lengths | tests/hamsci/test_hamsci_qrss.cpp |
| FSK441 | meteor scatter pings, decoded text | test_hamsci_fsk441.cpp |
| ISCAT | decoded ISCAT messages | test_hamsci_iscat.cpp |
| PI4 | decoded beacon messages | test_hamsci_pi4.cpp |
| Throb / ThrobX | decoded text | test_hamsci_throb.cpp |
| SuperSID | VLF station levels, flare detection | test_hamsci_sid.cpp |
| Riometer | absorption against a sidereal quiet-day curve | test_hamsci_riometer.cpp |
| Pulsar folding | folded profile, S/N, and the antenna it takes | test_hamsci_pulsar.cpp |

Engine wiring: tests/hamsci/test_hamsci_engine.cpp and tests/app/test_hamsci_mode.cpp.

## What is simulated

Every mode has been checked on simulated signals only, made by `core/src/hamsci/hamsci_gen.cpp` (noise, frequency offsets and drift added by the tests). No mode has been tested on a real recording or a live signal yet. The SuperSID station list and the pulsar table values (period, dispersion measure, flux) were written from memory and still need to be checked against the published catalogues.

## What was skipped

- Opera and SIM31: not built. There is no public description of either protocol, and the project does not invent protocols.
- WINMOR: not part of this lane. It has a specification; its pilot detection was added later in the xlink family (core/src/xlink/winmor.cpp).

## What the pulsar mode needs

A pulsar is thousands of times weaker than the noise in a single sample. It shows only after minutes to hours of folding, and only with roughly one square metre of effective area and a low-noise amplifier at the feed (a 1.5 to 2 m dish, or several long Yagis) for B0329+54 at 408 MHz. A whip, discone or wideband dipole will not show a pulsar. The screen computes the area needed from the radiometer equation (`hamsci::pulsarRequiredAreaM2`). This mode was not tested on a real pulsar.
