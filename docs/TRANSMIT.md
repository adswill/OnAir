# Transmit

**You are responsible.** OnAir limits nothing: not the frequency, not the power. What you type goes to the radio as typed (the radio's own range is shown as information only). Transmitting needs a licence or permission where you are, and a radio without an antenna, an attenuator or a dummy load can disturb others. Transmit is meant for test signals into a dummy load or a shielded setup, and for your own use. The first time you start, a one-time notice has to be accepted with "I understand".

## How to transmit

Switch the RX / TX slider at the top left to **TX**. The transmit view looks like the receiver: nothing is sent until you press the big Start button.

**The top bar.** The same bar as the receiver's. On the left the run button (start / stop) and the **big frequency digits**, the same widget as the receiver's: click the upper or lower half of a digit to step it, scroll over a digit, type digits while the mouse is over the row, or double-click to type the whole frequency in MHz. Next to the digits a chip says whether the transmit frequency is **TX = RX** (it follows the receiver; typing a different frequency unlinks it) or **TX independent** (its own; the receiver's frequency is shown under it, and a click makes the transmitter follow it). **Presets** lists the well-known frequencies of the chosen signal. On the right: the **output level meter** (what goes to the radio, in dBFS) and the **Level** slider (the radio's own TX gain scale).

**Three panes.** Left: the **signal list** (searchable, grouped like the receiver's mode list, with the last signals as one-click Recent buttons). Middle: the **Basic** block. Right: the **preview**. A narrow window or the pop-out window stacks them in one column.

**Basic.** The state word (**Idle**, **Preparing**, **Transmitting** with a pulsing red dot and the elapsed time, **Test (no RF)**, **Carrier tune**) and the one big button: **START TRANSMITTING** or **STOP**. Space starts and stops, Esc stops (not while you are typing in a text box). For voice signals **Push to talk** turns the button into one that transmits only while you hold it. Errors and notes appear as boxes, never as popups (except the one-time notice). Under the button: **Test (dry run, no RF)** (the generator runs into nothing: no radio is opened or claimed, and the spectrum and levels show what would go out), the **transmit radio** (only radios with a transmit path are listed; a radio a receiver is using is refused in plain words unless it is genuinely full duplex), the radio's range as information, the **output level** (the radio's own TX gain scale, with its stage names; HackRF: TX VGA, and the +14 dB amplifier as its own switch), and **what to send**: the signal's own content (message text, callsign, bandwidth, test pattern, input file, microphone / camera choice) with sensible defaults. Empty fields send the mode's built-in demo signal. While a transmission runs the frequency and the level can still be changed: **Apply frequency and level now** retunes the running radio (a radio that cannot change while sending says so and carries on as before).

**Advanced.** Folded under Basic. Every parameter of the signal, in the sections **Modulation**, **Audio**, **Filters**, **Output** and **Timing**. Each row has a tooltip with what it does, its unit and its default; a row that differs from its default is drawn brighter. **Reset to default** puts every row back; **Save preset** keeps the rows, the frequency and the level under a name for this signal, and the **Presets** list brings them back. The last values of each signal are remembered. The table lives in `app/panels/tx_params.cpp` (one row per parameter: key, label, tooltip, range, default); the values reach the generator as `TxRequest::opt` (keys such as `am.depthPct`) or the request's own fields. What each signal offers:

| Signal | Parameters |
|---|---|
| Airband AM | type (AM / DSB suppressed carrier), modulation depth, carrier level, test tone and a second tone, speech compressor (threshold, ratio, make-up, release), high-pass, low-pass, VOX (threshold, hang time) |
| QO-100 SSB | sideband (USB / LSB), carrier injection, peak level, the same audio, filter, compressor and VOX rows (the DATV path has the TV settings) |
| FM broadcast | deviation, pre-emphasis, stereo and pilot level, RDS on / level / programme type / TP / TA / music flag, left and right test tones and levels, RF level |
| Digital voice and scene signals with "send analog NFM instead" | deviation, pre-emphasis, CTCSS tone and level, RF level, the same audio, filter, compressor and VOX rows |
| FT8 / FT4 / WSPR / JT / Q65 / FST4 / MSK144 | audio frequency, sideband |
| Olivia / Contestia / MFSK / THOR / DominoEX / Hell | centre frequency, sideband |
| CW, PSK | pitch / audio frequency |
| Every signal | output trim, mirror the spectrum (swap I and Q), key-up fade-in, frequency offset, repeat or one pass of N seconds |

The signal-specific content (callsign, text, IDs, files, services, test pictures, baud and shift tables) stays under Basic.

**Preview.** A live spectrum and waterfall of what will be sent (made in the background a moment after each change) or, while running, of the IQ really handed to the radio. Under it: **Peak** and **RMS** meters in dBFS, a **Clipping** lamp (red when the limiter has to work: lower the level), the **Sample rate** the radio will use and the estimated **Occupied bandwidth** (99% of the power), and an IQ scatter. **Carrier tune (hold)** sends an unmodulated carrier while the button is held, for adjusting drive, filter and SWR. **STOP ALL** stops the transmission, the test, the carrier and any pending start and gives the radio back. The log below lists what was sent and what happened.

**Pop out.** The small button in the top right corner moves the whole panel into a window of its own.

**Level safety.** Every block passes a soft limiter just before the radio: samples up to 0.9 of full scale pass untouched, above that they are rounded smoothly towards 1.0 and never exceed it, and a NaN or infinity becomes zero. The HackRF's 8-bit conversion clips at plus or minus 127. The limiter is not a power limit: it only keeps the generated signal inside the converter's range.

**Map upload** stays suspended while transmitting.

## Modes offered

This list is taken from the tables in `core/src/tx/tx.cpp` (`txModes()`).

### Modes offered (99)

| Id | Mode | Engine code | Generator |
|---|---|---|---|
| dvbt2 | DVB-T2 | 0 | T2Generator, real PLP with the test transport stream |
| dvbt | DVB-T | 0 | dvbt::Generator, demo programme |
| atsc | ATSC 1.0 (8-VSB) | 3 | atsc::Generator, demo programme |
| dab | DAB / DAB+ | 4 | makeDabSynth, transmission mode I with real audio |
| isdbt | ISDB-T | 6 | ISDB-T layers A and B with the demo programme |
| dvbs | DVB-S / S2 / S2X | 8 | makeDvbsSynth (dvbs_tx.h chain), demo programme |
| dtmb | DTMB | 9 | makeDtmbSynth (dtmb_tx.h chain), demo programme |
| adsb | ADS-B / Mode S | 13 | makeAdsbSynth, Mode S frames of simulated aircraft |
| atsc3 | ATSC 3.0 | 5 | makeAtsc3Synth, in-memory test programme |
| fm | FM broadcast | 7 | FmGenerator, stereo multiplex with RDS |
| atv | Analog TV | 10 | makeAtvSynth, test card and sound carrier |
| dmr | DMR | 11 | makeDmrSynth |
| drm | DRM | 12 | makeDrmSynth |
| sonde | Radiosonde | 15 | makeSondeSynth |
| ais | AIS | 16 | makeAisSynth |
| marine | Marine (NAVTEX / DSC / weather fax) | 17 | makeMarineSynth |
| acars | ACARS | 18 | makeAcarsSynth |
| inmc | Inmarsat-C | 19 | makeInmcSynth |
| aero | Inmarsat Aero | 20 | makeAeroSynth |
| iridium | Iridium | 21 | makeIridiumSynth |
| mesh | Mesh (LoRa) | 22 | makeMeshSynth |
| hdr | HD Radio | 23 | makeHdrSynth |
| cdr | CDR | 24 | makeCdrSynth |
| pager | Pagers (POCSAG / FLEX) | 25 | makePagerSynth, FM on a 25 kHz channel |
| packet | APRS / Packet | 26 | makePacketSynth |
| rtty | RTTY | 27 | makeRttyTxAudio / makeHfdigSynth |
| sstv | SSTV | 27 | makeSstvTx, PPM or test picture with callsign stamp |
| freedv | FreeDV | 27 | makeFreedvTestAudio |
| ft8 | FT8 | 27 | makeFtxTxAudio |
| ft4 | FT4 | 27 | makeFtxTxAudio |
| wspr | WSPR | 27 | makeFtxTxAudio |
| ft2 | FT2 | 27 | makeFtxTxAudio |
| hamdrm | HamDRM | 27 | makeHamdrmTx, file bytes and callsign |
| jt65 | JT65 | 27 | makeFtxTxAudio |
| jt9 | JT9 | 27 | makeFtxTxAudio |
| q65 | Q65 | 27 | makeFtxTxAudio |
| fst4 | FST4 | 27 | makeFtxTxAudio |
| msk144 | MSK144 | 27 | makeFtxTxAudio |
| olivia | Olivia | 27 | makeKbdTx |
| contestia | Contestia | 27 | makeKbdTx |
| mfsk | MFSK | 27 | makeKbdTx |
| thor | THOR | 27 | makeKbdTx |
| dominoex | DominoEX | 27 | makeKbdTx |
| hell | Hellschreiber | 27 | makeKbdTx |
| airband | Airband AM | 28 | makeAirbandSynth, AM voice channels |
| vdl2 | VDL Mode 2 | 29 | makeVdl2Synth |
| vdl4 | VDL Mode 4 | 84 | makeVdl4Synth |
| uat | UAT 978 | 85 | makeUatSynth |
| wisun | Wi-SUN | 86 | makeWisunSynth |
| eczas | e-Czas 225 kHz | 87 | makeEczasSynth |
| fpv | FPV video (analog) | 103 | makeFpvSynth |
| wmbus | Wireless M-Bus | 88 | makeWmbusSynth |
| mt63 | MT63 | 97 | makeMt63Synth |
| fsq | FSQ | 115 | makeFsqSynth |
| ifkp | IFKP | 118 | makeIfkpSynth |
| cquam | AM stereo (C-QUAM) | 109 | makeCquamSynth |
| ardop | ARDOP | 109 | makeArdopSynth |
| zwave | Z-Wave | 100 | makeZwaveSynth |
| mscatter | Meteor scatter | 106 | makeMscatterSynth |
| goesdcs | GOES DCS | 109 | makeGoesDcsSynth |
| modeac | Mode A/C | 107 | makeModeAcSynth |
| bletracker | BLE tracker detector | 109 | makeBletrkSynth |
| satbeacon | Satellite beacon | 109 | makeSatbeaconSynth |
| vorils | VOR / ILS | 32 | makeVorSynth |
| m17 | M17 | 33 | makeM17Synth |
| dstar | D-STAR | 35 | makeDstarSynth |
| cw | CW | 36 | makeCwSynth |
| hfdl | HFDL | 37 | makeHfdlSynth |
| psk | PSK31 | 38 | makePskSynth |
| js8 | JS8Call | 39 | makeJs8Synth |
| nxdn | NXDN | 40 | makeNxdnSynth |
| dvbc | DVB-C | 42 | makeDvbcSynth |
| uplink | Mode S uplink | 45 | makeUplinkSynth |
| orbcomm | Orbcomm | 46 | makeOrbcommSynth |
| dpmr | dPMR | 47 | makeDpmrSynth |
| tetra | TETRA | 48 | makeTetraSynth |
| mac | MAC | 49 | makeMacSynth |
| ale | HF ALE | 51 | makeAleSynth |
| p25 | P25 | 52 | makeP25Synth |
| qo100 | QO-100 | 55 | makeQo100Synth |
| meteor | Meteor-M LRPT | 61 | makeMeteorSynth |
| goes | GOES HRIT | 62 | makeGoesSynth |
| cubesat | CubeSat telemetry | 63 | makeCubesatSynth |
| globalstar | Globalstar | 64 | makeGlobalstarSynth |
| vdes | VDES | 65 | makeVdesSynth |
| hrpt | HRPT / AHRPT | 66 | makeHrptSynth |
| zigbee | Zigbee / 802.15.4 | 67 | makeZigbeeSynth |
| flarm | FLARM | 68 | makeFlarmSynth |
| hfbeacon | HF beacons | 69 | makeHfbeaconSynth |
| dji | DJI drones | 70 | makeDjiSynth |
| loraraw | LoRa (raw) | 73 | makeLoraRawSynth |
| same | NOAA Weather Radio SAME | 78 | makeSameSynth |
| dect | DECT cordless phones | 80 | makeDectSynth |
| gnss | GNSS | 14 | makeGnssSynth |
| ism | ISM sensors | 30 | makeIsmSynth |
| cell | Cellular (LTE) | 34 | makeCellSynth |
| gsmr | GSM-R | 41 | makeGsmrSynth |
| specx | Explorer | 43 | makeSpecxSynth |
| specan | Spectrum analyser | 44 | makeSpecanSynth |
| astro | Radio astronomy | 50 | makeAstroSynth |
| ble | Bluetooth LE | 53 | makeBleSynth |
| iono | Ionospheric sounders | 54 | makeIonoSynth |
| wifi | Wi-Fi | 56 | makeWifiSynth |
| general | General receiver | 57 | makeGeneralSynth |
| rid | Drone Remote ID | 58 | makeRidSynth |
| nr | 5G NR | 59 | makeNrSynth |
| timesig | Time signals | 60 | makeTimeSynth |
| nbiot | NB-IoT | 71 | makeNbiotSynth |
| halow | Wi-Fi HaLow | 72 | makeHalowSynth |
| codar | CODAR | 74 | makeCodarSynth |
| datawell | Datawell | 76 | makeDatawellSynth |
| multitone | Multitone paging | 77 | makeMultitoneSynth |
| ant | ANT+ | 79 | makeAntSynth |
| tetrapol | TETRAPOL | 81 | makeTetrapolSynth |
| ndb | NDB beacons | 31 | makeNdbSynth |
| elektro | Elektro-L | 82 | makeElektroSynth |

### Modes without a generator

Nothing is refused: OnAir blocks no mode, frequency or power. A mode can only be missing when it has no signal generator (for example the signal workbench, engine code 75); the panel then says "this mode has no transmit generator". Limits of the radio (no transmit path, a sample rate too low for the mode) are reported as they come.

### Hellschreiber text in other scripts (#225)

Hellschreiber sends letters, digits, accented Latin, Cyrillic and Greek from its built-in glyphs. For Chinese, Japanese, Korean and symbols, pick a
font file (`.ttf` / `.otf`) under Advanced in the transmit panel of Hellschreiber ("Advanced: font for other scripts"). OnAir draws the characters the
built-in glyphs lack from that file, 10 pixels high inside the 14-pixel column (the receiver finds the column start from the blank rows above and
below), at most 16 columns wide. Characters the font does not have stay `?`. No font is bundled: download one (GNU Unifont or Noto Sans CJK, from their
own sites) and keep its licence. The choice is remembered (setting `hellFont`). The built-in glyphs always win for ASCII, Latin-1, Cyrillic and Greek,
so the fldigi-compatible picture is unchanged. Transmitting is still behind Arm, the licence box and the band plan.
