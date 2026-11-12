# Panels sized to their content (4.0)

The owner's screenshots of CODAR / HF ocean radar and NOAA DSB: the sidebar layout (the default) gave every mode the same three panels at fixed
sizes, the spectrum and waterfall, the list on the right ("results column") and a full-width lower row. CODAR has nothing in the lower row, so
a large empty box sat under the spectrum; NOAA DSB has one small square constellation in a full-width row, and a list of two lines in a
full-height column. 4.0 sizes the panels to what they show, for every mode, with the spectrum and waterfall taking what is freed.

## What it does

`app/ui/overview_layout.h` (pure, no ImGui) decides the boxes; `app/ui/overview_layout_ui.inc` (included by `ui2.cpp`, called from `bodyParent`)
draws the panels in them and measures what each drew (from the child window's first cursor position to its furthest item).

| Content seen | Placement |
|---|---|
| The lower row drew nothing (no `ModeUi::panels`, or nothing to show yet) | Row hidden; the spectrum takes the height. The row is drawn out of sight every third frame, so it comes back when content appears. |
| Lower content narrower than the right column (a square constellation, a fixed 140 px box, a few text lines) | Moved into the right column, under the list. With a short list the column is only as wide as the plot. |
| Lower content wide but shorter than the row (a level history line) | The row shrinks to the content (at least 90 px). |
| Lower content wide and as tall as the row (DVB-T, DAB, FM, ATSC, FT8 audio) | The row as before. |
| A list of about three lines or less ("stopped - showing saved results", "starting") | A strip above the spectrum, as tall as the lines; the spectrum takes the column's width. The list is drawn out of sight in a column box, so a list that grows (a table of stations once decoding starts) gets its column back. |
| A list that drew nothing | No strip, no column. |
| A mode with no list of its own that makes no transport stream (the 51 modes in group 2) | `rightPanel` draws nothing there in this layout, instead of the TV modes' clock and an empty service list (tv.cpp). The TV modes that make a stream keep their service cards. |

No jumping: a change of place must hold for 0.25 s (content appearing) or 0.6 s (a panel going away), and at least 1 s must pass since the
previous change; then the sizes glide there in about 0.15 s. A mode just opened goes to its place at once (0.35 s settling). A box that
shrank to its content only grows again when the content overflows it, so content that sizes itself to its box does not ratchet down.
The state is kept per mode (and per transmit view), so a mode opened again starts in its place.

The user's sizes: dragging the bar between the spectrum and the column, or above the lower row, stores the size for that mode
(`App::panelSizes`, "family=rightW,bottomH;" in unscaled points; a double click puts the automatic size back). A dragged row stays a row at
that height (an empty one is still hidden and comes back at that height); a dragged column stays a column at that width. A mode never
dragged uses the sizes dragged before 4.0 (the old global `splitRight` / `splitBottom`). View > Reset layout clears the per-mode sizes;
View > Fit panels to their content (on by default) turns it all off: the 3.9 layout exactly (`ovl::fixedLayout`).

Where: the sidebar layout of the new interface (View > layout 0, the default), not the Unified layout (it has its own handling: the
analysis is a tab there), the Scope / Tiles / Faceplate / Dock / touch layouts or the classic interface. Those keep their fixed panels.

## Every mode, grouped by layout type

`tools/layout_survey.py <repo> out.json` reads `app/ui/mode_table.cpp` and `app/panels/modeui.cpp`, finds each mode's `ModeUi` and
classifies its `panels` function from the code (a table or `GetContentRegionAvail().x`: wide; a square plot sized to the row's height: small;
`kvRow` lines: text) and whether it has a list. 449 rows of the mode table (the rest have symbolic family numbers; the four HF utility ones
were added by hand). Runtime does not depend on this: the placement comes from what each panel draws. The survey is how the groups and the
table below were chosen.

The empty share is the part of the main area (spectrum, list and lower row together) that shows nothing: the area minus the spectrum pane
minus what the list and the lower content drew. Computed by `ovl::layout` and `ovl::unusedShare` for a 1600 x 900 main area with content
models of each group (tests/app/test_overview_layout.cpp prints the table; the numbers below are from the GitHub Linux runner). They are
models of what the panels draw, not screenshots of each mode: screenshots of a few modes are still to do (the Mac was on battery).

NUMBERS_TABLE

### Group 1: no lower content, own list (288 modes)

Placement: lower row hidden, list in its column (or a strip while it shows only "stopped" / "starting"). Nothing added to the lower row:
these modes show their history and tables in their own tab, and the freed height goes to the waterfall, which is where their signals are
seen (CODAR's sweeps are lines in the waterfall).

Internet TV (IPTV), TV sat beacon decoder, FPV video (analog), AM broadcast, AM stereo (C-QUAM), Mode S uplink, ACARS, VDL2, VDL4, VDL3, HFDL, Airband, VOR, ILS, NDB, Drone Remote ID, AIS, VDES, Marine VHF, SELCAL (aircraft tone call), ATIS (inland waterways), DGPS beacon, Inmarsat Aero, COSPAS-SARSAT 406 MHz, Argos, UAT, PACTOR-II, PACTOR-III, PACTOR-IV, MIL-STD-188-110 serial, STANAG 4285, ARQ-E, ARQ-N, AUTOSPEC, SPREAD, G-TOR, EFR teleswitch, Piccolo Mk VI, Coquelet-8, QO-100, FANET, OGN tracker, GBAS VDB, LDACS (structure), AeroMACS (structure), End of train (AAR EOT/HOT), DSRC road toll (EN 12253), 802.11p V2X (ITS-G5, DSRC), C-V2X (structure), ERMES paging, Marine radar (detect), Weather radar (detect), RC model PPM (72 MHz), UVB-76 buzzer monitor, GRAVES meteor echoes, Radio Jove bursts, Beacon propagation monitor, EM4100 LF RFID (125 kHz), UHF RFID reader (EPC Gen2), Qi wireless charging, FM modulation monitor, AM modulation monitor, Power-line noise locator, Radar pulse analyser, GNSS interference detector, Passband signal classifier, VLF MSK transmitter monitor, Spectrum occupancy monitor, Modulation quality (MER) meter, Carrier frequency accuracy meter, PTT and duty-cycle logger, Intermodulation finder, Spur and image finder, Satellite Doppler curve logger, Frequency-hopper analyser, Persistent carrier (birdie) finder, Burst finder, Fading analyser, SDR input health meter, Phase noise meter, SSB transmitter test meter, FM SINAD meter, FSK parameter analyser, Noise figure (Y-factor) meter, CW keying quality meter, Noise floor logger, Narrowband FM deviation and CTCSS meter, AFSK (TNC) tone tuner, OOK / ASK pulse analyser, Transmitter frequency stability meter, Crest factor (PAPR / CCDF) meter, Occupied bandwidth meter, Spectral kurtosis RFI finder, SDR stream glitch detector, ADS-L, FLARM, DJI drones, CubeSat telemetry, AX100 Mode 5, AX100 Mode 6, USP, NGHam, U482C, BEESAT / Mobitex, Globalstar, NXDN, P25, P25 Phase 2, dPMR / PMR446, CB radio, FRS / GMRS, PMR446 analog, MDC-1200, Wireless mic finder, Tone decoders (CTCSS, DCS, DTMF, selcall), TETRA, TETRAPOL, MPT1327 trunking, LTR trunking, Motorola Type II, TETRA DMO, EDACS trunking, Cell towers, 5G NR cells, NB-IoT cells, GSM-R, LBJ, POCSAG, FLEX, ISM sensors, Sub-GHz sensors, Sferics (lightning), Lightning position (network), Bluetooth LE, BLE tracker detector, Zigbee / 802.15.4, Thread, Wi-Fi HaLow (802.11ah), Datawell Buoy HF Link, CODAR / HF ocean radar, Multitone Paging, NOAA Weather Radio SAME, ANT+ sensors, DECT cordless phones, Wi-SUN FAN, Wireless M-Bus (meters), NFC / RFID 13.56 MHz, Z-Wave, Bluetooth Classic, LoRa (raw), Sigfox, Wi-Fi networks, Time signals, e-Czas (225 kHz, Poland), Meteor scatter, GOES DCS, Meteosat HRDCP, Fleet Broadcast (UHF SATCOM), SBAS, Ionospheric sounders, Radio astronomy, PSK31, MT63, SITOR, eLoran / Loran-C, ASTERIX, ARDOP, JS8Call, CW, CP16, M17, D-STAR, Yaesu Fusion, APRS (AFSK), Ham FM repeaters, Shortwave, HF ALE, LoRaWAN (all channels), LoRa satellites, HF waveform ID, HF beacons, PACTOR-I (experimental), HFGCS / HF voice, ROS (HF), SSDV pictures, EBICab / Eurobalise (needs a user test), Inmarsat (all carriers), Inmarsat BGAN, Reticulum (RNode), ExpressLRS, MAVLink (SiK radio), FrSky RC (D8 / D16), FlySky RC (AFHDS / 2A), Futaba RC (S-FHSS), Spektrum RC (DSM2 / DSMX), wfb-ng / OpenHD, New Packet Radio (NPR-70), WINMOR, mioty, DECT NR+, nRF24L01 (needs a user test), Flipper Zero Sub-GHz chat, Signal workbench, General receiver, Spectrum explorer, Spectrum analyser, KAIT, Oregon Scientific sensors, Acurite sensors, LaCrosse sensors, Fine Offset / Ecowitt sensors, Bresser sensors, Hideki sensors, TFA Dostmann sensors, Nexus / Prologue sensors, Alecto / Esperanza sensors, Ambient Weather sensors, Wireless alarm sensors, X10 RF, Itron ERT meters, Neptune R900 water meters, TPMS: Schrader, TPMS: Renault, TPMS: Toyota, TPMS: Citroen / Peugeot / Fiat, Wireless M-Bus S1, Wireless M-Bus T1, Wireless M-Bus C1, Wireless M-Bus R2, Wireless M-Bus N1 (Wize, 169 MHz), AX.25 G3RUH 9600 (satellites), CCSDS Reed-Solomon (satellites), CCSDS concatenated (satellites), ILS marker beacons (75 MHz), FSK modems (Bell 103, V.21, V.23, Bell 202), SSR interrogations (1030 MHz), AIS Class A vessels, AIS Class B vessels, AIS base stations, AIS aids to navigation (AtoN), AIS search and rescue aircraft, AIS-SART, MOB and EPIRB-AIS, DCF77 (Germany, 77.5 kHz), MSF (United Kingdom, 60 kHz), WWVB (United States, 60 kHz), JJY 40 kHz (Japan), JJY 60 kHz (Japan), WWV / WWVH (United States, HF), BPM (China, HF), BPC (China, 68.5 kHz), HLA (Korea, 5 MHz), BPM coded (China, HF), RWM (Russia, 9996 kHz), Weather stations (all brands), TPMS: all makes, Utility and energy meters, Remotes, doorbells and switches, Wireless M-Bus: Kamstrup, Wireless M-Bus: Landis+Gyr, Wireless M-Bus: Itron, Wireless M-Bus: Sensus, NFC: ISO 14443 Type A, NFC: ISO 14443 Type B, NFC: ISO 15693, STANAG 4539, 3G-ALE, Link 11 CLEW, STANAG 4481 FSK

### Group 2: no lower content, no list (51 modes)

Placement: lower row hidden, no column, no strip: the spectrum and waterfall take the whole main area. Their readouts are in the status
bar and their own tab.

OpenSky identifier, QRSS grabber, DFCW grabber, FSK441, JT6M, Schumann resonance monitor, Whistler and tweek detector, Moon echo (EME) test, Radio Jove, e-CALLISTO solar bursts, Aurora / Es / TEP beacons, ISCAT, PI4, Throb, SuperSID (VLF), Riometer, Pulsar folding, UMTS cells, CDMA2000 cells, TD-SCDMA cells, Zombie satellites, What satellite is this?, UHF SATCOM skimmer, Thuraya GMR-1 channels, Starlink detector, OneWeb detector, TDRS carriers, Tiangong / Shenzhou carriers, Tsikada / Parus, Gonets, Tiantong signalling, Space station, NavIC, FSQ, IFKP, DME, CIS-36-50 / 50-50 (BEE), CIS-14, CIS-12 (MS5), CIS-36 (MFSK), CIS OFDM HDR, CIS-128, CIS-3000, KAAL, KAVB, KAPP, KAFT, KAWS, HF packet 300 Bd, Voice inversion scrambler, ELT 121.5 MHz

### Group 3: a square plot as tall as the row (45 modes, the weather and Earth-observation satellites)

`panels` draws one constellation (`scatter`) sized to the row's height, with a caption. Placement: the plot goes into the right column under
the list (the column narrows to the plot when the list is short); the spectrum takes the full height.

Meteor-M LRPT, GOES HRIT, Elektro-L, GK-2A LRIT, GK-2A HRIT, FY-4 LRIT, FY-4A HRIT-II/III, FY-2 S-VISSR, HRPT / AHRPT, NOAA APT, NOAA HRPT, NOAA GAC, NOAA DSB, Meteor HRPT, Elektro-L RDAS, Arktika-M RDAS, Arktika-M GGAK, AWS L-band, MSG Raw Data, HimawariCast, GeoNetCast, JPSS HRD (NPP, NOAA-20), JPSS HRD (NOAA-21), EOS DB (Aqua, Aura), EOS DB (Terra), MetOp X-band dump, MetOp-SG DDB, Landsat 8/9 X-band, Landsat 8/9 S-band, Proba S-band dump, Proba-V X-band dump, GCOM-W1, GCOM-C1, STEREO high rate, Oceansat-2 DB, Oceansat-3 L-band, Saral L-band, Oceansat-3 X-band dump, Saral X-band dump, Kanopus-V, DMSP RTD, FY-3 AHRPT, FY-3 MPT

### Group 5: a few text lines in the lower row (11 modes)

`panels` repeats the receiver's state lines (`kvRow`). Placement: in the column under the list when the lines fit its width, else a row
only as tall as the lines.

GOES GRB, GOES GVAR, GOES CDA, GOES MDL, GK-2A UHRIT, GK-2A CDAS, S-band CCSDS, Geoscan, Lucky-7, Spino, Erminaz

### Group 6: full-width plots (65 modes)

Placement: the row as in 3.9, shrunk when a plot is shorter than the row.

Analog TV, MAC, DRM, HD Radio, CDR, ADS-B, Mode A/C, NAVTEX, DSC, Weather fax, GNSS, Inmarsat-C, Iridium, Orbcomm, DMR, Digital voice (auto), LoRaWAN, Meshtastic, MeshCore, Radiosonde, FT8, FT4, FT2, WSPR, JT65, JT9, Q65, FST4, MSK144, JT4, RTTY, SSTV, HamDRM, Olivia, Contestia, MFSK, THOR, DominoEX, Hellschreiber, FreeDV, LoRa APRS, MeshCom, PSK demodulator (experimental), Radiosonde: Vaisala RS41, Radiosonde: Vaisala RS92, Radiosonde: Graw DFM, Radiosonde: Meteomodem M10, Radiosonde: Meteomodem M20, Radiosonde: Lockheed Martin LMS6, Radiosonde: InterMet iMet-4, Radiosonde: InterMet iMet-54, Radiosonde: Russian MRZ, Radiosonde: Meisei iMS-100, GPS L1 C/A, QZSS L1, GLONASS L1, BeiDou B1I, Analog TV: PAL B/G, Analog TV: PAL I, Analog TV: PAL D/K, Analog TV: SECAM D/K, Analog TV: SECAM L, Analog TV: NTSC M, Analog TV: PAL M, Analog TV: PAL N

### Group 9: the TV and broadcast modes

DVB-T / T2 (classic panels: four constellations across the row), TV (auto), ISDB-T / TSB, DVB-C2, DAB / DAB+, FM, ATSC, and the stream
modes DVB-S/S2, DTMB, ISDB-S (BS), DVB-SH, DVB-C: unchanged (the row is full, the service list fills the column). Mode Builder and plugin
modes are placed by what they draw, like any other.

## Tests

`tests/app/test_overview_layout.cpp` (ctest `overview_layout`): CODAR (empty row hidden, one-line list as a strip, row back 0.25 s after
content appears, glides, list back to a column when it becomes a table), content flickering on and off every frame for 10 s (no change),
on and off every 0.5 s (at most one change), NOAA DSB (plot under the list in a column as wide as the plot, the spectrum full height, the plot
fits its box, stable for 17 s, the column widens when the list becomes a table), a fixed 140 x 160 plot (column) that grows to 600 px wide
(back to a row only as tall as the plot), DVB-T (exactly the 3.9 boxes), a 80 px row (shrinks), content at 60 % of its box (no ratchet),
dragged sizes (kept, row hidden when empty and back at the dragged height), adaptive off (exactly the 3.9 layout), no lower row allowed
(ATSC 3.0, TX), and a sweep of 21 x 12 window sizes from 640 x 400 to 3840 x 2160 with 5 list and 8 lower-row models (every box inside
the area, no overlaps, the spectrum at least its minimum width, nothing moving after 4 s).

Not tested here: the ImGui side (the measuring and the probes) was syntax-checked and built on the GitHub runner, not run: the Mac was on
battery. Screenshots of CODAR, NOAA DSB, FT8, DVB-T and a group 2 mode with `--hidden --mute --shot` and a renamed app copy (isolated
settings) are the next step.
