# Multitone paging (detector, with measured framing)

Multitone Electronics (UK) pagers, Mk IV to VII (hospitals, industrial sites; 25 to 470 MHz, narrowband FM). The format is proprietary and not
published: sigidwiki.com/wiki/Multitone_Paging says FFSK "similar to POCSAG 1200" but with its own headers, readable only by Multitone pagers, and no
specification, tone table or sample of the digital format is public. So this mode is a detector, not a decoder, and every report is labelled unverified.
No message text is produced, nothing is decrypted or transmitted. (If you have the format description, send it: the receiver has the hooks.)

## What is reported per transmission

The carrier (up to carrier gone), the FM deviation, the spectral lines and steady tones of the audio (a tone list in order of arrival with durations),
and, for two-level FSK or audio FFSK: the measured baud rate, the bit stream in hex, how many 32-bit words pass the POCSAG BCH(31,21) check, whether
the POCSAG sync word is in it, the 0x605A word count, and (new in 3.6) the **framing measured from the bit stream**.

## Framing measured on the owner's recording (3.6)

Recording: `Multitone_2015-12-31T01-37-17Z_27599.8kHz.mp3` (FM audio of 27.5998 MHz, 8 kHz, 28 s). A cycle of 12 s repeats: an FFSK burst of 2.9 s,
the squelch noise of the carrier going away (1.4 s), then a tone sequence of about 3.4 s (a steady 470 Hz tone for 0.5 s, then a 234 Hz tone with its
harmonics for about 1 s, then 234 / 250 Hz steps), noise, and the next cycle.

The FFSK burst: the audio tones are 293 Hz (f) and 586 Hz (2f). The analysis slices it at 586 symbols per second; **every bit is sent twice** (the 586 baud
stream is made of pairs of equal symbols), so the data rate is 293 baud, 850 bits in 2.9 s. In that stream a **12-bit marker** `001101010101` comes back
every 24 bits (8 times in a row, then a second marker `110011000100`), each time followed by a 12-bit word. The words come in **pairs** (each word is
repeated once) and only about ten different words occur in the burst (for example `011111111111`, `001010101111`, `000000000111`, `010101010111`). That is
all that is measured: how the words map to pager addresses, tones or text is not known and is not guessed. The numbers above are tested on a 8.2 s
excerpt of the recording (`tests/data/multitone/`, `test_multitone_real`).

Plan B for a real decoder: the format description (word table and the address field) or a recording with a known message sent to a known pager.

## Tests

`tests/multitone`: the detector on the generator's signals (`test_multitone_rx`, `test_multitone_engine`) and the owner's recording (`test_multitone_real`).
