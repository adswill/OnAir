# The KA family (own modes, in development)

KAAL, KAPP, KAFT, KAVB, KAIT and KAWS are digital modes designed from scratch by Kristijonas Adomaitis for amateur radio and built into OnAir. Every transmission carries the sender's callsign, nothing is encrypted, and they use their own unpatented codecs. **All figures below were measured in simulation (generated signals with simulated noise); no KA mode has been on the air yet.**

| Mode | Job | Width | Notes |
|---|---|---|---|
| KAAL | voice, beacon, station ID | about 10 kHz (8 ksym/s, 16-QAM) | own 12 / 16 kbit/s voice codec live; 24 / 32 kbit/s live at 16 ksym/s; the callsign is also hidden in a sweep signature |
| KAPP | short messages and small data | 100 kHz | nine profiles, BPSK to 1024-QAM, selective-repeat bursts |
| KAFT | file transfer | 200 / 500 / 1000 kHz | ten profiles, hash-checked files |
| KAVB | video | 1 to 2 MHz | physical layer and container only; no video codec yet (in development) |
| KAIT | still pictures | under 200 kHz | a picture in a 10 s burst, sent in layers |
| KAWS | HF weak-signal chat and beacons | 2.4 kHz | chirp and OFDM profiles in UTC slots |

KAAL, KAPP and KAFT can write an IQ file you can open in SDR++ or OnAir without a radio. KAPP and KAFT only listen: they send no acknowledgement.

## In the app

KAAL voice and beacon (10 kHz, receive and transmit), **KAPP** packets (100 kHz), **KAFT** file transfer (200 kHz to 1 MHz), **KAVB** video (1 to 2 MHz, spec and physical layer only), **KAIT** pictures in 10 s bursts and **KAWS** HF weak-signal chat. Measured in simulation only, never on the air.
