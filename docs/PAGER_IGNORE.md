# Pager Ignore list (POCSAG and FLEX)

Hide the addresses you do not care about. Works for POCSAG (RIC) and FLEX (capcode), one list per protocol, saved in the settings.

## Use

- **Right-click an address** in the Messages table: *Ignore this address* (or *Stop ignoring this address*).
- **Capcodes view**: an *Ignore* checkbox on every row, and the *Ignore list* section (open it with the header) to type, import or export the list.
- Entries are separated by spaces, commas or line breaks. `#` starts a comment.
  - a number: `836001`
  - a range: `836000-836999` (overlapping ranges are merged)
  - a wildcard: `8360*` (any digits, also none), `83?00` (one digit); matched against the decimal address
- **Import...** adds the entries of a text file to the list; **Export...** saves the list (same format). **Clear** empties it.

## What is hidden, and what is not lost

Ignored pages are only hidden. The receiver keeps them, so *Show ignored* (Messages toolbar, or the button under the table) brings every one back, in grey. The counter under the table says `ignored: N messages hidden`; the Capcodes view says how many addresses are hidden.

The list applies to: the Messages table, the side list, Copy, Save and Auto-save to file, the Capcodes view, and (in the receiver) the automations, so ignored pages send no notification, start no recording and call no webhook, and they are not stored in the message database. They still count in the statistics.

Whole span works the same: the list is checked per page, whatever carrier it came from.

## Limits

There is no size limit on the list (lookup is a binary search; 200000 ranges are checked in a test). A very large list is stored in the settings file as text.
