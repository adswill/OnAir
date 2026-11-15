# TETRA names (ISSI and GSSI)

Your own names for TETRA terminal numbers (ISSI) and talkgroup numbers (GSSI). They are shown beside the numbers on the TETRA screen: the slot lines
("Listen"), the Messages table (From and To), the Talkgroups table, the Map (labels, card, list) and the **Speaking now** line under the voice strip,
which shows "ISSI 1234 Name -> GSSI 91 Name (slot 2)" for calls heard in the last seconds. Calls that are encrypted carry no ids and show none; a name
changes only what is displayed, never what is decoded. Receive only.

Open the **Names** tab of the TETRA screen: pick ISSI or GSSI, type number and name (optional notes, MCC and MNC, colour code, site), **Add**; **Edit**
and **Delete** on a row; **Search**; **Import ...** and **Export ...**. The list is kept in `<data folder>/tetra_names.csv` and read at the start. OnAir
ships no names and the file never leaves your computer.

## Network

An entry with MCC and MNC applies only on that network; an entry with neither applies on every network (the usual case). When both exist for a number the
one of the network heard wins. The colour code and site are labels for you. The talkgroup list of the shared **Talkgroup names** directory is used as a
fallback for GSSI names.

## CSV (always works)

```
type,id,name,notes,mcc,mnc,colour_code,site
ISSI,1001,Example unit,spare radio,,,,
GSSI,2001,Example group,,901,1,3,Test site
```

`type` is ISSI or GSSI (a missing type column uses the type chosen in the tab), `id` is 1 to 16777215. Comma, semicolon or tab; quotes, BOM and CRLF are
fine. Columns may come in any order; `label`, `alias` or `description` are read as name / notes. Bad rows are skipped and counted.

## Plain lines (tolerant)

Without a header, one number per line: `1001;Name`, `1001,Name,notes`, `1001<tab>Name`, `1001 Name`, `1001=Name`, `ISSI=...` as `ISSI:1001;Name`,
`GSSI 2001 Name`. Lines starting with `#` are comments.

## The "TETRA Trunk Tracker" (SDR# plugin) files

The layout of the plugin's label files could not be established from public documentation. What public forum posts say (second hand, not checked
against a real file): the plugin keeps a GSSI label file `tetra_GSSI.txt`, an MCC label file `TETRA_mcc.txt`, labels are limited to 30 characters and must
not contain `;` or `|`, and the files have fields such as GSSI, MCC, MNC, label, priority and lockout, or SSI, GSSI, MCC, MNC, label, date. Nothing of this
is parsed specially: a line that is plain `number<separator>name` imports with the tolerant reader above, everything else is skipped and counted. If you
send a sample file (made-up or your own, with the private parts removed) a proper reader can be added.
