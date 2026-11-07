# FANS-1/A CPDLC decode

CPDLC (controller-pilot data link) messages ride inside ACARS text on VDL2, HFDL and the Aero (Inmarsat / Iridium) channels.
OnAir recognises the ARINC 622 applications `AT1` (CPDLC message), `CR1` (connect request), `CC1` (connect confirm) and
`DR1` (disconnect request), checks the CRC-16, and decodes the ASN.1 PER body of the FANS-1/A ATC uplink and downlink
message sets to the plain message text:

```
CPDLC Downlink Message:
 Header:  Msg ID: 8   Timestamp: 15:56:32
 Message data: POSITION REPORT ... Flight level: 340 ... Next fix: GORLO ...
```

Where it shows: the detail pane of every VDL2 ACARS frame (summary line carries the first words of the decoded text), and the
decoded-message detail of ACARS and Aero. It is not a mode of its own: it is a layer on those receivers, so it has no panel,
no mode number and no Auto setup. Nothing to configure.

Source of the decoder: libacars (Tomasz Lemiech, MIT) asn1c tables and text formatter in `third_party/libacars`, see
THIRD_PARTY.md. The direction decides the message set (ground to air = uplink); a message of the wrong set is decoded as that
set and may read as nonsense, check the CRC line first.

Not covered: ATN B1 CPDLC (ISO ASN.1 over the ATN, Europe above FL285 on VDL2 as X.25/CLNP, a different message set), ADS-C
contract requests (uplink ADS-C is still the in-house group decoder), JSON output.

Tests: `test_aero_cpdlc` (four real downlinks and one real connect request from the libacars examples, CRC and decode; encode to
decode loopback with the asn1c encoder, damaged digit, lower case, truncated texts, free text).
