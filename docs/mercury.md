# Mercury monitor (experimental)

A receive-only monitor for **Mercury V2**, the HERMES HF data modem of Rhizomatica (https://github.com/Rhizomatica/mercury, GPL-3.0).
Mode list: **HF DATA > Mercury (HF OFDM modem)**. Use it on an HF dial in upper sideband (the modem sits around 1500 Hz of audio).

**Experimental.** It was written from Mercury's source and documents and checked against OnAir's own modulator, a simulated HF channel
(carrier offsets, sample-clock error, noise, Watterson fading) and the LDPC test vectors of the upstream code. No real over-the-air
recording was available, so how it behaves on real signals is not verified. A short IQ or audio recording of a Mercury link would help.

## What it shows

- The OFDM data modes DATAC0, 1, 3, 4, 13, 14, 15, 16, 17 and QAM16C2 at once, found by their preamble or postamble, LDPC decoded.
- The frames: CQ, CALL and ACCEPT with call signs, the carousel's polls and data rounds of a connected session, DISCONNECT, broadcast
  messages and RaptorQ broadcast files (reassembled when enough symbols were heard, viewable and saveable).
- Sessions in both directions with the data reassembled.
- **Encrypted sessions are listed, never decoded.**

## Not decoded

The MFSK floor of the ARQ ladder is not decoded as a session. Nothing is transmitted by this mode. The TX studio entry only writes a test
signal (a demo link, a broadcast message and a file) to a file.

## Credits

Mercury: Rhizomatica, Rafael Diniz and contributors (GPL-3.0). The OFDM data modes follow the codec2 fork inside Mercury (David Rowe and
contributors, LGPL-2.1). RaptorQ is RFC 6330. See THIRD_PARTY.md.
