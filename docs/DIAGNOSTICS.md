# Diagnostic codes

Every failure or notable condition in OnAir carries a unique six-digit code next to a specific message:

```
[E352413] Writing the WAV recording /home/me/onair/radio_2026-11-13.wav failed (the disk may be full), so the recording stops.
```

The code is the same in every language and on every platform, and it appears in exactly one place in the source code. If you quote it in an
issue, a developer finds the line that produced it with one search. Failures that used to go unreported now have a debug-level diagnostic
with its own code. You see it only when debug diagnostics are on.

The DAB and DAB+ receiver (`core/src/dab`) is the first module with codes. The other modules follow one at a time.

## For users

### Reading and quoting a code

- `E` and six digits, in square brackets, at the start of the message. Copy the whole line: the code says where the condition happened, and
  the values in the message (file names, frequencies, sizes) say what was going on.
- Lines that start with `[v] ` are debug diagnostics. They describe something OnAir handled quietly: a picture that did not decode, a
  cache file that could not be written, the sync lost and found again. They are not errors by themselves.
- A code never changes and is never reused. A code quoted in an old issue still points to the same place, even after the message is reworded.

### Where codes appear

- **The log**: the app's log, where the verbose lines go too, and the terminal when OnAir runs without a window.
- **Status texts and dialogs**: for example, the DAB recording line says `stopped: [E352413] Writing the WAV recording ...`.
- **The web API** (planned with the server module): a separate `"code"` field next to the existing message field. The message text and
  the other fields keep their meaning.

### Turning on debug diagnostics

Any of these turns them on:

- View > Verbose logging on the desktop, or Settings > Verbose logging on Android. These switches also turn on the existing verbose lines.
- The environment variable `ONAIR_DEBUG_DIAG=1`, set before OnAir starts:

  ```
  ONAIR_DEBUG_DIAG=1 onair            # Linux, macOS
  set ONAIR_DEBUG_DIAG=1 && onair.exe # Windows
  ```

  Any value other than empty or `0` turns them on.

While debug diagnostics are off they cost nothing that can be measured. Each one costs one memory read and a branch, and nothing is
formatted. While they are on, a condition that repeats on every frame is reported at most once every few seconds, or as a count.

## For developers

### Adding a diagnostic (C++)

Include `dect2/common/diag.h` in the `.cpp` file. Do not include it in a header (see "Name clash" below). Get fresh codes from the lint
tool:

```
python3 tools/lint/diag_check.py --new 5
```

Then write the call with the code as a plain number:

```cpp
#include "dect2/common/diag.h"

if (avcodec_open2(ctx, codec, nullptr) < 0) {
    DIAG_DEBUG(613587, "The FFmpeg AAC decoder did not open for DAB+ header 0x%02X, so this service stays silent.", hdr);
    avcodec_free_context(&ctx);
    return false;
}
```

| form | level | when |
|---|---|---|
| `DIAG(code, fmt, ...)` | warning | a failure or condition the user should see in the log |
| `DIAG_ERROR(code, fmt, ...)` | error | a failure that stops what the user asked for |
| `DIAG_DEBUG(code, fmt, ...)` | debug | a failure that is handled quietly (a silent `return false`), at an operation boundary |
| `DIAG_EVERY(code, seconds, fmt, ...)` | warning | a rare condition on a busy path: at most once per `seconds` from this call site |
| `DIAG_DEBUG_EVERY(code, seconds, fmt, ...)` | debug | the same, at debug level. Nothing is evaluated while debug diagnostics are off. |
| `err = DIAG_TEXT(code, fmt, ...)` | none | the same `[E######] text` returned, not logged: error out-parameters and status texts that the interface shows |
| `DIAG_UI(code, tr("..."))` | none | a translated interface message: the code goes in front, outside the translation key |

- **Output.** The lines go to the process log (`diagEmit` in `dect2/common/verbose_log.h`). That is the app's log when it has set a sink,
  else stderr. Debug lines start with `[v] `, like the other verbose lines.
