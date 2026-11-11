# "Works, but" issues, batch B (3.9)

Branch feat39-worksbutB. Issues checked: #358 #357 #350 #275 #225 #213 #84 #103 #407 #404 #420 #419 #469 #122 #463 #458 #408.

## Changed

### COSPAS-SARSAT, checked against C/S T.001 Issue 4 Rev. 7 (March 2021)

ELT(DT), protocol 9 (A3.3.8):

- All identity types: 00 aircraft address, 01 operator designator and serial, 10 TAC and serial, 11 reserved (shown as raw bits).
- The location test (bits 43-66 all 0 or all 1) and the cancellation message (A3.3.8.5).
- The PDF-2 rotating field with the aircraft operator 3LD.
- Offset seconds 1111 are the default and no longer shift the position.

A3.3.8.1 does not state how bits 43-66 split for types 01 and 10. The split of the serial location protocols (A3.3.5.2 c) is used, and the raw 24 bits are shown next to it.

Found while checking, affecting other protocols:

- Standard location PDF-2 (A3.3.5.4): minutes are bits 114-118 and seconds 119-122. The decoder and the encoder both had 4 + 5 bits, so round-trip tests passed while real offsets were lost or wrong.
- Homing flag (A3.3.2.2): bit 112 for standard and national location, bit 108 for RLS, bits 84-85 for user protocols. It was read from bit 111 for all of them.
- The aircraft operator designator is the shortened 5-bit Baudot, not A = 1.

The tests build the bits by hand from the spec instead of using encode(). The French traffic of #407 is checked against the hex ID and position that dec406 printed.

Not validated on a real burst: no raw bits or IQ of an ELT(DT) were available. Newer revisions of T.001 (Rev. 13 exists) could not be read.

In the #407 examples, bits 41-42 are 11 (reserved) and bits 43-66 differ between bursts, in dec406's own hex IDs as well. The hex ID follows those bits as the spec defines it, so that traffic still shows several IDs.

### Hamlib sound card (#358)

The add-rig form asked for a device id the UI never shows. Names now resolve to the id (resolveAudioInputId), so rigs already saved with a name start, and the form picks the card from a list. Not run with a real IC-7300.

### Android RDS after a station change (#463)

nativeRetune used Engine::retune, which keeps the receiver state. A frequency move now uses retuneReset for FM and later modes, as the desktop does. Not run on a device, and the JNI file was not compiled locally (no jni.h here).

## Not changed

Status of every issue and draft replies are in scratchpad/smart37-backup/WORKSBUT-B.md.

#408: the reporter attached a recording (I-Q.wav, about 14 MB, a renamed .rar). It was not downloaded or analysed in this batch, so the Tetrapol traffic question is still open.
