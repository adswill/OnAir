# Talkgroup names (DMR, TETRA, P25)

A name list for talkgroup numbers, shown beside the number in the voice and trunking panels: DMR calls (Calls and Messages tables, current call),
TETRA slot lines, the P25 current call, call log and Talkgroups table, and the trunk pool's radios and call log (by the protocol of the call).
Open **Talkgroup names** in the mode's Receiver tab (P25: the Talkgroups tab; trunk pool: Advanced). It is collapsed by default.

* **Add** a number and a name (and a group); **Edit** and **Delete** on a row; **Search** filters by number, name or group.
* **Import CSV** reads either the RadioReference talkgroup export (columns Decimal, Alpha Tag, Description, Tag, Category: the Alpha Tag becomes the
  name, Tag the group, Description the note) or this list's own file (`Kind,Id,Name,Group,Note`, Kind = DMR, TETRA or P25). A file without a header is read
  as number, name[, group[, note]]. Comma, semicolon or tab; rows without a usable number or name are skipped and counted. Names for the same number are
  replaced by the import.
* **Export CSV** writes the names of this system type in the own format (it re-imports anywhere).

Each system type (DMR, TETRA, P25) has its own numbering, so a name belongs to a type and a number. The whole list is kept in `<data folder>/talkgroups.csv`
(own format) and read at the start. OnAir ships no lists: get them from RadioReference (talkgroup pages, for subscribers), your network operator or
your own notes. Receive only: the names are labels for what is already decoded.

Code: `core/src/common/tg_dir.cpp` (parse, edit, filter), `app/voice/tgdir_ui.cpp` (editor and file), wired in `app/dmr`, `app/tetra`, `app/p25`,
`app/trunkpool`. Test: `tg_dir`. The talkgroup rules (allow / deny / priority / record) of the trunk follower are separate (`AccessList`).
