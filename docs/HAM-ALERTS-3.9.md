# HF digital and CW: alerts, message history, country column (3.9)

Round-4 items 6, 14 and 35 (scratchpad r4-per-mode list). Branch feat39-r4.

## ham.decode (items 6 and 14)
- New event **ham.decode** (family `ham`). It is sent once for every station heard on:
  - FT8, FT4, FT2, WSPR and the other WSJT modes: once per decode, slot and message. The early decode of a slot is left out because the full decode replaces it.
  - the CW skimmer: once per signal and call every 10 minutes
  - RTTY, PSK and the other keyboard modes: the last callsign in the text, once per mode and call every 10 minutes
- Fields:
  - call, grid, text, mode, band, country, snr_db, dt, audio_hz, dial_mhz, cq, WSPR dbm
  - lat and lon, from the locator. Messages like RR73 are not read as a locator.
  - the logbook's marks: new_dxcc, new_dxcc_band, new_grid, new_grid_band, worked and worked_band
- Events are only built when something wants them: an automation, Integrations or the message database.
- **Message history**: the message database (View > Messages) now also keeps the `ham`, `sensor`, `broadcast` and `image` families. "Search everything heard" and the dossier find "where did I hear LY2XX". A call is stored at most once a minute.
- **JTAlert-style recipes**, off by default:
  - **New DXCC heard**: notify and sound, once per entity every 30 minutes
  - **New on this band**: a CQ from an entity not worked on this band
  - **New grid**
  - **Wanted callsign heard**: put your calls in the list (`call in ['3Y0J', 'P5DX']`), notify and sound
- Sound, desktop notification or push (the ntfy, Telegram and webhook actions) are the existing actions.

## Country and worked-before colour (item 35)
- The CW skimmer table has a **Call and country** column. The focused signal's calls, and the callsign line of RTTY, PSK and the other keyboard modes, use the same display.
- The call is coloured by the logbook's marks: red for a new DXCC, amber for new on the band, blue for not worked, grey for worked before. The entity name follows it, and the tooltip gives the entity, CQ and ITU zones.
- The country file is the Ham view's (built in, or your cty.dat).

## Tests
`tests/ham/test_ham_events.cpp`:
- the marks through the event against a logbook with two QSOs (worked, new on band, new grid, new DXCC)
- the 4- and 6-character locators, RR73 not taken as a locator
- each decode once, CW calls once per signal
- the four recipes on the engine: fire, quiet and the cooldown

CI only. The feed from the FT8, CW and keyboard receivers and the colours were syntax-checked only. The decodes in the tests are made up, not from a recording.
