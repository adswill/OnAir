# ATSC 1.0 and ATSC 3.0 in 4.0 (feat40-atsc)

Users reported that ATSC 1.0 and ATSC 3.0 were the weakest TV modes: they lagged, stuttered or froze, and did not decode some real
US stations. The synthetic tests of 3.9 used the AWGN channel and a few static echoes, and at those the receivers were at theory
(docs/SENSITIVITY-4.0-C1.md: 90 % of packets at 15 dB). This round looked at what real channels and real computers do instead.

**What was measured on what.** Every number below comes from simulated signals: the generator's clean 8-VSB signal with channels,
noise and interference applied by the new test, run on GitHub's Linux runners. No real ATSC recording was used, see "Real recordings".

## Real recordings

- Issue #484 (ATSC 3.0, channels 19 and 21, 3.9.0) links two IQ recordings on Google Drive. They were made with OnAir
  (`onair_atsc3_503.000MHz_10Msps_20261109-134033.cs16` and `onair_atsc3_515.000MHz_10Msps_20261109-134450.cs16`, 10 Msps, 16-bit, 2.0 GB
  each, with SigMF metadata). They were **not downloaded**: the worker rules need the owner's OK to download files, and the laptop had
  15 GB free on battery. The ATSC 3.0 signalling fixes below were made from the A/331 text and the symptoms in the issue, and need
  checking against these files.
- No public ATSC 1.0 recording with a licence for test use was found. The A/74 field ensembles (Annex A) are "available by request to
  the ATSC".

## ATSC 3.0: service signalling (issue #484)

