# DAB Map tab: transmitter positions and zoom (3.9)

Report from a German user (OnAir 3.8.0, Windows, PlutoSDR, DR Deutschland on 5C, SNR 27 dB): six TII codes decoded, none placed, every
row offering only "submit", the map's zoom not working.

## Root causes

1. **No list for Germany.** `dab-tii/DE.csv` does not exist in adswill/OnAir-channels (HTTP 404 on 2026-11-11; only AU, FR, GB and IT
   have lists). curl reports a 404 like a network failure, so the tab said "no shared list for DE yet (or no network)". DR Deutschland sends
   no FIG 0/22, so nothing else gave a position.
2. **Zoom.** When no transmitter had a position but the antenna position was set, the tab set the map's centre and zoom 8 again every
   frame (the `fitted` flag was never set in that branch), undoing + / - and the wheel. The sonde map has no such reset. The Cell map had the
   same bug (fixed). The follow-mode maps (ACARS, AIS, aero, D-STAR, FT8, GNSS, HFDL, Horus, Inmarsat, Iridium, JS8, M17, Orbcomm, packet,
   RID, SAME, SARSAT, sonde, VDL2) stopped following only on a pan; a wheel zoom at the map centre or + / - kept following and could be
   undone by the next fit. They now stop following on a zoom too.
3. **FIG 0/22 TD** was read as 10 bits; EN 300 401 V1.4.1 figure 53 gives 11 (0 to 2047 us). Positions were right (coarse + fine,
   two's complement, 90 / 2^19 and 180 / 2^19 per LSB; SubId offsets added to the MainId's reference), checked against the spec text.

## Claim (b): "a transmitter sends data about other transmitters"

Not so. qt-dab's FIG decoder skips FIG 0/22 as obsolete (`fib-decoder.cpp`, `case 22: // Obsolete`), and its tii-mapper looks up a local
database by `(EId << 16) | (MainId << 8) | SubId`. The Berlin row is that database's entry for a TII code qt-dab detected: real long-range
reception (Band III over 400 km is possible with tropospheric lift) or, at a low level, a false detection. The only in-band transmitter
data in DAB is FIG 0/22 (and FIG 0/11 regions), which DR Deutschland does not send: OnAir found none.

## Data sources checked (2026-11-11)

| source | TII codes | licence / terms | used |
| --- | --- | --- | --- |
| FMLIST (also inside qt-dab's `.txdata.tii`, AbracaDABra's and qt-dab's closed libraries) | yes | personal use only; qt-dab's TII library forbids passing it on | no: the user's own qt-dab file can be read in place |
| ukwtv.de | yes | no licence, all rights reserved | no |
| wohnort.org | yes | "© 2026 wohnort media", no licence | no |
| Bundesnetzagentur (DAB format sheet, 3 July 2025) | no TII field | data only for a closed user group | no |
| Wikipedia, Wikidata, OpenStreetMap (taginfo: no TII key) | no | CC BY-SA / CC0 / ODbL | no TII codes to use |
| Media Broadcast, Antenne Deutschland, dabplus.de | site lists, no TII | press releases | no TII codes |
| Ofcom TxParams (UK) | probably | OGL v3 (allows reuse with credit) | not fetched: the download sits behind a bot check |

So no shipped list for Germany is possible: **0 rows bundled**, no coordinates or TII mappings invented. The shared DE list fills as users
share transmitters they know.

## What changed

- Map tab: centres once; + / - and the wheel work with or without placed transmitters.
- The ensemble's country (EId nibble + ECC of FIG 0/9) picks the shared list to fetch, in addition to the country box; every cached
  country list is used (border reception). A 404 is told apart from no network (a second fetch of the repository's README).
- Rows without a position: "position unknown" with the reasons in a tooltip, and a box above the table per ensemble: the list's state for
  the country, how many transmitters of this EId the lists know, whether the ensemble sends FIG 0/22, the FIG 0/11 region if sent; then
  what to do. "submit" is now "share..." and marked optional, with a "?" help on where lists come from.
- A SubId whose own FIG 0/22 offset is not sent is placed at its MainId's reference point as approximate (ring marker, `~` distance),
  and left out of the SFN delay matching.
- qt-dab's `.txdata.tii` (scrambled or plain): offered when found in the home folder, or via "import list..."; read in place, never
  copied, exported or submitted (FMLIST data, personal use). Format read from qt-dab's GPL-2.0-or-later source, credited in THIRD_PARTY.md.
- Core: `DabFicInfo::regions` (FIG 0/11 TII lists and rectangles, FIG 1/3 labels), TD 11 bits, `TxConfig::extraFigs` for tests.

## What a German user now sees

The map zooms. The rows show "position unknown" and, above the table: "Nobody has shared a transmitter for DE yet: the OnAir-channels
repository has no list for it. No list in use knows any transmitter of this ensemble (EId 10BC). The ensemble does not send transmitter
positions (FIG 0/22) ..." then "qt-dab's transmitter list was found on this computer: use it" (the user has qt-dab), after which the
transmitters that list knows are placed with distance and bearing.

## Distance from the impulse response

Skipped. An SFN path delay is (d_i - d_ref) / c plus each transmitter's static delay, which German networks set and do not publish (FIG
0/22's TD would carry it, but is not sent). Which path belongs to which TII code is also ambiguous. An estimate from it would be wrong by
up to tens of km without saying so.

## Tests (simulated only)

- `dab_fig`: FIG 0/22 TD 1500 us (11 bits) and the analyser text; FIG 0/11 TII list + rectangle + FIG 1/3 label; country from EId + ECC
  (DE, GB, NL, CH); a simulated ensemble (EId 10BC, ECC E0, four TII codes at 0 / -3 / -6 / -9 dB, TII every frame, FIG 0/9, 0/11, 0/22,
  1/3) through the receiver at 20 dB SNR, +800 Hz, 8-bit samples, matched as the Map tab does: 2 exact, 1 approximate, 1 from a list.
- `dab_txlist`: qt-dab list plain and scrambled (keys 0x07, 0xAA, 0x30), skipped rows, matching precedence and key separation.
- quick.yml (Linux, builds dect2 too): dab_fig, dab_txlist, dab_tii passed, https://github.com/adswill/OnAir/actions/runs/42970713929.
- No real German capture with FIG 0/22 exists; the zoom fix is checked by code review, not by a GUI run (battery: no local build).

## Left for the owner

- OnAir-channels (server side, not changed): add `dab-tii/DE.csv` (header only) and the same for other countries, so a fetch returns
  200 with zero rows; the probe then is not needed. Consider an `import` script for Ofcom's OGL TxParams DAB file for GB (credit
  "Contains Ofcom data, OGL v3"), after checking its columns.
- The German list fills only from user submissions; the "?" help says so.
