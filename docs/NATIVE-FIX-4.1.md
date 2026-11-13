# Native driver test fix (4.1)

The ctest `native` (native radio drivers against fake vendor libraries, Linux/macOS only) failed 4 checks on integ-40, the 4.0 release
head 365700b82 and integ-41 (runs 43363137455, 43368500233, 4.0 full run 43264454366).

None of it is a 4.0 regression: `core/src/source/source_rtlsdr.cpp`, `source_airspyhf.cpp`, `native_common.h`,
`tests/source/test_native.cpp` and `tests/source/native_fakes.cpp` are byte-identical between v3.9.0 and v4.0.0. The test is only run
when CI's test selection picks it, so it went unnoticed since 0.2.6/0.2.7.

| Check | Cause | Kind | Users affected |
|---|---|---|---|
| HF+ "tone ... at 192 kHz" | A rate change first collects the last stream's cancelled USB transfers (25 x 20 ms, the #78/#160 Windows crash fix, 0.2.6). The test read the new stream for only 250 ms. | Outdated test | No (0.5 s gap on an HF+ rate change, by design) |
| HF+ "stream after the refused rate" | Since the Pluto full-duplex change (0.2.7) `NativeSource::retune` stops the source when `configure()` fails on a rate change, unless the driver says the radio is unchanged. The HF+ refusal of a too-wide rate (TV, DAB) happens before anything is sent, but the driver did not say so: the HF+ went silent. | Real bug | Yes, minor: 0.2.7 to 4.0, HF+ only, when a mode asks for more than 912 kHz with a wide channel (the error shows, then Start is needed) |
| "V4: direct sampling switched (0 calls)" | Direct sampling is never switched (0 calls is correct). The check also wanted no note, but the fake is a stock osmocom librtlsdr (no `rtlsdr_check_dongle_model`), for which the "HF needs the Blog library" note is right. | Outdated test | No |
| "HF+ with a bias-tee: not offered" | The HF+ listing is frozen once an HF+ was opened in the process (#78: `list_devices` opens every radio and races stop/close). The test raised the bias-tee count after earlier checks had used the radio, so the cached entry was stale. In the app the first listing, before any open, reads it. | Outdated test | No |

## Changes

- `source_airspyhf.cpp`: `resumeAfterConfigureFailure()` returns true when the rate was refused before the radio was touched; the stream
  resumes at its old rate. `requested_` is only updated once a rate is sent. Each open also refreshes the frozen listing's bias-tee flag.
- `test_native.cpp`: tone window 1.5 s; the V4 check expects the note exactly when the fake is not a Blog library; the bias-tee check
  opens the radio once before listing.

## 4.0.1?

Not needed for this alone: the only real bug is old (0.2.7), HF+ only, and the user sees the refusal message and can press Start. It can
ship with 4.1. RTL-SDR V4 direct sampling and the HF+ bias-tee work in 4.0 as in 3.9.

All of this is validated against the fake libraries only (no hardware).

## CI

- quick.yml run 43377142724 (Linux, this branch, compare v3.9.0): native, native_nobias, dxpatrol, native_lifecycle, usb_fd all pass. The
  same tests built from v3.9.0 on the same runner: native fails the same 4 checks, so 3.9 had them too.
- Run 43374746364 (before the refused-rate fix): 3 of the 4 fixed, "stream after the refused rate" still failed, which found the real bug.
- Run 43374750816 (Windows): native_lifecycle, usb_fd, sdrplay, pluto_duplex, rx888 pass. `native` itself is UNIX-only in CMake
  (dlopen of copied fakes), so it has no Windows run; the HF+ change is shared code and builds there.
- native takes 185 to 680 s on a shared runner, against the 900 s default timeout: worth a TIMEOUT or a slow label if it starts timing out.
