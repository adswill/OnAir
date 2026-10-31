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
