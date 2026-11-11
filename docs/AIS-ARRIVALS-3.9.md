# AIS arrivals board (3.9)

Round-4 item 15 (scratchpad r4-per-mode list). Branch feat39-r4.

- **AIS > Arrivals**: type a port name, its UN/LOCODE (optional) and other spellings crews use. The board lists every vessel whose declared destination (message 5 or 24) names that port, sorted by the declared ETA. Vessels without an ETA come last.
- Matching:
  - case, spaces and punctuation are ignored
  - the LOCODE with or without a space ("LT KLJ", "LTKLJ", "LTKLJ01")
  - a country prefix ("LT KLAIPEDA"), and suffixes such as ROADS or ANCH
  - one wrong or missing letter in names of 6 letters or more
  - in "FROM>TO" only the part after the last ">" counts
  - shorter names must match exactly or as the start
- The declared ETA has no year. It is read as the next date at or after one day ago, so a ship that is a few hours late keeps its ETA. Times are shown in local time with "in N h" or "N h late".
- **Port is here** takes the receiver position as the port position. With a position, the board also shows the distance and an ETA worked out from distance and speed over ground (not for speeds under 0.5 kn).
- The port is saved (`aisArrivalsPort`).

## Tests
`tests/ais/test_ais_arrivals.cpp`:
- 14 spellings that must match and 9 that must not, and the short-name rule
- the declared ETA: tomorrow, earlier today, 31 December, after New Year, not given
- the board order, the distance and worked-out ETA, a ship at anchor, vessels without a position
- the settings text

Run locally: all passed. Simulated vessels only. The UI was syntax-checked only.
