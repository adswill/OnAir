# DVB-T2 baseband export (BB frames and T2-MI)

OnAir can write the baseband frames of the PLP it decodes to a file and/or send them by UDP, so that outside tools (T2-MI extractors,
re-modulators, analysers) can use what the receiver recovered after LDPC and BCH.

Command line (`onair-cli`, DVB-T2):

    --t2-mi FILE      the PLP as T2-MI (ETSI TS 102 773, baseband-frame packets) inside 188-byte TS packets, to FILE
    --t2-bbf FILE     raw BB frames, each BBHEADER (10 bytes, descrambled) + data field + padding, back to back (kBch/8 bytes per frame)
    --t2-udp IP:PORT  the same stream by UDP, 7 TS packets (1316 bytes) per datagram; T2-MI unless --t2-bbf is given
    --t2-pid N        PID of the T2-MI TS packets (default 0x1000 = 4096)

Details and limits:

- Only the selected PLP (the one OnAir decodes) is exported. A block that failed BCH is left out; in T2-MI the packet counter still advances, so the gap is visible.
- Each T2-MI packet starts a new TS packet (pointer_field 0) and the last TS packet is padded with 0xFF; a standard T2-MI demultiplexer ignores the padding.
- `superframe_idx` counts FRAME_IDX wraps since the export started (it is not the transmitter's value); `intl_frame_start` is set on the first block of each T2 frame.
- Not exported: the T2-MI L1-current and timestamp packets (OnAir does not rebuild them). A re-modulator needs them from another source; BB-frame extraction does not.
- Multi-channel TV (several channels from one wide capture) is not exported; only the single-channel receiver is.
- Verified with the generator and receiver in loopback (noise, carrier offset, normal and short FEC frames, several modulations): `ctest -R t2mi_out`.
  Not verified against a real transmitter's stream or a real T2-MI tool.
