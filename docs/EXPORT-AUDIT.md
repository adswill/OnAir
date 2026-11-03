# Export audit (feat36-export)

Shared helper: `app/ui/kit/export.h/.cpp/_ui.cpp` (`kit::exportBar`) on top of `resexp` (app/state/results_export): CSV (RFC 4180, UTF-8),
JSON, ADIF, KML, GPX, GeoJSON via the format box of "Save results...", plus "Copy CSV" to the clipboard. KML/GeoJSON write rows that
have `lat`/`lon` columns. Test: `kit_export`, `results_export`.

## Wired in this branch
sonde (table, with positions), APRS packet stations (positions), DMR calls and messages, P25 calls and talkgroups, LoRaWAN devices, GNSS channels.

## Already had an export before
acars, adsb, ais, cell, dab, dmr, fm, freqdb, gnss, gsmr, hfbeacon, hfdl, inmc, iono, keys, lorawan, marine, mesh, modeac, nbiot, nr, p25, packet, pager, panels, prop, qo100, rfi, satbeacon, scan, sonde, specx, tetra, tones, trunkpool, ui, vdl2, voice

## Mode folders with a table or list and no export yet (to wire next, same 10 lines each: build a `resexp::Table`, call `kit::exportBar`)
aero, airband, ale, ant, antd, ardop, argos, asterix, atis, atsc3, atv, ble, bletrk, codar, codec, cubesat, cw, datawell, dect, dgps, dji, dpmr, drm, dstar, dvbs, ebicab, eczas, edacs, elektro, eloran, esubghz, flarm, fsq, fy3, fygeo, gk2a, globalstar, goes, goesdcs, halow, ham, hdr, hfdig, horus, hrpt, inmsat, iptv, iridium, isdbt, ism, js8, kaft, kapp, kavb, kaws, lbj, loraraw, lorasat, ltr, m17, mac, marinevhf, memory, meteor, milhf, mpt1327, mscatter, mt63, multitone, ndb, nfc, nrf24, nxdn, orbcomm, p25p2, packs, pactor, pactorhf, plugin, psk, pskdsp, radar, railway, rid, roshf, same, sarsat, sbas, sigfox, ssbconv, ssdv, sweep, tetrapol, time, transcribe, uat, uplink, vdes, vdl4, vfo, vor, watchlist, wb, wifi, wisun, wmbus, zigbee, zwave

Notes: many of the folders in the last list only draw settings or status tables, not lists of heard items; each needs a look.
Pager, ADS-B, AIS, ACARS, VDL2, HFDL, TETRA, marine DSC, mesh already export.
