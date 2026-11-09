# Pager smart package (3.7): proposals 12, 13, 14, 15

All four are automatic and on by default; the manual part is a checkbox. Verified first that none existed (no geocode, template, language-model, digipeater-graph or failed-frame vote code before; the existing multi-slicer and bit-flip repair stay as they are).

## 12 Message intelligence (core/src/pager/pager_intel.cpp)
- Positions in the text: decimal degrees (with N/S/E/W), degrees and minutes, UTM, MGRS. UTM and MGRS tested against an independent forward UTM over 400 random points of the globe (both hemispheres): 400/400 within 5 m (worst 1.4 m); MGRS 1 m grid 400/400; the Eiffel tower MGRS point is right.
- Place list: the user's `pager_places.csv` in the data folder (`name,lat,lon` per line), read each time the receiver starts. No place data is shipped.
- Capcodes' usual area: a page with no position gets the mean of that capcode's earlier positions, marked "approximate".
- Learned fields per capcode: job numbers, "Key: value" pairs, address-like text, and segment roles once a capcode shows a delimiter (| ; , /): Type, Number, Address, Position.
- Incidents: pages of different capcodes with the same job number, a position within 300 m, or nearly the same words (Jaccard 0.6) within 15 minutes share an incident number; shown in the Statistics view.
- Privacy: everything stays in memory. Medical-looking pages are not analysed, learned from or grouped unless "Include medical pages" is on.
- Not done: street-address geocoding (needs map data we do not ship); the user can list streets in the place list.

## 13 Text repair (same file, PocsagParser keeps which bits a lost code word took)
A lost 20-bit code word leaves characters with some bits known. The lost characters are searched over word segmentations from the capcode's own vocabulary (and the channel's), scored with a unigram model; numbers compete as words, so a lost number is never replaced by a word. A repair is accepted only when the best reading beats the second by 3x. Guessed characters are marked (colour and a bracketed list in the tooltip), unrecoverable ones stay `?`, a lost last word is cut and shown as one `?`.
Measured (test_pager_intel, 40 clean pages of one capcode, one lost word per page, 300 pages): 940 of 1118 unknown characters filled in right, 0 wrong, 155 left as `?`. With words the capcode never sent: 749 lost characters, 0 wrong guesses. FLEX is not repaired (the Rust decoder does not report where words were lost).

## 14 RF path and coverage (core/src/packet/packet_path.cpp)
From the H bits of every frame: stations to digipeaters to digipeaters edges, aliases (WIDE1, TRACE, TCPIP...) are not stations, IGates (TCPIP behind them), loops (a call twice in a path, or the same packet again and again through one digipeater), silent digipeaters (no frame for much longer than their usual beat), and per digipeater the relayed stations, their convex hull and range. Shown in a Digipeaters pane of the APRS / Packet mode. Unit test test_packet_path.

## 15 Demodulator vote (ax25::voteRepair, packet_rx.cpp)
The receiver already ran 3 correlator pairs x 6 slicers; what was missing is combining their failed copies. Copies of the same length are voted bit by bit and the up to 8 disputed bits are tried fewest flips first until the FCS and the plausibility check fit. On by default, checkbox "Vote" in the channel options. Frames made this way are marked `[repaired by vote]`.
Measured (test_packet_tnc --vote, 80 frames per run, 1200 baud, CNR in 25 kHz): at 4 dB 38 -> 50 and 37 -> 49 frames, at 5 dB 74 -> 75 and 71 -> 75, total over 1 to 5 dB 222 -> 254 (+14 %), 0 wrong frames. test_packet_votebits: bit level, no wrong frame in any case, independent errors outvoted 400/400.
Existing tests unchanged: test_packet_tnc (all checks), test_pager_* run.

## Files
core/include/dect2/pager/pager_intel.h, core/src/pager/pager_intel.cpp, pager_proto.cpp (lost-bit mask), pager_rx.cpp (smart layer, setSmartRepair/setSmartInsight/setAnalyseMedical/setPlaces), packet_path.*, packet_ax25.* (voteRepair), packet_rx.cpp (setVote), app/pager/pager_ui.cpp, app/packet/packet_ui.cpp.
