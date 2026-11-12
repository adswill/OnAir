# Ham station (4.0)

The Ham view (the third choice of the RX / TX / Ham switch) as one amateur station screen. This page lists what the Ham view had before 4.0,
what 4.0 adds, and what is still missing.

## What existed before 4.0

| Part | Where | What it does |
|---|---|---|
| Ham view | `app/ham/ham_ui.cpp` | Mode bar (FT8, FT4, FT2, JT65, JT9, JT4, WSPR beacon, keyboard modes), band bar with the WSJT-X dials, callsign / grid, licence box, Arm and Halt, tabs Operate / Log / Awards / Map / Spots / Contest / Guide |
| QSO engine | `core/src/ham/ham.cpp` (`QsoEngine`) | Auto sequence Tx1 .. Tx6 for FT8 / FT4 / FT2 / JT65 / JT9 / JT4, watchdog, retry limit, auto log |
| TX path | `core/src/ham/ham_tx.cpp` (`HamSlotTx`), `ham_safety.cpp` | One gate for every Ham transmission: explicit Arm; licence, band, country plan are warnings only; duty-cycle limits; TX audit log |
| Keyboard modes | `core/src/ham/ham_kbd.cpp`, `app/ham/ham_kbd_ui.cpp` | PSK31 .. PSK250, QPSK, RTTY, CW, Olivia, Contestia, MFSK, THOR, DominoEX, Hellschreiber, MT63, FSQ, IFKP: type-ahead, macros, callsign pick, contest exchanges |
| Logbook | `core/src/ham/ham_log.cpp` (`Logbook`) | ADIF import / export with unknown fields kept, edit, undo / redo, dupe check, search, awards (DXCC, WAS, grids, CQ zones), statistics |
| Countries | `core/src/ham/ham_dxcc.cpp` | Built-in prefix table, user cty.dat |
| Contest | `core/src/ham/ham_contest.cpp`, `ham_ops.cpp` | Exchanges of the common contests, serials, dupe sheet, Cabrillo export |
| Uploads | `core/src/ham/ham_upload.cpp` | QRZ Logbook, Club Log, eQSL; off by default; credentials local |
| Rig | `core/src/source/hamlib_rig.cpp`, `hamlib_link` | Hamlib CAT: PTT, frequency, mode, split |
| PSK Reporter | `core/src/net/pskreporter.cpp` | Sending your own reception reports (opt-in) |
| Others | `ham_bandhop.h`, `ham_spotsum.h`, `band_open.cpp`, `ham_mapfeed.cpp`, `ham_events.cpp` | Band hopping by spot counts, propagation summary, band-open matrix, map feed, ham.decode events |

## What 4.0 adds

### The screen
- **Header**: the dial frequency in large type, an S-meter (receive level; a right click calibrates dBFS to dBm, then it reads S units:
  S9 = -73 dBm below 30 MHz, -93 dBm above, 6 dB per unit), and the band-plan segment at the TX frequency.
- **Band plan hint** (`ham_station.h` `segmentWarning`): a simplified IARU plan per region (CW-only parts, narrow-band parts, beacon guard
  bands, satellite and FM parts). It only warns ("14.0300 MHz is in the CW part ..."); it never stops a transmission. Region 3 uses the
  region 1 table (its HF plan is close; 80 m and 40 m are wider there). National plans can differ: this is a hint.
- **Band bar for every mode**, the keyboard modes too, with a **frequency memory per band and mode** (`FreqMemory`, saved in the settings):
  a click on a band goes back to where you were last on that band in this mode, else to the usual frequency. Switching the mode does the
  same on the current band. "Usual" forgets the remembered one. Usual frequencies of the keyboard modes (PSK31, RTTY, the QRP CW calling
  frequencies, JS8, SSTV, MSK144) come from common practice and differ by region; the slot modes keep the WSJT-X dials.
- **Modes**: Q65 (sub-modes A .. E), FST4 and MSK144 join the slot modes (they had decoders and generators; the Ham view did not offer
  them; Q65 and FST4 run at their 60 s default period). **More modes** lists every other amateur mode OnAir decodes that has its own screen
  (JS8Call, FSK441, ISCAT, JT6M, SSTV, SSB / AM / FM through the general receiver, FreeDV, M17, D-STAR, DMR, C4FM, APRS, HF packet,
  ARDOP) and opens it. The catalog is `hamModeCatalog()`: one list with where each mode runs, its ADIF mode / submode and its report style.
