# LBJ, KLW and TDCS railway reception

The LBJ message parser and the KLW / TDCS receive chain are ported from
[railair-terminal](https://github.com/frank-1225/railair-terminal), source revision
`a3a2bcbc998ca79c379f0af05d4ae3ffee033164`, under GPL-3.0-or-later.
The existing OnAir LBJ modem is retained. KLW and TDCS share `core/src/railway/`
and have separate mode entries and receiver state.

| Mode | Default frequency | Engine standard | Active telemetry standard | Data |
|---|---:|---:|---:|---|
| LBJ | 821.2375 MHz | 83 | 82 | 1200 bit/s POCSAG numeric pages |
| KLW | 866.2375 MHz | 148 | 147 | 1200 bit/s FFSK, 1200 / 1800 Hz tones |
| TDCS | 457.550 MHz | 149 | 148 | Same CIR/FFSK physical framing |

Frequencies are defaults and remain editable in OnAir. The KLW default is the
passenger end-of-train channel used by the source project, not a claim that all
KLW equipment uses that channel. These modes register no transmit capability.

## LBJ fields and association

RIC 1234000 carries the numeric train number, speed in km/h, and mileage in
tenths of a kilometre. Negative mileage is retained. Even train numbers display
up, odd numbers down; the POCSAG function bits do not determine direction.
Blank or lost speed / mileage remain empty. A lost train number rejects the
basic page, preserving the original receiver's partial-field behavior.

RIC 1234002 uses fixed character offsets in the numeric alphabet. Spaces must
not be removed: they represent hexadecimal C in the binary fields.

| Characters (zero based) | Meaning |
|---|---|
| 0..3 | Two ASCII bytes: train prefix |
| 4..7 | Four-digit locomotive type / subtype |
| 8..11 | Four-digit locomotive serial |
| 12..13 | Cab-end bytes, retained raw |
| 14..29 | Eight bytes of GBK route text |
| 30..38 | Longitude: three degree digits, six minute digits (minutes / 10000) |
| 39..46 | Latitude: two degree digits, six minute digits |
| 47..49 | Unknown tail, retained raw |

Coordinates are WGS-84. Minute values must be below 60 and the position must be
inside the source protocol's bounds (70..140 E, above 0..60 N). Invalid or zero
positions remain absent; no map conversion or position extrapolation is done.
GBK converts to UTF-8 using iconv on Unix and Windows code page 936 on Windows.
Four-digit subtypes such as 3073 are looked up before a trailing-zero base
type such as 2390. Unknown type codes remain visible.

One unconsumed basic page may pair with one detail page within five seconds,
in either arrival order. A later basic report cannot reuse an old extension.
An orphan detail retains its locomotive, route and valid position, with an
explicit unpaired label and empty train / speed / mileage. Malformed or unknown
pages break pending association. Association cannot distinguish interleaved
trains because the extension carries no independent train number.

RIC 1234008 is a separate time broadcast: characters 1..4 are HHMM. It never
inherits a train or a position. Existing alternate RICs 1233999 and 1234001 and
combined basic/detail pages remain supported. Correction counts include all
message codewords, including the extension. The legacy `crcOk` field remains
the UI quality flag; POCSAG pages do not have an application CRC.

LBJ result files use `lbj3` to preserve the added fields, with readers for `lbj1`
and `lbj2`. Coordinates, pairing status and time broadcasts are also exposed in
the remote telemetry JSON.

## CIR/FFSK and application frames

The shared IQ path mixes the selected channel, resamples to 48 kHz, filters
before FM demodulation, and feeds the same audio decoder used by the tests.
A continuous 300..4000 Hz FIR rejects discriminator DC. A coherent 40-sample
1200 / 1800 Hz tone detector feeds a soft timing loop and eight fallback symbol
phases. Filter history, oscillator phase and timing survive callback boundaries.

The air frame is sync `0x0DD4259F` followed by 26-bit words: 16 data bits and
10 parity bits, polynomial `0x5B9`. Normal and inverted bit polarity are
accepted, with up to two errors in the sync. Each BCH syndrome retains every
candidate with at most two corrected bits; candidates are bounded to 64 and
frame storage to 258 bytes. An odd application length has one padding byte
outside the CRC.

Only a unique candidate passing the application length, address, control /
command, function and CRC-16/XMODEM checks becomes a message. CRC polynomial
`0x1021`, initial value zero, covers the header and payload; the transmitted CRC
is big endian. Raw unprotected byte streams are not accepted as air frames.
Duplicate hypotheses from parallel clocks are suppressed for 150 ms using
sample time. Later repeated receptions remain separate messages.

KLW accepts payload lengths 6, 11, 15 and the 17-byte A1/A2 clock variants, both
known destination addresses, BCD locomotive / tail IDs, all 19 source-project
commands, optional pressure and validated calendar time. Unknown commands and
parameter bytes are retained. The eight-digit locomotive ID contains a
three-digit type, one reserved digit and a four-digit serial.

TDCS uses a 32-byte little-endian payload: train prefix and numeric ID,
locomotive type and serial, signed mileage and its direction bit, 10-bit speed,
weight, calculated length, car count, station / track fields and driver ID.
Mileage test sentinels 9999888 / 8888888 and unavailable sentinels 9999999 /
0xFFFFFF never become a displayed kilometre post.

The panel exposes the decoded fields, correction count and raw frame. `cir1`
result files retain the CRC-checked frame, reception time and correction count;
loading revalidates the frame and the selected protocol. Telemetry JSON exposes
the decoded message list. Histories are bounded to 400 messages per receiver.

## Tests and validation

The normal CMake mode glob registers these tests:

```sh
ctest --test-dir build --output-on-failure -R '^(lbj|railway)_'
```

* `lbj_proto`, `lbj_rx`, `lbj_impair`: existing POCSAG, modem and impairment
  regressions. Synthetic up trains now have even IDs because direction follows
  train-number parity rather than the POCSAG function bits.
* `lbj_fields`: fixed offsets, GBK, subtypes / unknown types, coordinates,
  one-use pairing in both orders, expiry, malformed pages, time broadcasts,
  negative mileage, extended IQ reception and results round trips.
* `railway_proto`: CRC check vector, KLW payload variants, clock validation,
  TDCS mileage sentinels, mutations, both polarities, BCH errors, noise,
  recovery after a failed CRC and rejection of raw byte shortcuts.
* `railway_rx`: audio in single-sample and larger callbacks, DC offset,
  polarity, real repeats, +/-0.3% symbol-clock drift; FM IQ at 240 and 960 kS/s,
  15 / 25 dB SNR, carrier offsets through +/-8 kHz, results persistence and
  protocol isolation.

Local validation of this port used Windows x64 and Zig 0.14.1's C++20 compiler,
including compilation of the changed app and engine units. The full app's
dependency build and cross-platform CI have not been run locally. No physical
radio was used for this port. Source-project reception was previously checked
against supplied KLW and DMIS WAVs; those recordings are not redistributed and
are not a substitute for on-air testing of the C++ receiver.

## Provenance

* `railair/decoders/lbj.py`, `railair/decoders/railway.py`,
  `railair/decoders/loco_types.py`, `railair/dsp/cir_framing.py` and
  `railair/dsp/ffsk.py` in railair-terminal are the implementation sources.
* The source project's LBJ field research cites
  [undef-i/LBJ_Console](https://github.com/undef-i/LBJ_Console) and
  [MisakaXing/RP2040-Based-LBJ-Receiver](https://github.com/MisakaXing/RP2040-Based-LBJ-Receiver),
  GPL-3.0. The original OnAir modem retains its own provenance in [lbj.md](lbj.md).
* TDCS / KLW layouts originate in the user-supplied CRRD reference, MIT,
  Copyright (c) 2020 Alex L Manstein; the notice is in
  [LICENSES/CRRD-MIT.txt](../LICENSES/CRRD-MIT.txt).
* The CIR syndrome / candidate decoder and timing recovery were independently
  implemented in railair-terminal and ported here. No multimon-ng executable,
  DLL, user recording, voice clip, private configuration or archive is included.
* The locomotive table contains the code/name facts supplied with the source
  project. No live train, aircraft, route or location database is bundled.