Symptom in #484: the service list (SLT) shows, a service is chosen, but it stays at "waiting for its signaling" and no ROUTE objects arrive.
In 3.9 the receiver read the service layer signalling (SLS, TSI 0 of the service's ROUTE session) only as a plain `multipart/related`
package. A/331 allows more, and stations use it:

| What A/331 allows | 3.9 | 4.0 |
|---|---|---|
| the whole SLS package gzip compressed (7.1.6.1) | not read | gunzipped first |
| Signed Package Mode: `multipart/signed` around the package, then the signature (A.3.3.5, A/360) | not read | the package inside is opened (nested up to 4 levels); the signature is not checked |
| a package without its MIME header (starts with the delimiter) | not read | split |
| LLS SignedMultiTable (LLS_table_id 0xFE, 6.7): the SLT, SystemTime... of a station that signs its signalling | ignored: no services | every table inside is read |
| ROUTE source packet: PSI high bit 1 (A.3.4), low bit not defined | low bit had to be 0 | only the high bit is checked |

New in the ATSC 3.0 tab: "service signaling: N objects on its signaling channel, M documents read", a note when the station signs its
signalling, and a warning when link-layer packets carry compressed IP headers (ALP type 1, ROHC; not decoded yet, services sent that
way stay silent). With the recordings of #484 these lines tell which of the cases applies.

Tests: `atsc3_lls` (signed table with two tables, truncation at every 37 bytes, a length past the end), `atsc3_route` (PSI 11, gzip
package, signed package plain and gzip, headerless package, a broken gzip object counted but not read).

MMTP services (slsProtocol 2) are still listed as "not supported yet".

## ATSC 1.0: lag, stutter and freezes

In 3.9 a radio (non-blocking receiver) lost fields like this:

- The field worker (equaliser fit, equaliser, trellis, Reed-Solomon) must finish a field every 24.2 ms. When **three** fields waited
  (73 ms), all queued fields were dropped and the decoder **and the equaliser** were reset. The next fields then needed the plain
  equaliser to train first; on a channel with strong echoes that fails, so a single stall cost a second or more of picture.
- The packet callback (TS demultiplexer, player, recorder, outputs, under the engine's TS lock) ran **on the field worker**. Anything
  that held that lock or the player's lock for a while stopped the equaliser.
- A lost and found lock also threw the equaliser away.
- Fields with damaged segments got a second equaliser try at full cost, also while the worker was already behind.

Changes (commit 5529cd7):

| | 3.9 | 4.0 |
|---|---|---|
| fields that may wait for the worker | 3 (73 ms), then all dropped + full reset | 10 (242 ms), then the oldest one only; the decoder restarts after it with the taps it had |
| packet callback | on the worker | its own thread (`onair-atscout`), about 10 s may wait, then the oldest are dropped and counted |
| lock lost and found | equaliser reset | decoder reset, equaliser taps kept (a new channel or rate still resets all) |
| worker behind (2+ fields waiting) | same work | reuses the last taps every second field, no second try, no echo searches |
| telemetry / receiver tab | - | decoder load (% of real time), fields dropped, packets dropped |

`ATSC_OLDDROP=1` and `ATSC_SYNCCB=1` restore 3.9's behaviour (used for the comparison runs).

CPU on the CI runner (`atsc_cpu`, AMD EPYC 9V45, CPU seconds per second of signal, all threads): ATSC 1.0 8 Msps 0.58, 10 Msps 0.47,
ATSC 3.0 10 Msps 0.39, both together 1.02 (real-time factor 2.1 for ATSC 1.0 at 10 Msps on one runner core). 3.9's figures on the same
processor type were 0.52 / 0.54 / 0.35 / 1.10 (docs/ATSC-X86-3.9.md): unchanged within run-to-run spread. The decoder load shown in the
receiver tab was 0.54 of real time in the paced run.

**Real-time run** (`atsc_channels_paced`: the receiver non-blocking, the feed paced to the signal, the player stalling 150 ms every 80
callbacks). The first channel chosen (R2.2 #2) does not decode at all, before or after (0 good packets in both runs, 0 fields dropped,
load 0.50 / 0.54), so it shows no difference. The test now uses a -10 us -3 dB echo plus an R.1 pair at 24 dB: 10.4 % good, 0 fields
dropped, 12 lock losses, decoder load 0.96 on an EPYC 7763 runner. There is no 3.9 run on this channel, so **the lag fix is not yet
measured against 3.9 in real time**; what is shown is that no field was dropped although the player stalled. The next step is to run
it with `ATSC_OLDDROP=1 ATSC_SYNCCB=1` on the same runner.

CPU on a second runner (EPYC 7763, run 43154327743, all changes): 0.98 / 1.07 / 0.58 / 1.72 (3.9 on that processor type: 0.79 / 0.91 /
0.62 / 1.72). ATSC 1.0 alone reads 15-20 % higher there, both together the same; runs on these shared runners differ that much from
run to run (docs/ATSC-X86-3.9.md), so this needs a same-runner comparison (`quick.yml -f compare=`) before it is called a cost.

## ATSC 1.0: real channels (A/74)

New test `atsc_channels` (tests/atsc/test_atsc_channels.cpp, four ctest entries labelled slow): the generator's clean signal at 10 Msps
(20 Msps for the adjacent channel), the channel applied by the test (fractional delays, a phase that turns at the Doppler rate), white
noise in the 5.38 MHz noise bandwidth with the power of all paths as the signal (A/74 5.5.3.3). Threshold = the lowest SNR in 1 dB
steps with at least 90 % of the packets good and no more than 3 wrong (bisection between 12 and 32 dB; "-" = not even at 32 dB). 1.6 to
2 s of signal per point, one seed. Wrong packets: at most 3 in every case, before and after.

Changes:

- **Pre-echo canceller span 100 -> 330 symbols (9.3 -> 30.7 us)**, also tried when the fit is merely poor (< 16 dB) and the field sync
  shows an earlier path of -14 dB or more beyond the feed-forward taps (every eighth field at most). Field jobs start 1600 symbols
  before the field (was 107), so the earlier field sync is in view. (91fb1f4)
- **New post-echo canceller for 118-480 symbols (11-44.6 us)**, the mirror of the pre-echo one (fitted on the field sync, forward
  recursion), used when a later path of -14 dB or more is found and it raises the fit by 0.5 dB. (91fb1f4)
- **A badly decoded field (10 %+ segments lost) is retried with taps solved from its own decoded symbols**, then the plain equaliser
  from 20 % (3.9: the plain one from 20 %). (3d07713)
- **In-field tracking** (`equalizeTrack`): the same equaliser one segment at a time, with a normalised LMS step of all taps after each
  segment. Tried first among the retries; kept on while it wins, off after 200 clean fields. (5894e74)

Before = 3.9's behaviour (run 43150881059, ATSC_OLDDROP=1 ATSC_SYNCCB=1, pre-echo span 100). After 1 = the cancellers and the retry
(run 43152211329). After 2 = plus tracking (run 43154327743).

| Case (A/74) | before | after 1 | after 2 |
|---|---|---|---|
| AWGN | 15 dB | 15 | 15 |
| 5.6 echo -30 us -7 dB | - | **17** | 17 |
| 5.6 echo -15 us -5 dB | - | **18** | 18 |
| 5.6 echo -10 us -3 dB | - | **21** | 21 |
| 5.6 echo -5 us -0.5 dB | - | - | - |
| 5.6 echo +5 us -0.5 dB | 32 | 31 | 31 |
| 5.6 echo +10 us -1 dB | 31 | 31 | **27** |
| 5.6 echo +15 us -1 dB | - (0 % good) | - (29 %) | - (29 %) |
| 5.6 echo +20 us -2 dB | - | **23** | 23 |
| 5.6 echo +30 us -3 dB | - | **21** | 21 |
| 5.6 echo +40 us -4 dB | - | **20** | 20 |
| R.1 0.5 us, 7 / 3 / 0 dB, 2 Hz | 16 / 18 / 31 | 16 / 18 / 31 | 16 / 18 / **25** |
| R.1 2.0 us, 7 / 3 / 0 dB, 2 Hz | 16 / 19 / - (67 %) | same | 16 / 19 / - (78 %) |
| R2.1 #1 - #4 (path 5 at -10 dB, 5 Hz) | - (73 / 0 / 0 / 0 %) | same | same |
| R2.2 #1 - #3 (path 5 at 5 Hz) | - (0 %) | - | - |
| airplane flutter, -12 dB at 3 us, 60 Hz | - (0.4 %) | - (1.3 %) | - (0.5 %) |
| burst noise 165 us, -5 dB, 10 Hz | 15 | 15 | 15 |
| co-channel DTV, D/U 15.5 dB | - (0 %) | - | - |
| adjacent DTV +6 MHz, D/U -20 dB (8-bit) / -33 dB, 20 Msps | 15 / 15 | 15 / 15 | 15 / 15 |
| carrier 10 dB below the signal at +1.2 MHz | 17 | 17 | 17 |
| 6.5, 7.5, 8, 9.14, 10, 12.5, 20 Msps, 8-bit, +38 kHz, +30 ppm | 15 each | 15 each | 15 each |
| R2.2 #2 + 38 kHz + 30 ppm, 8-bit | - | - | - |

What this says:

- Single long echoes inside A/74's range (-30 to +40 us) went from no picture at all to 17-23 dB. The guideline asks for these
  levels at "TOV" with a strong signal; 3.9 decoded none of the six beyond +-10 us.
- The sample rates of real radios (6.5 to 20 Msps, non-integer ratios, 8 bits, tuning and clock errors), burst noise, an in-band
  carrier and the adjacent channel were already fine in 3.9 and stay so.
- **Not solved:** the R2.1 / R2.2 ensembles (five echoes, one turning at 5 Hz), a near-0 dB pre-echo (-5 us, -0.5 dB: the pre-echo
  canceller needs the earlier path clearly weaker), +15 us at -1 dB, airplane flutter at 60 Hz, R.1 at 0 dB with 2 us, and co-channel
  DTV at D/U 15.5 dB (0 % good: the other station's sync and pilot disturb the loops; noise of the same power decodes). These are the
  next items; likely parts: the post-echo search threshold (-14 dB misses R2's -15 to -18 dB path 6), a tracking step tuned per channel,
  and a co-channel case in the sync loops.

Guards in `atsc_channels`: every case that decodes keeps its "after" threshold within 2 dB (kGuards in the test).

## ATSC 1.0: other

- PSIP: a TVCT entry with program number 0 or 0xFFFF (an analog or inactive channel some stations still list) no longer becomes an
  empty service in the list. Test: `ts`.

## Not done

- The #484 recordings (owner's OK needed to download 4 GB). With them: check which SLS form the stations use, and the "no PLP" state of
  channel 21.
- ATSC 3.0: no changes to bootstrap, channel estimation or LDPC (feat40-sensX3 owns OFDM channel estimation); MMTP services and ROHC
  compressed IP not decoded; no lag measurement for ATSC 3.0 beyond the existing load and dropped-block counters.
- Player (item 4 of the assignment): not changed. AC-3 / E-AC-3 already play through FFmpeg; AC-4 is user-installed with its help
  card (docs/CODECS.md). Frame pacing and channel-change time were not measured.
- The real-time lag comparison against 3.9 on the same channel (see above).