- **Cluster tab** (online spots, every source off by default):
  - DX cluster (any host and port the user enters) and the Reverse Beacon Network (CW / RTTY port 7000, FT8 / FT4 port 7001) over telnet.
    These servers ask for a callsign: OnAir sends only that.
  - POTA and SOTA activator spots from their public APIs (nothing about the user is sent).
  - PSK Reporter "who hears me" (the callsign is in the query, at most every 5 minutes).
  - Your own decodes as "own" spots.
  - Needed marks per spot, a filter for this band and for needed stations only, a click tunes there (a slot mode goes to the band's dial
    for it) and puts the call in the QSO panel; "UP 2", "QSX 14.205" in the comment is read as a split and the QSO panel offers to set the
    rig's split (Hamlib) for that station.
  - **Alerts**: new DXCC, DXCC on a new band, in a new mode class, new zone, new state, new grid, new prefix, and a watch list with `*` and
    `?` wildcards; each station and band alerts once in 15 minutes.
- **Needed / worked before** (`ham_needed.h`): DXCC entity, entity on the band, entity in the mode class (CW / phone / digital), CQ and ITU
  zone, US state, grid square, WPX prefix, call, call on the band and mode, and "worked, not confirmed". The decode list shows new zone /
  state / mode / prefix beside the old marks.
- **Callsign lookup** in the QSO panel (`ham_callbook.h`): name, QTH, state, county, grid, QSL route, LoTW / eQSL user, from the local cache,
  from your own earlier QSOs, and, only when the user enters an account, from QRZ.com XML or HamQTH. The account stays in the local
  settings and goes only to that service, over HTTPS, in the request body. Logged QSOs get the empty fields filled from the same sources.
- **Activate tab**: POTA, SOTA, WWFF, IOTA. While an activation runs every logged QSO carries `MY_SIG`, `MY_SIG_INFO` and `MY_POTA_REF` /
  `MY_SOTA_REF` / `MY_WWFF_REF` / `MY_IOTA` (a two-fer: several parks). The count against the program's minimum (POTA 10 per UTC day,
  SOTA 4, WWFF 44), the POTA upload file `CALL@PARK-YYYYMMDD.adi` per park, the SOTA CSV v2. A station picked from a POTA / SOTA spot gets
  `SIG`, `SIG_INFO` and `POTA_REF` / `SOTA_REF` in the log. Nothing is uploaded: the files are for the user to upload.
- **Auto features**:
  - FT8 / FT4 sequencer, WSJT-X rules (see below), with a CQ modifier (CQ DX, CQ POTA, CQ SOTA, CQ WWFF, CQ TEST, CQ NA, CQ EU).
  - Several callers in one slot: the most needed is answered (new DXCC first), until the first answer is sent.
  - Band hopping by a UTC schedule ("0000-0600 40m", "1600-2000 auto"), "auto" choosing the band with the most decodes of the last hour
    and visiting the others now and then; only while no QSO or message is pending.
  - CW speed matching: the keyer follows the WPM of the CW station in focus (outliers ignored, 5 to 45 WPM).
  - Split "fake it" (slot modes with a CAT rig, opt-in): for each burst the rig's dial moves in 500 Hz steps so the audio stays between
    1500 and 2000 Hz, then comes back; the RF frequency and the TX gate's check are those of the real signal.
  - Auto log on QSO completion (as before), with the activation, hunted reference and callbook fields.
- **Logbook tools** (Log tab): Cabrillo import (the export existed, so a contest log round-trips), QSL confirmations merged from a LoTW,
  eQSL or card-list ADIF file (same base call, band, mode class, start within 30 minutes; also the DXCC, zones, state, grid the service
  knows), QSL counts, a backup of the log once a day (the last 30 kept, `ham-backups/` in the data folder), and **LoTW through TQSL**:
  OnAir writes the QSOs not yet sent and runs the user's own TQSL (`tqsl -q -d -u -a compliant -x -l <location> <file>`); OnAir never
  holds the certificate. Sent QSOs get `LOTW_QSL_SENT`.
- **Setup wizard**: country (its built-in band plan sets the IARU region), the grid from the receiver position (View > Map, GPS), a list
  of antennas, the callsign lookup account.

### FT8 / FT4 contest exchanges
The 77-bit ARRL Field Day (i3 0, n3 3 / 4) and ARRL RTTY Roundup (i3 3) messages: OnAir decoded them before; 4.0 can send them
(`core/src/hfdig/hfdig_ftx_msg.cpp` `packFieldDay`, `packRttyRu`). In the Operate tab choose the contest and your exchange ("2B EMA",
"579 MA" or "579 0001"); CQ becomes CQ FD / CQ RU, and the sequence is the WSJT-X one: the caller sends its exchange, the CQ station
answers R + its exchange, the caller sends RR73. The QSO is logged with STX / SRX (and the RST for the Roundup) and CONTEST_ID ARRL-FD or
ARRL-RTTY, so Export Cabrillo writes it. Other contests (NA VHF, EU VHF, WW Digi) are not done.

