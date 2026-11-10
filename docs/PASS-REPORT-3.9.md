# Weather satellite pass report card (3.9)

Round-4 item 20 (scratchpad r4-per-mode list). Branch feat39-r4.

- **Meteor-M LRPT > Pass reports**: one row per pass with:
  - the start time and the satellite (from the orbit data)
  - the maximum elevation from the receiver position, worked out from the orbit
  - mean and peak SNR
  - the share of frames that decoded (RS good out of all frames heard)
  - lines, and gap lines (lines no channel received)
  - the comparison with earlier passes of the same satellite family within 10° of elevation: "better than usual", "as usual" or "worse than usual", with their mean lines and frame share. Better or worse means more than 15 % more or fewer lines, or 10 points more or fewer frames.
- A pass starts when frames decode and ends after a minute without new frames, or when the receiver stops. Frames counted before the pass (noise) and earlier passes on the same running counters do not count.
- The last pass is summarised on one line above the picture. Every pass is appended to `<data folder>/stats/meteor-passes.csv`.
- **Only Meteor-M LRPT.** HRPT, FY-3 and AWS keep their own counters in other receivers and were not wired in this round.

## Tests
`tests/meteor/test_meteor_passreport.cpp`:
- a pass built from the receiver's running counters: noise before it, the quiet minute, a second pass on the same counters, a restart that resets them
- the comparison: as usual, worse, better, nothing similar, elevation not known, another satellite
- the CSV round trip

Run locally: all passed. Simulated counters only, not a real pass. The UI and the elevation from the orbit data were syntax-checked only.
