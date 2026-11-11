# Lane B group 3: unique mode-specific features (4.0)

Pure logic in dect2/<mode>/ headers, tests in tests/feat40c (simulated data only; nothing here was checked against a live signal). UI wiring is noted per item.

## AIS
- **Coverage polar plot** (ais/ais_coverage.h `AisCoverage`): farthest and typical (90th percentile) range per bearing sector, plus "blind sectors" (empty sector between two heard ones). Why: shows where the antenna really reaches and where a mast or hill hides ships. Core only; plot not wired.
- **Ship type mix** (`AisTypeMix`): ships by class (cargo, tanker, passenger, fishing, tug, sailing, ...). Why: "what is out there" at a glance.
- **Name cache** (`AisNameCache`): remembers name / call sign / type / length per MMSI from static messages, merging message 24 parts, never erasing a known field with a blank. Why: position reports carry no name; rows get names as soon as any static message was heard. Core only.

## Airband
- **ATIS letter tracker and VOLMET stations** (airband/airband_atis.h): reads the information letter from a transcript, marks ATIS channels (labelled, or two broadcasts with a letter), reports "new ATIS" when the letter changes and its age; lists the airports named in a VOLMET read-out (ICAO codes, or spelled phonetically). Why: tune the ATIS channel and see Kilo became Lima without listening to the loop. Auto-finding active AM channels already exists (AirbandBandWatch), not rebuilt. Needs a transcript source; core only.

## Pagers (POCSAG / FLEX)
- **Keyword alerts** (pager/pager_watch.h `PagerKeywords`): whole-word and phrase match, `*word` matches inside words; returns the words that hit. Why: ring once for "structure fire" and ignore the rest.
- **Automatic capcode groups** (`PagerGroups`): capcodes paged together at least 3 times within 20 s are one group (a station's pagers). Roll calls of more than 60 capcodes are ignored. Why: builds the unit list without typing it in.

## Amateur text modes (CW, PSK, RTTY, Olivia, MFSK, JS8)
- **Callsign extraction and CQ parser** (ham/ham_callsign.h): callsigns in decoded text, "CQ DX DE ..." and "A DE B" lines, the signal report, and a heard list. Why: click a station to start a log entry.
- **Mode guess** (ham/ham_modeguess.h): CW / PSK31-125 / RTTY (shift and baud) / FT8 / FT4 / WSPR / Olivia / MFSK16 from the measured bandwidth, tones, spacing, rate and slot. Why: click a trace and the decoder opens already set. Guess only, the decoder's lock decides.
- **Spot-based propagation summary** (ham/ham_spotsum.h): per band over the last half hour, spots, median and best SNR, farthest, and open directions (8 sectors), as one line. Why: "20 m open to SE, 5500 km" without reading the list; band_open.h covers hour-of-day history, this is the present.

## Tones, selcal, DTMF
- **Tone system identification** (tones/tone_ident.h): from heard tones says CTCSS (with the tone), DTMF, a five-tone plan (ZVEI1 / CCIR / EEA / EIA / DZVEI, told apart by tone length where the tones are shared) or SELCAL. Why: open the right decoder on an unknown channel.

## ISM devices
- **Burst classifier** (ism/ism_classify.h): from pulse and gap lengths gives PWM / PPM / Manchester / PCM, short and long lengths, row separator, repeated rows, bits per row and an rtl_433 style flex line. Noise-only bursts are not claimed (at most 5 % of 200 random bursts passed 0.8 confidence in the test). Why: a protocol author gets the numbers instead of a pulse list.

## ADS-B
- **Orbit / holding detector** (adsb/adsb_loiter.h): finds aircraft flying circles or racetracks from the position fixes (signed heading change added up over 15 min, speed, loop radius, centre, direction). Not triggered by a straight route with a bend, GPS jitter, a parked aircraft or a very wide loop (all in the test). Why: holding patterns, survey and patrol flights and helicopters over an incident are the interesting tracks, and the 3.9 manoeuvre alerts only react to a steep turn right now. Core only; not wired to the map.

## FT8 / FT4 / JS8 / WSPR
- **Clear TX frequency finder** (ham/ham_clearfreq.h): widest free gap in the passband for the even and the odd slot and the quieter slot parity, from the last decodes. Why: calling on top of another station wastes the slot.
- **Band-hop schedule** (ham/ham_bandhop.h): for an unattended receiver on one SDR: the band with the most spots in the last hour gets the slots, every band is still visited at least every 30 min (an opening starts on a quiet band), a minimum dwell protects a decode from being cut, excluded bands are skipped. Simulated: 4 h on 4 bands, silent band visited 32 times, busy band 896 slots, no dwell under 4 slots.

## Cospas-Sarsat
- **Beacon incidents** (sarsat/sarsat_incidents.h): bursts grouped by beacon ID with count, median repeat interval (a real beacon repeats about every 50 s; a 5 s echo is flagged as not plausible), self-tests kept apart, position drift, and an "alert" rank. Why: an annual test or a decoder echo must not look like a distress. Core only.