- **Compile-time checks.** A code that breaks the number rules does not compile, and neither does a code that is not a constant. A
  message that is not a string literal does not compile. The compiler checks printf placeholders against their arguments.
- **Hot paths.** Per-sample, per-symbol and per-frame code gets a counter in the telemetry, not a log line. Where a counter does not fit,
  use a `_EVERY` form, or one summary line from the code that runs once per report. Examples: `fibBad` in the DAB receiver, and the FIB
  summary in `decodeFic()`.

### The rules

1. **The code is a plain six-digit decimal literal at the call site**, in the file where the condition happens, on the same line as the
   call. There is no central table, no enum, no generated numbering, and no macro that builds the number from parts.
2. **Valid codes:**
   - 100000 to 999999;
   - the last digit is not 0;
   - not within ±1 of a power of two (131072, 262144, 524288);
   - more than 3 bits set;
   - not all digits the same.

   `--new` draws random codes over the whole range. Do not pick sequential or round numbers, or a block per module.
3. **Codes are permanent.** A reworded message keeps its code. A removed code is never used again. A code that leaves its file must be
   listed in `tools/lint/diag_retired.txt` as `retired` or `moved`. When a whole file is renamed, its codes go with it and need no entry.
4. **One message is one string literal.** Placeholders are fine; `+`, joined literals and variables are not.
5. **A message is a full sentence of at least 6 words.** It starts with a capital letter, digit or placeholder, and ends in `.`, `?` or
   `:`. It says what happened, with which value, and what OnAir does next. A message such as "failed" or "error" alone is rejected.
6. **Messages are unique in the tree.** The lint compares them after the placeholders are normalised.
7. **No behaviour change.** These things stay exactly as they are:
   - decoder output, timing, file formats, settings, CLI options, web API fields and plug-in interfaces;
   - machine values that something compares, such as `status == "codec needed"`, a DMB `note` checked by a test, or `r[0] == "ok"`.

   Text that a test, script or the web client parses keeps its wording. Add a diagnostic next to it instead.

### Name clash

`core/src/dvbs/dvbs_sf.cpp` is third-party code and has its own constant named `DIAG`. The header must never reach that file. That is why
it is included from `.cpp` files only. The lint fails if a header includes it, or if `dvbs_sf.cpp` does.

### The lint: `tools/lint/diag_check.py`

The lint uses the Python standard library only. The quick gate (`tools/gate/run.sh`) runs it twice:

- `diag_check.py --self-test` runs the rules on `tests/diag/fixtures/` (C++, Kotlin, Rust, Python and JS). It checks that every finding
  marked `expect:` in the fixtures is reported, and nothing else.
- `diag_check.py` checks the tree. Run it yourself before pushing.

It does this:

- **Lexing.** A small lexer per language resolves comments and string literals: C++ raw strings, Kotlin `"""` and templates, Rust
  `r#"..."#` and lifetimes, Python prefixes, JS template literals. A call inside a comment or a string is not a call site.
- **Checks.** It checks every call site against rules 1, 2 and 4 to 6. In C++, it reads only the files that include `diag.h`. A `DIAG(`
  with a code in a file without the header is reported.
- **Permanence.** It reads the history of `HEAD` once and caches the result in the git folder (`onair-diag-history.json`). After that it
  only reads the new commits. A committed code that is gone, or that turns up in another file while its old file still exists, fails
  unless `diag_retired.txt` lists it. A shallow clone (CI) checks the commits it has.
- **Warnings.** A `DIAG`, `DIAG_DEBUG` or `DIAG_ERROR` inside a `for` or `while` body gets a warning, not a failure. A per-sample or
  per-frame path wants a counter or an `_EVERY` form, while a loop that exits right after reporting is fine.
- **`--new N`.** Prints N random valid codes. They are not used in the tree, the history, the retire list or the fixtures.

### Other languages (planned, same rules)

