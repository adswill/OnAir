# Android IQ recording (3.8, issue #463)

Request: record the raw signal in the Android app so DRM, CDR, DTMB or anything that will not decode can be shared for analysis.

## What was built
- Core (shared with the desktop): the existing `IqRecorder` (CS8 file + SigMF `.sigmf-meta`: centre frequency, sample rate, mode id, UTC time, sample count) is reused. Additions: `setSigmfExtra` (`core:hw`, optional `core:geolocation`), `setMaxBytes` / `capReached` (size cap per recording, default stays the 4 GB safety cap), `sigmfAddExtra`. Engine passthroughs `setRecordExtra`, `setRecordMaxBytes`, `recordingCapReached`.
- JNI: `nativeRecStart / nativeRecStats / nativeRecStop` (tools/check_jni.py: 0 problems).
- Kotlin: `ReceiverRepository.startRecording / stopRecording` (only on the user's tap, never automatic), `data/IqRecording.kt` (state, folder, share, MediaStore copy), `RecordIqBar` under the receiver header. The header is the same in every mode, so the menu is in all of them.
- Menu (three dots): Record IQ, Share recording (Android share sheet, data file + metadata), Save to Downloads (MediaStore, `Download/OnAir`), size limit (256 MB to 4 GB, default 1 GB), privacy note. While recording the button turns into Stop and a line shows time and size against the limit; it turns to a warning at 90% and the recording stops by itself at the limit with a note.
- Storage: the app's own external folder `iq/` (scoped storage, no storage permission), shared through the existing FileProvider.
- Screen off: the recorder is a native thread in the process kept alive by the existing foreground service, so recording continues with the screen off. Stopping the receiver closes and finalises the file first.
- Nothing is uploaded; no network use.

## Not done / honest limits
- Location in the metadata: the core supports `core:geolocation`, but the Android app has no location permission, so it is not filled in (needs a settings opt-in and a permission; asked for, not added).
- No local build, emulator run or C++ test was done (battery rule); verification is on GitHub: Android workflow (debug APK with native library) and ci.yml. Emulator run with the built-in test signal is not done.
- `.sigmf-data` hard link is attempted by the core; on Android storage it may not succeed, so the shared pair is `<name>.cs8` + `<name>.sigmf-meta` (core:dataset names the .cs8).
- Tests added: `tests/source/test_sigmf.cpp` (hw, geolocation, JSON balance, cap stops the recording), `IqRecordingTest.kt` (formatting, stats parsing). Not run locally.

## Draft reply to #463 (not posted)
> thanks, good idea. added to the android app for 3.8: three dots under the frequency in any mode > Record IQ. it saves the raw iq (cs8 + sigmf metadata with freq, rate and mode) in the app folder, shows time and size, stops at a size limit (1 GB default, you can change it), and works with the screen off. then "Share recording" opens the share sheet, or "Save to Downloads". nothing gets uploaded anywhere. if drm/cdr/dtmb wont decode, record 20-30 s and send it over
