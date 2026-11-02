# Radio ID names (DMR and NXDN)

DMR and NXDN calls carry only a number. With a radio ID list loaded, OnAir shows the name next to it:
`1234567  W1ABC Joe Smith` in the DMR call log, slot cards and messages, and in the NXDN current call, call log and unit list.

## Where to get the list

OnAir does not ship the list: it is community data with its own terms.

| Mode | File | Where |
|---|---|---|
| DMR | `user.csv` (about 25 MB) | https://radioid.net/static/user.csv (the Database > Download page of radioid.net) |
| NXDN | `nxdn.csv` | https://radioid.net/static/nxdn.csv |

Use it as the site allows (amateur radio use). Your own list works too.

## Using it

Open the mode's Receiver tab, "Names from a radio ID list":

* **Download** fetches the file when you click (never by itself) and keeps a copy in the data folder (`radioid/dmr.csv`, `radioid/nxdn.csv`), read again at the next start.
* **Import file ...** takes a CSV you saved yourself.
* **Unload** removes the list and the kept copy.

## File format

Comma, semicolon or tab separated, quotes and CRLF allowed, UTF-8 BOM ignored. A header line names the columns (case and punctuation ignored):
radio id / id / dmr id / nxdn id, callsign / call, first name / name, last name / surname, city, state / region, country.
Without a header the order is id, callsign, first name, last name, city, state, country. Rows without a numeric id are skipped and counted.
A repeated id keeps the later row.

## Privacy

The names are looked up when the screen is drawn and are not written to recordings, exports or session files (the session store keeps ids only, hashed where the privacy rules say so).
Call logs of the DMR location (LRRP) map are not annotated.

Code: `core/include/dect2/common/radioid_db.h`, `app/voice/radioid_ui.cpp`, test `tests/radioid/test_radioid.cpp`.

## Watch list and Watch view (all voice modes)

DMR, P25, NXDN and TETRA have a **Watch** sub-view (Advanced). Type a talkgroup (`TG 91`), a radio ID (`3120123`), a range (`100-199`), a callsign or a prefix (`LY*`) and press **Watch**; the entry follows the target in this mode or, with **All voice modes**, in every voice mode. Per entry: **Notify** (desktop notification and banner when a call starts, off by default), **Beep** (with Notify, never when muted) and **Record** (keeps an auto-record rule for it and switches auto-record on; encrypted calls are never recorded). Names from the radio-ID list and talkgroup labels show in the history. The History table lists the active and ended calls the monitors still hold (time, mode, from, to, seconds, flags, recording file) and copies as CSV. In DMR each entry also shows the last LRRP GPS position of the radio (or of a radio that sent to the talkgroup) with **Show on map**. Core: `core/include/dect2/voice/watch.h` (test `voice_watch`); window: `app/voice/watch_ui.cpp`. The list is saved in the preference `voiceWatch`. Trunk-controller code is not touched.
