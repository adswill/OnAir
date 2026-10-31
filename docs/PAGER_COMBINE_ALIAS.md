# Pagers: capcode names and multi-copy combining

Two additions to the Pagers mode (POCSAG and FLEX). Both work without any set-up.

## Capcode names (alias CSV)

Capcodes view > **Import names...** loads a CSV; **Export names...** saves it; typing in the **Name** column of the Capcodes table names a capcode
directly, and right-clicking an address in Messages > "Name this address" does the same. Once a name exists, Messages gets a **Name** column
(category in the tooltip), the filter box also matches names and categories, and Copy / Save include the name. The table lives in
`pager_aliases.csv` in the data folder and is loaded at start.

File format (UTF-8; comma, semicolon or tab separated; header optional, its column names are recognised in any order):

```
capcode,name,category,protocol
1234567,Fire Station 3,Fire,POCSAG
1000-1099,Hospital pagers,Medical,
```

* `capcode`: one number or a range `a-b`. `protocol`: `POCSAG`, `FLEX`, or empty / `any` for both. Entries are per protocol: the same number on
  POCSAG and FLEX can have different names.
* Without a header the columns are capcode, name, category, protocol. Lines starting with `#` are comments. Quoted fields (`"Smith, John"`,
  `""` for a quote, newlines inside quotes), a UTF-8 BOM and CRLF line ends are handled.
* Which entry wins for a capcode: one for its own protocol before one for both, a single capcode before a range, a narrower range before a
  wider one. Importing replaces entries with the same range and protocol and keeps the rest. Bad lines are counted as skipped.
* Names that start with `= + - @` get an apostrophe in the export so a spreadsheet does not run them as formulas; the import takes it off.

Code: `app/pager/pager_alias.h` (pure, tested by `pager_alias`).

## Multi-copy combining

Paging networks send a page more than once. A copy that lost code words (more wrong bits than the BCH(31,21) code can repair) is completed from
the earlier copies **before** the word is given up (`core/src/pager/pager_combine.cpp`, test `pager_combine`, `pager_combine_rx`):

* A code word is chosen from all copies: a copy that passes the BCH check votes with its repaired data (most votes win, fewer repairs break a
  tie). If no copy passes: with three or more copies the bits are voted and the result repaired; with two, the bit positions in which they
  differ (at most 10) are tried and only a unique valid code word is accepted.
* A word that the BCH code "repaired" into a wrong word (burst errors) is outvoted too: a clean word beats a repaired one, and two copies that
  agree beat one. A copy cut short because a message word was repaired into an idle or address word is completed from the longer copy; a
  genuinely short message that only starts like a longer one is left short (its ending word is a clean idle word).
* POCSAG: copies are grouped by RIC and function code, within 15 minutes. Two copies belong together when the same address word is seen and at
  least a third of the message words are good in both, with (almost) none contradicting. The copy being decoded always keeps its own good
  words. A page made this way is marked `[combined]` in the list and its tooltip says how many copies were used.
* FLEX: a phase (88 code words) is compared word for word with earlier phases of the same speed and phase number. If the layout lines up (a
  repeat in the same frame of a later cycle) the damaged words are voted before the phase is decoded.
* Switch: Messages > **Combine repeats** (on by default). `PagerReceiver::setCombineCopies()`.

Limits: FLEX repeats whose pages sit at different positions in the frame are not aligned and not combined. A page lost completely in every copy
is not recovered. Two different messages to the same capcode that agree in most words could in principle be merged; the match rule rejects them
when any good word differs.

Measured on the synthetic test signal (POCSAG 1200, bursts of 6 ms noise at random times, `pager_combine_rx`): complete correct pages 33 of 47
without combining, 45 of 47 with it; wrong texts 14 down to 2. Real paging networks and real interference were not available.
