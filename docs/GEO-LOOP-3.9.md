# Geostationary full-disk loops (3.9)

Round-4 item 24 (scratchpad r4-per-mode list). Branch feat39-r4.

- When a GOES, GK-2A, Elektro-L or FY-4 picture reaches the image gallery, OnAir rebuilds the loop of that product: the last 24 pictures of the same channel and sector, oldest first, as an animated GIF. It is written in the background to `<data folder>/images/loops/<source>-<product>.gif`, at most 1000 pixels on the longer side, 4 frames a second, looping for ever.
- A product is the picture's name with the digits taken out, so times and sequence numbers do not split it ("IMG_FD_023_IR105_20190907_000000" and "IMG_FD_024_IR105_20190907_001000" are one product, IR105 and SW038 are two). Only pictures of the same size go into one loop. A loop needs at least 3 pictures.
- On by default. The setting `galleryGeoLoop` turns it off. The frames are grey, which is what these channels are. Colour composites are not looped.
- The GIF encoder is OnAir's own (GIF89a, LZW with 9 to 12-bit codes, a clear when the table is full). MP4 is not written: that needs a video encoder, and OnAir ships no patented codecs.

## Tests
`tests/goes/test_goes_loop.cpp`:
- the encoder against its own reader, exact on noise (fills the 12-bit table many times), gradients and flat frames at 333 × 211, and on a 1 × 1 frame
- the header, the endless-loop block and the trailer
- the product key
- a loop from three PNG files, scaled, oldest first, and one picture refused

Also checked once by hand: the macOS image decoder (sips) read a noise frame from the encoder with 0 of 70 263 pixels different. The gallery hook was syntax-checked only.
