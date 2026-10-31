# ACARS and VDL2: long messages in one piece

A long ACARS message (weather, flight plan, ADS-C with extras) is sent as several blocks. OnAir now shows it as one message in the ACARS and VDL2 lists. Nothing to set up.

- The blocks of one message are held until the last one arrives, then joined (ACARS list, detail pane: "joined from several blocks").
- If the last block never comes (45 seconds of signal time) or a block in the middle was missed, the part that was received is shown and marked "last block missing".
- A block sent twice is counted once. Two aircraft, or two labels from one aircraft, never mix.
- VDL2: ADS-C and position reports in a joined message are read from the whole text. The session frame log (Export) keeps every raw block, each with its UTC time: rows with `raw_block` = 1 are the single blocks, rows with `blocks` greater than 1 are the joined message.
- Not changed: VDL2 channel list (Europe 2027 button), aircraft type and distance columns, frame log export (issues #400, #394, #309, already in 3.4).
