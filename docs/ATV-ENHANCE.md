# Analog TV video enhancer

Decoded analog TV (any standard the receiver locks to, live or from a recording) can be shown as a clean, square-pixel 720p or 1080p
video. The algorithm is in `core/src/atv/atv_enhance.cpp` (`dect2::AtvEnhancer`, no UI, CPU only, no model, no new dependency); the
analog TV tab has one "Enhance video" switch (Off / Light / Full), a size choice (As received / 720p / 1080p) and the sliders under
"Advanced". It runs on a worker thread on the newest picture: the decoder and the UI thread never wait for it (a slow machine
shows fewer pictures per second, never a stall). The still-picture Enhance control (docs/IMAGE-ENHANCE.md) is unchanged.

Per picture, on 8 bit Y/Cb/Cr planes:

| Step | Light | Full |
|---|---|---|
| Motion against the previous cleaned picture (3x3 mean of the difference, noise level estimated from the still parts) | yes | yes |
| Temporal noise reduction, weight falls to 0 between T/2 and T (a moving object leaves no ghost) | up to 0.5 | up to 0.72 |
| Comb repair (a line that sticks out of both neighbours where something moved is interpolated: motion-adaptive deinterlace of a woven picture) | - | yes |
| Spatial noise reduction (3x3 bilateral on luma) | - | yes |
| Colour edge fix (joint bilateral on Cb/Cr guided by luma: colour stops at the brightness edge) | - | yes |
| Scaling to square pixels in the picture's shape (4:3, 16:9 from the WSS) | bilinear | Lanczos-3 luma, bilinear colour |
| Sharpening (unsharp, coring of the noise, clamped to the local range: no ringing) | - | yes |

Dot crawl and line jitter are fixed in the decoder (comb filter, line time base), not here: this works on the finished picture.
The raw picture is never changed; the Enhance control for stills still works on top of it.

Test: `tests/atv/test_atv_enhance.cpp` (PAL and NTSC test pattern with noise: noise power drops, edge positions and aspect after scaling,
no trail behind a moving square, comb repair, same bytes for the same input). It also prints the time per picture.

## Cost per picture (measured)

PAL 768x576 input, one thread, GitHub Linux x86_64 runner (quick.yml, best of 6, noisy input; a laptop core is similar or faster):

| Level | to 1280x720 | to 1920x1080 |
|---|---|---|
| Light | 16.6 ms | 30.1 ms |
| Full | 48.1 ms | 76.3 ms |

Light keeps 25 pictures per second at both sizes on one core. Full at 720p keeps about 20 per second on that runner and 1080p about 13: the worker then shows the newest picture it has finished and skips the ones in between (the decoder is never held up), so "Full, 1080p" is a quality choice for a fast machine or a recording, and Light (or Full at 720p) is the live choice. The UI shows the time of the last picture next to the switch.