The lint already reads these call forms. The helpers arrive with the first module of each language.

**Kotlin**: `android/.../diag/Diag.kt`, on top of `AppLog`.

```kotlin
object Diag {
    @JvmStatic fun e(code: Int, msg: String, t: Throwable? = null) = AppLog.e(TAG, "[E$code] $msg", t)
    @JvmStatic fun w(code: Int, msg: String, t: Throwable? = null) = AppLog.w(TAG, "[E$code] $msg", t)
    @Volatile var debugOn = false                 // set next to nativeSetVerbose()
    inline fun d(code: Int, msg: () -> String) { if (debugOn) AppLog.d(TAG, "[E$code] " + msg()) }   // nothing is built while off
    fun text(code: Int, msg: String) = "[E$code] $msg"
}
Diag.e(274913, "The IQ recording $path could not be written, so the recording stops.", e)
Diag.d(715349) { "The USB device ${dev.productName} answered the control request late, so it is opened again." }
```

**Rust**: `rust/onair_common/src/diag.rs`. Lines go to C++ through one FFI sink (`onair_set_diag_sink`), which C++ connects to `diagEmit()`.
The debug switch is an `AtomicBool` that C++ sets.

```rust
#[macro_export] macro_rules! diag_debug {
    ($code:literal, $fmt:literal $(, $a:expr)* $(,)?) => {
        if $crate::diag::debug_on() { $crate::diag::emit($crate::diag::Level::Debug, $code, format_args!($fmt $(, $a)*)) }
    };
}
diag_debug!(358927, "The FIG 0/1 subchannel list is {} bits, longer than its {} byte FIB, so it is skipped.", n_bits, buf.len());
```

`$code:literal` and `$fmt:literal` make a non-literal a compile error. `diag!` and `diag_text!` follow the same pattern.

**Python** (tools): `tools/diag.py`.

```python
def diag(code: int, msg: str, *args) -> None:      # to stderr
    print(f"[E{code}] " + (msg % args if args else msg), file=sys.stderr)
diag(482917, "The CTest timing file %s is missing, so the shards are split by test count instead.", path)
```

**JS** (web workers): `diag.js` in each worker package.

```js
export const diag = (code, msg) => console.error(`[E${code}] ${msg}`);
export const diagText = (code, msg) => `[E${code}] ${msg}`;
return jres({ error: diagText(518263, "Sign in with GitHub first: this badge action needs an account."), code: 518263 }, 401);
```

A JSON API adds the numeric `code` field. Clients that compare the `error` text keep working, and old app versions see the fields they
know.

## The DAB pilot

`core/src/dab` has the codes in all the files that OnAir owns. The FIG analyser (`dab_fig*`) is third-party code and has none. The
test-signal generators (`dab_gen*`) have none either.

- **Recording errors.** The recording line of the DAB panel now shows them with their codes, for example "could not be created" and
  "write failed".
- **Silent failures at operation boundaries** now have debug diagnostics with their own codes. They cover these areas:
  - FFmpeg and faad2 decoders that do not open, and FFmpeg packets that cannot be allocated;
  - the MP4 recording muxer;
  - SPI and EPG parsing, plus the programme guide cache: save, load and clean;
  - MOT reassembly and Journaline objects;
  - DMB section, PES and SL errors;
  - sync lost and found, reconfigurations, an ensemble identifier that changes, and sub-channels that match no protection profile.
- **Per-frame rejects** stay counters, as they were: FIB CRC, superframes and RS code words. A rate-limited summary line is added.
- **Decoder output does not change.** The DAB tests (`dab_*`) give the same results.
- **Constants.** The FIC analysis keeps a FIG it has not heard for 300 s. That limit is now the named constant
  `kFigKeepFrames = 3125` (300 s ÷ 0.096 s per mode I frame). Its value did not change. No other value was changed.

`tests/diag/test_diag.cpp` (ctest `diag`) checks the format, the switches, the rate limits and the cost while off.
