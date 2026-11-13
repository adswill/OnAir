# Wi-Fi networks (2.4 / 5 / 6 GHz beacon monitor)

Passive receiver for the access points around: beacons, probe responses and (6 GHz) FILS discovery frames, shown as a phone's Wi-Fi list
(SSID, BSSID, vendor, band, channel, width, standard, security, capabilities, level, first and last seen, beacon count). Nothing is
transmitted, probed, joined or decrypted; data frames are counted only. `std` 56 in the engine, `dect2cli --standard wifi`.

## Files

| file | what |
|---|---|
| `wifi_rx.cpp` | the receiver: 802.11b DSSS chain (2.4 GHz), one OFDM receiver per 20 MHz channel, the network table, telemetry |
| `wifi_ofdm.cpp`, `wifi_ofdm_rx.cpp` | OFDM PHY (clause 17): tables, convolutional code, Viterbi, scrambler, transmit side for the test signal; receiver (short-training detect, offset estimate, long-training sync, equaliser, SIGNAL, DATA, FCS) |
| `wifi_proto.cpp` | DSSS PHY pieces, frame parser (beacon, probe response, FILS discovery), beacon builder |
| `wifi_elems.cpp` | element parsers: HT / VHT / HE / EHT capability and operation, 6 GHz operation information, Reduced Neighbor Report, RSN AKM suites and PMF, country, power constraint, mobility domain; plain-words capability text and rates |
| `wifi_chan.cpp` | channel plans (2.4 / 5 / 6 GHz), regulatory domains, UNII / DFS / PSC, operating classes, 40 / 80 / 160 / 320 MHz centre channels |
| `wifi_mk.cpp` | builders for the elements and the FILS discovery frame (test signal) |
| `wifi_gen.cpp`, `wifi_gen56.cpp` | test signal generator (DSSS and OFDM, carrier offset, clock error, noise, position in a wide capture, duplicates); the 2.4 GHz and the 5 / 6 GHz scenes |
| `wifi_oui.cpp` | compact OUI table (vendor column) |

## Bands and channels

* 2.4 GHz: channels 1 - 14 (2407 + 5 n MHz, 14 at 2484). DSSS 1 Mbit/s beacons and OFDM beacons.
* 5 GHz: channels 36 - 64, 100 - 144, 149 - 177 (5000 + 5 n MHz): UNII-1, 2A, 2C, 3, 4 (5.9 GHz); DFS marked; lists per domain: all, Europe, USA, Japan, China.
* 6 GHz: channels 1 - 233 and 2 (5950 + 5 n MHz, ch 2 at 5935): UNII-5 to UNII-8; Europe lists 1 - 93; the preferred scanning channels 5, 21, 37 ... 229 (PSC).
* Beacons are non-HT OFDM at 6 Mbit/s whatever the BSS width; a 40 / 80 / 160 MHz BSS repeats them on every 20 MHz part, so the 20 MHz primary is enough.

## What is decoded

Elements: SSID, DS parameter set, HT Capabilities / Operation (primary channel, 40 MHz, streams, short GI), VHT Capabilities / Operation (width set, streams,
max MCS, centre channels), HE Capabilities / Operation (TWT, streams, MCS, BSS color, 6 GHz operation information: primary, width, centre, regulatory
info indoor / standard power / very low power, duplicate beacon), EHT Operation (width up to 320 MHz) and presence of EHT Capabilities, RSN (WPA2 / WPA3-SAE / OWE
/ enterprise, AKM suite names, PMF capable / required), WPA vendor element, country, power constraint, mobility domain, Reduced Neighbor Report
(co-located 6 GHz APs: operating class, channel, BSSID, short SSID, BSS parameters). FILS discovery: SSID or short SSID, beacon interval, timestamp.
Unsolicited probe responses (a probe response to the broadcast address) are flagged. The standard column is derived from the elements (`a/n/ac/ax/be`).

DSSS 2 Mbit/s (DQPSK) after the long and the short preamble is decoded too (4.1, for ESP-NOW); the OFDM receiver reads 12 Mbit/s (QPSK) when a mode asks for it (`setOfdmQpsk`).

Not decoded: CCK 5.5 / 11 Mbit/s, OFDM rates above 12 Mbit/s (a good SIGNAL field is counted as "other rate"), the optional fields of a FILS discovery frame after the SSID, Wi-Fi 7
multi-link elements, EHT capability details, 802.11ah (HaLow has its own mode).

## Receiving

A 20 MHz channel needs 20 Msps (25 Msps is better for the radio's filter). A capture of 43.5 Msps or more centred on 5 / 6 GHz holds several channels: each gets
its own channel filter and OFDM receiver (`channelsInSpan`). Radios that stop below 20 Msps show a
message in the tuner; radios specified below 5 GHz or 6 GHz show the range message. The scan hops over the chosen band and domain with a dwell of 100 - 2000 ms
(default 200: a beacon comes every 102.4 ms).

## Tests (`tests/wifi/`)

`test_wifi_ofdm` (PHY tables, code, loopback with offset / noise / clock / mid-frame start), `test_wifi_elems` (channel plans, element parsers on byte strings built
from the standard's layouts), `test_wifi_56` (every channel of both plans, sweeps over offset, noise, clock, sample rate, off-centre, mid-frame start, a wide
capture with non-HT duplicates), `test_wifi_engine56` (through the engine), plus the earlier DSSS tests.
