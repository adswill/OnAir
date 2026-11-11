# DAB transmitter lists (Map tab)

Every DAB transmitter sends its own identification (TII: MainId / SubId) in the null symbol, never where it stands. OnAir lists every
transmitter it hears, on every channel, on the DAB **Map** tab. To put one on the map it needs its position, from (the first that has it wins):

1. **Your own list**: `dab-transmitters.csv` in OnAir's data folder (the **open folder** button).
2. **The shared lists**: `dab-tii/<country>.csv` in [adswill/OnAir-channels](https://github.com/adswill/OnAir-channels), crowd-sourced and
   public domain (CC0). OnAir fetches the list of the country set on the Map tab and the list of the country of every ensemble it hears (the
   EId's country nibble and the ECC of FIG 0/9). A list is fetched at most once a day and kept for offline use. A country nobody has shared
   a transmitter for has no list yet, and the tab says so ("nobody has shared a transmitter for DE yet"), which is a different message from
   "could not be downloaded (no network?)".
3. **qt-dab's list**, if you have one (below).
4. **The ensemble itself**: FIG 0/22 (EN 300 401 up to V1.4.1) carries the position of each MainId's reference point and offsets for the
   SubIds. Few networks still send it (later editions of the standard dropped it). A SubId whose own offset is not sent is shown at its
   MainId's reference point as an approximate position (a ring on the map, `~` before the distance). FIG 0/11 region definitions, when
   sent, are shown as the transmitter's region (with its FIG 1/3 name).

A transmitter with no known position is listed with the reason (no list for the country yet, the list has no transmitter of this
ensemble, the ensemble sends no FIG 0/22) and what you can do. **share...** on its row is optional.

## Germany and other countries without a list

OnAir does not ship transmitter lists. For Germany no source with TII codes allows copying: FMLIST's data is for personal use only,
ukwtv.de and wohnort.org are copyrighted sites without a licence, and the Bundesnetzagentur's transmitter data (for the closed user group)
has no TII field. So the shared list for DE starts empty and fills as users share transmitters they know.

## qt-dab's list

qt-dab keeps a transmitter list (from FMLIST) as `.txdata.tii` in the home folder; the qt-dab repository has a copy as
`helpers/tiiFile.zip` (unzip it). OnAir can read that file where it is:

- when it finds `~/.txdata.tii` (on Windows `C:\Users\<you>\.txdata.tii`), the Map tab offers **use it**;
- or pick the file with **import list...**.

The data is FMLIST's and for personal use: OnAir reads it in place, does not copy it into your own list, leaves its positions out of
**export heard**, and does not let it reach the shared list. **stop using it** switches it off.

## The format

One transmitter per line, comma separated (a tab-separated file works too). A line starting with `#` is a comment; a first line starting
with `eid` is the header. Columns after `channel_mhz` are ignored, so an export of heard transmitters can be used as it is.

```
eid,main,sub,lat,lon,site,power_kw,channel_mhz
ABCD,1,3,47.00000,11.00000,Example site,10,178.352
```

(The line above is an example of the layout, not a real transmitter.)

| column | meaning |
| --- | --- |
| `eid` | ensemble id, hexadecimal (the export of heard transmitters has it) |
| `main` | TII MainId, 0 to 69 (the pattern) |
| `sub` | TII SubId, 0 to 23 (the comb) |
| `lat`, `lon` | position in decimal degrees, WGS 84 (north and east positive). A row without both is skipped |
| `site` | the site's name; in double quotes when it holds a comma |
| `power_kw` | ERP in kW, optional |
| `channel_mhz` | the channel's centre frequency in MHz, optional (for people reading the list) |

TII codes are unique within an ensemble, not worldwide, so a transmitter is found by **(eid, main, sub)**. A site that carries several
ensembles needs one line per ensemble, each with that ensemble's id and the codes heard on it.

## Filling a list from what you hear

1. Run a band scan on the Scan tab (it waits for TII on every channel) or leave the receiver on the channels you want.
2. **export heard** on the Map tab writes `dab-heard-<time>.csv` to the data folder: every transmitter heard, in the columns above plus
   `ensemble` and `level_db`, with the position already known where there is one (not from qt-dab's list).
3. Fill in `lat`, `lon` and `site` for the transmitters you know (a spreadsheet works; keep it as CSV).
4. **import list...** adds every row with a position to your own list: a transmitter already there is updated, rows without a position are
   skipped, the other lines of your file stay as they were.

To share them, press **share...** on a row of the Map tab (it opens the OnAir-channels issue form with the ids filled in) or attach the file
to an issue there. Only add positions you know yourself or take from a source that allows it (your own survey, an operator's public
press release, an open data portal with a free licence; not FMLIST, qt-dab's list, ukwtv.de or wohnort.org); submissions become public
domain.

## A far transmitter in qt-dab

qt-dab does not decode FIG 0/22 (its FIG decoder marks it obsolete). The names and places it shows come from its local list, looked up
by (EId, MainId, SubId) of a TII code it detected. A transmitter 400 km away in that list means the code was detected on the air (Band III
can travel that far, mostly with tropospheric lift) or, at a low level, a false detection. Transmitters do not send data about other
transmitters, apart from FIG 0/22 where a network sends it.
