# Zigbee sub-GHz and Thread (3.7)

Both use the same receive-only 802.15.4 receiver (core/src/zigbee). Nothing is sent, joined or answered, no key is asked for or recovered.

## Zigbee, sub-GHz (new)
- 868.3 MHz (Europe, channel 0): BPSK, 300 kchip/s, 20 kbit/s. 906 - 924 MHz in 2 MHz steps (US, Australia, channels 1 - 10): BPSK, 600 kchip/s, 40 kbit/s.
- Chosen by the tuned frequency (below 1 GHz). The Zigbee channel list has the 868 / 915 channels below the 2.4 GHz ones; scan steps through the band you are on.
- PHY: differential encoding, 15-chip PN spreading, raised-cosine chips, preamble 32 zero bits, SFD 0xA7, PHR, PSDU with the FCS. Detection uses the chip-to-chip phase change, so carrier offsets of tens of kHz (+-40 ppm at 868 / 915 MHz) do not matter; the offset is then removed and bits are compared with the bit before.
- Needs: HackRF class radio at 4 Msps (the mode's rate); the demodulator itself works from 2 Msps (868) and 2.4 Msps (915), an RTL-SDR path is not enabled.
- Verified: synthetic loopback only (offsets +-30 / 36 kHz, chip clock +-40 ppm, 8 - 10 dB SNR, 2.4 / 4 / 5 Msps, wrong FCS, noise only). NOT verified: a real 868 / 915 MHz Zigbee transmission.
- Not covered: the 779 MHz (China) and 950 MHz (Japan) PHYs, 2003-style O-QPSK at 915 MHz.

## Thread (new, family 244)
- 2.4 GHz channels 11 - 26, O-QPSK 250 kbit/s (the Zigbee PHY), receive only.
- Shown in the clear: Thread beacons (network name, extended PAN ID, version), MAC headers, devices by 802.15.4 address, MAC commands, frames sent without link security (MLE Discovery Request / Response, the Joiner scan), their 6LoWPAN headers (mesh, fragments, IPHC with inline addresses, UDP ports) and the MLE command / CoAP code.
- Encrypted: almost all Thread traffic is AES-CCM at the link layer. Those frames show the 802.15.4 header, the frame counter and key index and are marked encrypted; the IPv6 / MLE / CoAP content is NOT readable without the network key. OnAir does not decrypt it and does not ask for or recover keys.
- Verified: byte-by-byte frame tests (tests/thread/test_thread_proto.cpp), synthetic O-QPSK loopback with +-100 kHz offset, chip clock error, 10 dB SNR, 4 / 5 Msps, Zigbee mode not calling Thread frames Thread. NOT verified: a real Thread network.

## Not done
- A user-supplied Zigbee network key (AES-CCM* decryption of NWK / APS) and a Thread master key: not implemented.
- Z-Wave is another worker's mode (feat37-modesB).
