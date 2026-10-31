# Inmarsat (all carriers)

Status: new in 3.3, **needs a user test with a real capture**. Everything below was checked on synthetic signals only.

One mode that finds and decodes every Inmarsat L-band carrier of a wide capture at the same time. It sits beside the single-channel modes **Inmarsat Aero** and **Inmarsat-C**, which stay as they are.

## Use it

1. Pick **Inmarsat (all carriers)**, tune to the middle of your satellite's L-band channels (about 1545.6 MHz) and use a sample rate of **6 Msps or more** if your radio does it. An L-band antenna (patch or helix, with an LNA) is needed.
2. Press nothing: the search runs by itself (**Auto**; the Auto button forgets the carriers and searches again).
3. **Carriers** shows the whole capture as a strip and a table: frequency, type, bit rate, level, Eb/N0, state, last activity, ocean region for Inmarsat-C. Click a row to focus it (a focused carrier is never parked).
4. **Messages** lists ACARS and ADS-C from all P channels and Inmarsat-C EGC broadcasts, with a filter. **Aircraft** shows the aircraft with a position on a map (reports come every few minutes; see below). The screen opens on it by itself the first time a position is decoded, and the header always says how many aircraft have one (click it to jump there). **Voice** is described below.

## Where the aircraft are

Aero P channels carry only what an aircraft reports about itself, so planes appear slowly: an ADS-C position report every few minutes (a default contract is 3 to 15 minutes), and ACARS messages with a position in the text. Until the first one arrives the map says "waiting for position reports". That is normal, not a fault.

Positions are taken from every source the receiver decodes: ADS-C reports (basic, with the earth and air reference groups for speed and track, and the predicted route), ARINC 702 "POS" and "/PS" position reports (also the position inside weather and flight reports that use that field), label 20 and label 16 / 12 reports. Log-on and log-off messages give an aircraft's ICAO address and ground station but no position; CPDLC position reports are ASN.1-encoded and are not decoded yet. Positions outside the globe (latitude beyond 90, longitude beyond 360, or exactly 0, 0) are dropped; a longitude past 180 wraps to the other side.

On the map each aircraft is an arrow (the way it was heading; a dot when no direction is known) labelled with flight or registration, flight level, ground speed, track and the age of the position. It fades as the position ages: solid for 5 minutes, faint after 40. The line behind it is its trail, the thin line ahead the route in its last ADS-C report. Click an aircraft, or a row of the table under the map, for the ICAO address, position, source, the satellite and carrier (frequency and bit rate) it was heard on, and its recent messages. The table sorts on flight, altitude, speed, age and message count and has a search box (flight, registration, ICAO hex, report type).

Without network access (or with the online map off) the map shows coastlines and a labelled latitude / longitude grid from the data built into OnAir, so it is never blank. Tiles, when used, only send tile numbers, never a position. The "?" on the map says the same in the app. Needs a user test with a real capture.

## Which satellite

The mode names the satellite by itself, from three independent clues that are scored together: the **decoded content** (the Aero P channel system table gives the satellite id, its longitude and spot or global beam; the Inmarsat-C bulletin board gives the ocean region), the **frequency fingerprint** (the set of P carriers against the published channel list of each satellite), and the **geometry** (where each geostationary slot stands in your sky from your location; a satellite below your horizon cannot be the one you hear). The Carriers tab shows the satellite, its longitude, the beam type, a confidence and the evidence behind it; every Aero carrier row gets the satellite name. Choose one under Advanced to override. Set your location under View > Map. The satellite tables are unverified: they come from public channel lists (credit: github.com/alphafox02/inmarsat-sniffer) and nominal ocean-region slots, and the system-table reading is JAERO's, so this **needs a user test with a real capture**. A TLE is not used (geostationary slots only); the beam's own name is not in the signal we decode.

## What radios do

| Radio | Sample rate | What it covers |
|---|---|---|
| RTL-SDR | 2.4 Msps | about 2 MHz: one P group or a few carriers |
| Airspy, SDRplay | 6 to 10 Msps | both P groups and neighbouring carriers |
| HackRF, HackRF Pro | 10 to 20 Msps | most of one satellite's downlink |

The 34 MHz downlink (1525 to 1559 MHz) never fits in one capture. Retune to see another part.

## What is decoded

- **Aero P channels** at 600, 1200 and 10500 bit/s (ACARS, ADS-C, log-ons, positions): all carriers at once, one window of about 160 kHz per group of carriers, cut from the capture with a single shared FFT.
- **Inmarsat-C** (EGC safety and news broadcasts) on the 2 kHz channels, with the ocean region and station from the bulletin board.
- A **CPU budget** (Advanced) limits the windows. Carriers that do not fit are listed as *parked: CPU*.

## What is not decoded

- **Aero C channels (voice).** They are listed ("may be a voice (C) channel") but cannot be demodulated yet: the C-channel modulation and framing are not in a public specification OnAir may reproduce. The voice mixer (per-carrier mute and solo), the call recorder and the codec hook are built and tested with test frames, but nothing feeds them from the air. This needs a user test with a real C-channel capture.
- **R and T channels** are aircraft-to-satellite signals on 1626.5 to 1660.5 MHz. A ground receiver sees them only from nearby aircraft; they are not decoded.
- **Voice codec.** Aero voice uses an AMBE-family codec. The codec is never shipped: OnAir never includes, downloads or links it: you build or install the plug-in yourself (`onair-codec-ambe`, see [CODECS.md](CODECS.md)). Until then the voice stays silent; calls are counted and their coded frames are saved. Only unencrypted traffic is ever played.

## How it relates to other tools

JAERO decodes one Aero channel at a time. InmarScope shows and decodes several channels. OnAir finds the carriers itself, decodes all of them in parallel and puts messages and aircraft from every channel in one place. Details and open questions: [INMARSAT-REVAMP.md](INMARSAT-REVAMP.md).

Carriers view: the line above the table counts the decoded P channels per rate (600, 1200, 10500 bit/s). A wide carrier that no decoder holds yet shows `~10500` in the Rate column (a guess from its width); it turns into a plain `10500` when frames lock. 10500 bit/s carriers are wide (about 10 kHz) so they look weaker on the spectrum than the narrow ones: the receiver gives them priority when it picks carriers.
