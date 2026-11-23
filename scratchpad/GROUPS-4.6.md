# Mode list groups, 4.6 (17 groups into 35)

Group numbers: `modegroups::Id` in `app/ui/mode_groups.h`, same order as `kGroupNames` / `kGroupCols` in `app/ui/mode_table.cpp`.
Nothing stores a group number. Saved lists hold group NAMES (hidden groups in modes-shown.txt and settings.conf, unfolded groups in `modeGroupsOpen`):
`modegroups::migrate()` turns an old name into every new name that took over its modes (applied in `modeVisibilityLoad` and `fromText`).
A new group is hidden or open if ANY of the old groups it was made from was.

## New order
TV, Broadcast radio | Aircraft datalinks, Navaids & radar, Drones & light aviation | Maritime | Navigation, Mobile & data satellites,
CubeSats & small sats, Rescue & tracking, Weather sats - low rate, Weather sats - high rate, Weather & sondes, Science & sensing |
Two-way radio, Cellular & rail, Pagers | Smart devices & RFID, Mesh & LoRa, Radio control & drones, Wireless networks, Vehicles & roads |
Amateur - weak signal, text modes, images & data, voice & packet | HF data, HF telegraph & FSK, HF monitoring |
Receivers & analysers, Meters, Interference hunting | Plugins, Kris custom modes, Custom modes.

## Moves beyond the brief's lists
- ELT 121.5 MHz: Aviation -> Rescue & tracking (a mode sits in one group; it is still found by the "Aviation" chip search words).
- Satellite Doppler curve logger, What satellite is this?, PSK demodulator (experimental): -> CubeSats & small sats.
- Arktika-M GGAK: Weather sats - low rate (with its RDAS sibling). MSG Raw Data, HimawariCast, GeoNetCast, AWS L-band, GK-2A UHRIT/CDAS, GOES CDA/MDL: high rate.
- STANAG 4481 FSK: HF telegraph & FSK (the other STANAGs are HF data).
- Weather radar (detect): Science & sensing. Radio Jove (both rows), e-CALLISTO, Schumann, Whistler: moved from Amateur - digital to Science & sensing.
- DGPS beacon, NavIC, SBAS, GNSS interference detector: Navigation.
- Weather & Science sat modes (HRPT, X/S-band dumps...) now also carry the Satellite chip (Weather sats groups: Satellite + Weather).
- README keeps its 14 topic rows: `tools/release/gen_changelog.py` README_GROUPS lists the new groups under each topic.
- Mode Browser group tags: `tagsOfGroup` in `app/ui/mode_picker_logic.h`; every group but the three tool groups and Custom is under a chip.
- Presets (Ham, Broadcast, Aviation, Marine) match group names by words; the word lists got the new names.
