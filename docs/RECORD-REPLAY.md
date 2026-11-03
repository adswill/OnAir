# Record and replay

What exists, what was missing, and how a recording goes from any mode to another computer.

## Audit (what was already there)

| Piece | Where | Notes |
|---|---|---|
| IQ recorder | `core/src/source/iq_record.cpp`, `IqRecorder`, `Engine::startRecording` | Writer thread, bounded queue, 4 GB cap. Works in every mode (it taps the radio's samples before the decoder). |
| SigMF write | `IqRecorder::setSigmf`, `sigmfMetaJson` | `.sigmf-meta` + `.sigmf-data` hard link; rate, centre, datatype, time and the **mode id** (in `core:description`). |
| SigMF / WAV / BLUE read | `probeIqFile` (`iq_file.cpp`), `bluefile.cpp` | Rate, centre, format, sample start, description. |
| File source | `makeFileSource`, device kind `File` | Plays with no SDR; loop, offset, end, big-endian. |
| Rewind buffer | `TimeMachine` (`time_machine.h`) | RAM ring, saved on trigger as CS8 + SigMF. |
| Recording library | `app/panels/recording_library_ui.cpp` | Lists SigMF / BLUE / raw files; Open and Re-analyse in a chosen mode. |
| Audio / decoded-output recording | `AudioRecorder` (diag row "Record audio"), DAB audio, TV recorder, message logs | The decoded sound and data of every mode. |
| Scheduler, automation, passes, remote API | `scheduler_ui`, `automation_sink`, `passes_ui`, `remote_api_app` | Start IQ recordings by rule. |

## Gaps found, and what changed

1. **No 16-bit recording.** Only 8-bit and float. Added `FileFormat::CI16` to the recorder (SigMF `ci16_le`, BLUE `CI`, name `.cs16`), a "16-bit" choice in the
   Record IQ format box and in Settings (`recFormat` 3). `guessFormat` reads `.cs16 / .ci16 / .s16`.
2. **The mode was written but never used on replay.** The mode id sat in the sidecar description, the library showed it, but opening a file left the
   receiver in whatever mode was selected. Now `restoreRecordingMode` (app) puts the receiver back in that mode, using `recordingStdMode(id)` (core,
   the inverse of `recordingModeId`). The id comes from the SigMF description, else from the `onair_<id>_...` file name. Centre frequency, rate and
   format were already restored; the centre is applied after the mode change because a mode change resets the frequency. "dvb" is DVB-T and T2 both, so
   it leaves the mode as chosen. Done by the Open button, the File info "Auto" button and the Library's Open.
3. **No way to hand a recording to someone.** Added "Share" to every Library row: opens the folder and copies the file path. A SigMF recording is the
   `.sigmf-meta` plus its data (the `.sigmf-data` hard link); the receiver replays it with its own frequency, rate and mode.

## Using it

* Record: the diagnostics row, "Record IQ" (8-bit, 16-bit, float or BLUE; SigMF box on by default). Decoded output: "Record audio" next to it.
* Replay: source "IQ recording file...", Open, pick the `.sigmf-meta` (or the library's Open). The mode, centre, rate and format fill in; press Start.
  No SDR is needed.
* Share: Library, Share, send the `.sigmf-meta` and the data file together.

## Tests

`test_replay` (`tests/source/test_replay.cpp`): the id to standard round trip for every registered mode; 16-bit SigMF recordings of a synthetic tone in a sweep
of modes, centres and rates read back from the metadata and the data (exact to one 16-bit step); 8-bit and float metadata; an engine recording of the
FM test signal as 16-bit SigMF replayed through the same decoder with no radio and the same receiver state. `test_iq_record`, `test_sigmf` unchanged.

## Not done

* Raw files without a sidecar still need "File info" (as before); BLUE descriptions are not read for the mode.
* The `--file` command-line path does not restore the mode (use `--standard`).
* Wide-band modes that bypass the engine's recorder (their own capture paths) record through their own panels.