### The sequencer (WSJT-X rules)
| Situation | Before | 4.0 |
|---|---|---|
| We sent RR73 | RR73 repeated until 73 or the retry limit | One RR73 is enough (WSJT-X); then CQ again (or idle). JT65 / JT9 / JT4 RRR still waits for 73 |
| Their R-report again after our RR73 (they did not copy it) | (RR73 still repeating) | RR73 again within 4 slots; the QSO is logged once |
| They repeat their grid / report (did not copy ours) | Counted against the retry limit: the QSO could be given up while they were still calling | The retry count starts again: only unanswered sends count |
| Several callers in one slot | The first one decoded | The most needed (priority hook), until the first answer is sent |
| CQ | CQ CALL GRID | Optional modifier: CQ DX / POTA / SOTA / WWFF / TEST / NA / EU |
| Q65, FST4, MSK144 | not in the Ham view | Slot modes with the QSO engine (RR73) |
| Field Day / RTTY Roundup | not sent | The contest sequence above |

## Rules kept
- Nothing is sent to any service unless the user switches it on; credentials and keys stay in the local settings and go only to their
  service (HTTPS, request body). Off by default: every spot feed, the online callbook, uploads, TQSL.
- Transmit is never blocked by OnAir's own checks: licence, band, country plan, band-plan segment are warnings; Arm stays explicit.
- No codecs are added: digital voice modes open their own screens, which load user-installed codecs.

## Tests (simulated data only)
| Test | What |
|---|---|
| `tests/ham/test_ham_seq.cpp` | Sequencer flows: CQ answered with a report at once, RR73 once and again after a repeated R-report (logged once), repeats reset the retry count, the retry limit after silence, caller priority (and no switch after answering), CQ modifiers, JT65 RRR waiting for 73, tail-ending a busy station, MSK144 |
| `tests/ham/test_ham_ftxcontest.cpp` | Field Day and RTTY Roundup messages pack and unpack to the same text (1 .. 32 transmitters, classes, sections, RSTs, serials, multipliers, TU;), what is not one keeps its old packing, the parser, both contest sequences with the logged exchange, a lost RR73 sent again |
| `tests/ham/test_ham_station.cpp` | Catalog (every keyboard entry is a real preset, every slot entry a mode the engine runs), usual dials, frequency memory text, band plan segments (every segment inside an amateur band) and warnings, S-meter, region of a country, setup hints, CW speed matching, the hop schedule (wrap over midnight, errors), fake-it split |
| `tests/ham/test_ham_spots.cpp` | Cluster and RBN lines (day wrap, locator, SNR / WPM, mode guess), POTA / SOTA JSON (escapes, null, nested values, malformed), PSK Reporter XML, split reading, spot book merge and expiry |
| `tests/ham/test_ham_callbook.cpp` | QRZ / HamQTH XML (record, login, errors), the log as a callbook, cache file, the client with a fake transport: off sends nothing, password only in the body, cache hits, stale entries, an expired session logs in once more |
| `tests/ham/test_ham_needed.cpp` | WPX prefixes with portable forms, every needed level, unknown entity, alerts with quiet time, watch list, rules |
| `tests/ham/test_ham_logtools.cpp` | Cabrillo export -> import round trip, Field Day exchange, transmitter id, X-QSO, bad lines; LoTW / card / eQSL merges; QSL counts; backups with pruning; TQSL command line; references, activation count, POTA file name, SOTA CSV |
| `tests/ham/test_ham_net.cpp` | Telnet line splitting (IAC, split packets, prompts), feed URLs, HTTP polling intervals with a fake getter (off fetches nothing, PSK Reporter at most every 5 minutes) |

The services (QRZ, HamQTH, POTA, SOTA, PSK Reporter, DX clusters, RBN, TQSL) were not contacted by the tests: their samples follow the
documented field names. The UI was built on GitHub (quick.yml); it has not been run with a real cluster or callbook account yet.

## Not done / next
- Band hopping changes the band only, not the mode, of a schedule line.
- FST4W beacon (like the WSPR beacon) and Q65 / FST4 periods other than 60 s in the Ham view.
- Auto upload to LoTW: TQSL runs only on the button.
