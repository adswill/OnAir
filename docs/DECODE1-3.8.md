# Decoder gap fill, batch 1 (feat38-decode1)

Items owner-approved from the round 3 per-mode decode-gap research. Everything below is parsed bit for bit against the named sources; none of it has run on a live signal in this work.

## AIS

New interpreter `core/src/ais/ais_apps.cpp`, records in the telemetry (`AisTelemetry::apps`), new **Reports** view, map overlay and CSV export.

- DAC 1: FI 0 text, 11 and 31 weather and water (IMO 236 and 289), 12/25 dangerous cargo, 13 fairway closed, 14/32 tidal windows, 15 air draught, 16/40 persons on board, 17 VTS targets, 18 clearance, 19 traffic signal, 20 berthing, 21 ship weather (the WMO coded variant is named, not read), 22/23 area notices (circle, rectangle, sector, polyline, polygon, text) drawn on the map, 24 extended voyage data, 26 environmental, 27/28 route information, 29/30 text.
- DAC 200 inland: 10 static, 21/22 ETA and RTA at locks, 23 EMMA warnings, 24 water levels, 40 signal status, 55 persons on board.
- DAC 235/250 AtoN monitoring; DAC 367 FI 33 uses the IMO environmental report layout, checked against a real sentence.
- Other DAC/FI pairs (including the US Seaway messages 316/366 and the US 366 FI 56 content, which is not public) are listed by name and left uninterpreted.
- Types 7, 10, 12, 13, 15, 16, 17, 20, 22, 23, 25 and 26 now have length checks and readable records. Layouts: gpsd AIVDM document and driver.
- Test: `tests/ais/test_ais_apps.cpp`, 13 real AIVDM sentences from the gpsd regression logs with gpsd's expected values, the rest generated from the layouts, plus truncated and random messages.

## ACARS

`core/src/acars/acars_text.cpp` reads plain-text formats by label and preamble (positions, OOOI, ETA, fuel, faults, winds, ARINC 702 slash reports, loadsheet-free subsets of labels 10, 12, 13-18, 15, 16, 1L, 1M, 20, 21, 22, 24, 2P, 30, 44, 4A, 4N, 4T, 58, 5Z, 80, 83, 8E, B6, :;, H1, H2, HX, QQ, SQ). The reading appears in the existing tables of ACARS, VDL2, HFDL, Aero and Iridium, positions reach their maps.

- libacars: MIT; the vendored subset (FANS-1/A CPDLC) has no label parsers, so it was not the source here. MIAM and OHMA remain out (need zlib and more of libacars).
- acars-decoder-typescript (MIT) was used as a format reference; its code was not copied. Its test suite's example messages are test vectors: 88 of 94 read as it expects. The six misses are airline-specific PRF/LDI/PRG/FPN expectations.
- Left out: SWA compressed and label H1 A02 / 37 / 40 / LDR variants, MIAM, OHMA.

## P25 Phase 1

`core/src/p25/p25_cc.cpp`: Motorola and Harris patches (the follower treats a supergroup grant as a call of every member), affiliations and registrations (Trunking view: units table, export), deny and queue responses with their reason text (call log), data channel grants, MBT converted to TSBK equivalents with CRC-32 checked, unconfirmed PDU reassembly with an IPv4 summary. Layouts: OP25; reason codes: SDRTrunk.

Not done: confirmed data PDUs (rate 3/4 trellis not implemented, shown as a header line); P25 Phase 2 regroup family; talker alias.

## DMR

`core/src/dmr/dmr_vendor.cpp`: Capacity Plus channel status (multi-block) and Connect Plus grants feed the trunk follower (user channel table, as Tier III); both appear in the call list. ARS (radios logging on) and TMS (text messages) from UDP ports 4005 and 4007 appear in the messages list. Layouts from DSD-FME, partly reverse-engineered there: the Connect Plus grant is checked against its published sample; Capacity Plus and TMS against the layouts only.

## TETRA

- D-TX GRANTED transmitting party (EN 300 392-2 table 14.18) keeps the floor holder on the slot; only for calls that say clear.
- MAC-FRAG / MAC-END reassembly (tables 21.57 / 21.59) per timeslot, then the whole TL-SDU is parsed again.
- D-SDS-DATA and D-STATUS (tables 14.13 / 14.14) go to the existing `tetra_sds` parser. Protocol identifier corrected to table 29.21 (2 simple text, 9 immediate text; the earlier 1 is OTAK). New **Messages** view with export, positions on the map's radios layer, events in the log.
- Clear only: nothing is read from air-interface encrypted blocks or end-to-end encrypted calls. No decryption was added.
- LIP short location report layout is unchanged and still unverified against a real capture: ETSI TS 100 392-18-1 could not be fetched.

## What was validated

All of the above only on real example sentences where stated and on generated bit patterns otherwise; none on live signals. Unit tests: `ais_apps`, `acars_text`, `p25_cc`, `dmr_vendor`, `tetra_frag`, Rust `onair_tetra` tests.
