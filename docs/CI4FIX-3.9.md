# CI run 42921489043: the eight non-slow failures (feat39-ci4fix)

Branch `feat39-ci4fix`, based on `pub/integ-39` at b10e8bd2e. Each item has its own commit.

| Test | Platform | Cause | Fix |
|---|---|---|---|
| custom_bandwidth_all | all | The seven CIS mode rows (families 620-626) each have their own family, but their mode-table `subId` held ids (`cisbee` ... `cis3000`) that name no SubMode entry. | `subId` is `nullptr`, as for other single-protocol families. web_schema_gen.cpp was regenerated with tools/gen_web_schema.py. |
| sample_guard | Linux (GCC) | `dropNonFinite` zeroed samples through a `uint32_t*` that aliased `complex<float>`. That is undefined behaviour, and GCC -O2 kept the stale sample value after the call. | The exponent bits are read with `memcpy`, and the zeros are written as `float`. |
| voice_insight | macOS | Not a time-zone or week-boundary bug. libc++'s random draws put two calls (91 s and 31 s) into Tuesday 17:00 local, an hour that usually carries about 1 s. By the detector's own rules (at least 120 s, and more than 3x usual) that alert is correct. | The test is wrong. False alerts are now only alerts in busy hours, or in quiet hours whose generated airtime is below the busy threshold. The order of the random draws is fixed too, because the argument evaluation order differed between compilers. |
| autoparam_kbd "live relock" | Linux | After a lock, the 45 s Auto window still held the old signal, so the re-analysis found the old settings again. Following a change took two analysis runs, and on a loaded runner the second one did not finish in time. | Once locked, Auto analyses only the audio since the text stopped (at least 8 s). |
| cubesat_more, CCSDS concatenated | macOS | The generator sent the ASM straight after uncoded preamble symbols. The receiver's Viterbi then had no settled path when the ASM arrived. With every code except CCSDS (whose all-zero output is exactly the 0101 preamble), about 5 % of ASMs came out with more than 4 bit errors and the frame was missed. | The generator sends 8 coded bytes before the ASM, as a transmitter's continuous stream does. Measured at sigma 0.6, 1600 frames per case: NASA-DSN went from 87 lost to 0, the uninverted codes from 6-7 to 0, and CCSDS stayed at 0. |
| vdl3_all | macOS | A training sequence that began in the last symbols of a good burst still decoded, because only the *end* of the sequence was compared with the burst's end. The result was a false M downlink with 24 % EVM. | The *start* of the sequence is compared with the burst's last symbol. Bursts on one channel never overlap. |
| marine_fax_thread | Windows | `push()` decodes 170 s of audio in a fraction of a second. The busy reader thread was starved and saw 18 of more than a thousand updates. That is a test-timing problem, not a decoder race: TSan on macOS reports nothing. | The producer yields (50 ms at most) whenever the reader has seen nothing new for 8 chunks. Push, reads and setters still overlap. |
| webapp | macOS | The test's supervisor thread (`host.supervise` + `app.tick`) was detached and still running when `main` returned. `app.tick` reached `Sessions::prune` on the destroyed WebApp. Only libc++ checks for a destroyed mutex, so only macOS aborted. The backtrace came from a diagnostic quick.yml run. | The test is wrong. The loop now stops on a flag and is joined before the server stops. The server code is unchanged. |

## Verification

- quick.yml on Linux (run 42941933736): custom_bandwidth_all, sample_guard, autoparam_kbd and marine_fax_thread pass. web_schema still fails on 3G-ALE, DVB-SH, Link 11 CLEW and the two STANAG rows. Those are fixed in the integrator's held batch, not on this branch.
- quick.yml on macOS (run 42945977316): webapp, voice_insight, cubesat_more, vdl3_all, marine_fax_thread and custom_bandwidth_all all pass.
- Local runs on macOS: voice_insight, cubesat_more (1600-frame sweeps), vdl3_all (also with ONAIR_TEST_LONG=1), and marine_fax_thread (also under TSan). All pass.

## Limits

- Everything above was tested on simulated signals only.
- cubesat_more: the receiver is unchanged. A real burst whose ASM directly follows an uncoded preamble still loses about 5 % of frames at sigma 0.6 with the NASA-DSN and uninverted codes.
- marine_fax_thread could not be run on Windows from here (quick.yml has Linux and macOS only). It was checked on macOS, including under TSan.
