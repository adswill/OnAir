# 3.6 queued modes: triage (2026-11-02, integ-36 at 2c111416b)

Checked against `app/ui/mode_table.cpp`, `docs/MODE-NUMBERS.md`, `core/src/*`, and `git branch -r | grep pub/` (no remote WIP
branch exists for any of the item-21 modes; every one of them was merged into main long ago).

| queued mode | state in integ-36 | where |
|---|---|---|
| FT8 / FT4 / WSPR / FT2 | done, merged | HF digital, family 25 (sub entries), deep decode stage, band monitor |
| Airband | done, merged | family 26 |
| LoRa APRS, MeshCom | done, merged | family 20 sub entries |
| LoRaWAN (monitor and all-channel) | done, merged | family 20 and 200, LoRa satellites 201 |
| HD Radio AM, DRM mode E / DRM+ | done, merged | families 21 and 10 (hdr_ui, drm_rx mode E) |
| GNSS multi-constellation | done, merged | family 12: GPS, QZSS, SBAS (150), Galileo E1, BeiDou B1I, GLONASS, interference monitor |
| Cell (LTE), 5G NR, NB-IoT | done, merged | families 32, 57, 69 |
| DTMB | done, merged | family 7 |
| DVB-C | done, merged | family 40 |
| Spectrum explorer, Spectrum analyser | done, merged | families 41, 42 |
| TETRA, MAC, radio astronomy, ALE, P25 (+ Phase 2), BLE | done, merged | families 46, 47, 48, 49, 50/180, 51 |
| RDS-TMC, Z-Wave, chirp sounder, QO-100, Sigfox, HamDRM, Wi-Fi, ATV, GOES, CubeSat, Meteor | done, merged | various |
| GK-2A, FY-4, FY-2, FY-3, PACTOR-II/III/IV | merged in integ-36, no live signal | 230-239 |
| Inmarsat voice, Iridium voice | integ-36 / own lanes | feat36-inmvoice (merged), feat36-iridvoice (running) |

## Rows still marked WIP in the mode list (the real remaining work of item 21)

| mode | family | what is missing |
|---|---|---|
| SITOR (WIP, table unverified) | 119 | no FSK front end, no engine path, FEC-B table is not ITU-R M.476 (a verified table already exists in marine_navtex.cpp) |
| DGPS beacon (WIP) | 120 | station id / frame counts only, no test against a real beacon |
| DME (WIP) | 121 | pulse-pair channel and ident only, needs a 1 GHz radio |
| eLoran / Loran-C (WIP) | 126 | Loran-C only, eLoran data channel not decoded |
| MPT1327 trunking (WIP) | 127 | control messages, no live capture |
| LTR trunking (WIP) | 129 | check bits shown, not verified |
| EDACS trunking (WIP) | 130 | message layout ASSUMED, not from public sources |
| Yaesu Fusion (WIP) | 128 | voice needs user codec plug-in; no live capture |
| KAVB | 123 | custom mode, owner decides |
