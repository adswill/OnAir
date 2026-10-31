package io.github.adswill.onair.data

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import io.github.adswill.onair.ModeDef
import io.github.adswill.onair.Modes
import io.github.adswill.onair.R
import io.github.adswill.onair.OnAirNative
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.diag.CrashLog
import io.github.adswill.onair.engine.ReceiverEngine
import io.github.adswill.onair.model.DeviceCaps
import io.github.adswill.onair.model.Demod
import io.github.adswill.onair.model.FileFormat
import io.github.adswill.onair.model.ModeTable
import io.github.adswill.onair.model.SourceKind
import io.github.adswill.onair.model.SpectrumFrame
import io.github.adswill.onair.model.StartConfig
import io.github.adswill.onair.model.Telemetry
import io.github.adswill.onair.model.Tuning
import io.github.adswill.onair.service.ReceiverService
import io.github.adswill.onair.settings.Settings
import io.github.adswill.onair.settings.SettingsStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/** Where the receiver is in its life. Only the repository changes it. */
enum class Phase { IDLE, STARTING, RUNNING, STOPPING }

/** A recording picked with the system picker; the engine reads it through [path] = /proc/self/fd/N while the fd is open. */
data class PickedFile(val name: String, val path: String)

/** The receiver's state: one immutable value, the single source of truth for the UI, the service and the notification. */
data class ReceiverState(
    val libraryLoaded: Boolean = false,
    val initialised: Boolean = false,
    val phase: Phase = Phase.IDLE,
    /** The last problem, in words for the user ("" = none). */
    val error: String = "",
    val modeIndex: Int = 0,
    val tuning: Tuning? = null,
    val freqMhz: Double = 100.0,
    val caps: DeviceCaps = DeviceCaps.none,
    val modeTable: ModeTable = ModeTable.empty,
    val file: PickedFile? = null,
    val coreVersion: String = "",
) {
    val running: Boolean get() = phase == Phase.RUNNING
    /** Starting, running or stopping: the play button shows "stop" and the foreground service stays. */
    val active: Boolean get() = phase != Phase.IDLE
    val mode: ModeDef get() = Modes.all[modeIndex]

    /** The engine's tuning of a mode of the list (its protocol's own entry first). */
    fun tuningFor(m: ModeDef): Tuning? = (m.subId?.let { modeTable.bySub[it] }) ?: modeTable.byStd[stdOf(m.family)]

    /** True when the mode needs a higher sample rate than [source] gives (the note in the mode list). */
    fun needsWideRadio(m: ModeDef, source: SourceKind): Boolean = (tuningFor(m)?.minRate ?: 0.0) > source.maxRateHz

    /** The tuned channel's width in Hz, for the band on the spectrum; 0 when the mode has none worth drawing. */
    fun channelHz(m: ModeDef = mode, maxMhz: Double = 1e9): Double =
        if (m.family == 5) 220e3 else (tuningFor(m)?.bwMhz ?: 0.0).let { if (it > 0.0 && it <= maxMhz) it * 1e6 else 0.0 }

    companion object {
        /** The engine's standard code of a desktop family number (app_state.cpp engineStd). */
        fun stdOf(f: Int) = when { f >= 6 -> f + 2; f == 1 -> 3; f == 2 -> 4; f == 3 -> 5; f == 4 -> 6; f == 5 -> 7; else -> 0 }
    }
}

/**
 * The repository around the JNI API: owns the [ReceiverEngine] (the receiver thread), the settings in memory and the receive session.
 * One per process (AppGraph); it outlives activities, so rotation and screen-off change nothing.
 *
 * Rules:
 *  - [state], [settings], [spectrum], [telemetry] are the only way out. The UI never calls the engine or JNI.
 *  - Intents (the public functions) may be called from the main thread; they never block. Multi-step operations (start, stop, mode
 *    change, source change) run one after another under [ops]; single native calls are queued on the engine as they come.
 *  - Settings live in [settings] (memory, authoritative while the process runs) and are written to DataStore in the background.
 */
class ReceiverRepository(private val ctx: Context, private val store: SettingsStore) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val ops = Mutex()

    private val _state = MutableStateFlow(ReceiverState())
    val state: StateFlow<ReceiverState> = _state.asStateFlow()

    private val _settings = MutableStateFlow(Settings())
    val settings: StateFlow<Settings> = _settings.asStateFlow()
    @Volatile private var settingsLoaded = false
    private val persist = Channel<Unit>(Channel.CONFLATED)

    private val engine = ReceiverEngine(
        onFailure = { where, t ->
            CrashLog.record(ctx, where, t)
            _state.update { it.copy(error = ctx.getString(R.string.engine_call_failed, where, t.message ?: t.javaClass.simpleName)) }
        },
        onEngineStopped = { note ->
            _state.update { if (it.phase == Phase.RUNNING) it.copy(phase = Phase.IDLE, error = note) else it }
        },
    )
    val spectrum: StateFlow<SpectrumFrame> = engine.frames
    val telemetry: StateFlow<Telemetry?> = engine.telemetry
    /** The gain the AGC chose (NaN when unknown or AGC is off). */
    val agcGain: StateFlow<Double> = engine.gainNow

    private var fileFd: ParcelFileDescriptor? = null

    /** USB radios, network radios, the sound-card input (data/RadioRepository.kt, docs/USB.md). */
    val radios = RadioRepository(
        ctx, scope,
        object : EngineExec { override suspend fun <T> call(name: String, block: () -> T): T = engine.call(name) { block() } },
        settings, ::updateSettings,
        onRadioLost = { msg -> stopBecause(msg) },
        onRadiosChanged = { if (_settings.value.source == SourceKind.USB) refreshCaps() },
    )

    init {
        scope.launch { for (u in persist) try { store.update { _settings.value } } catch (e: Exception) { AppLog.e("settings", "write failed", e) } }
        scope.launch { ops.withLock { initialise() } }
    }

    private suspend fun initialise() {
        _settings.value = try { store.data.first() } catch (e: Exception) { AppLog.e("settings", "read failed", e); Settings() }
        settingsLoaded = true
        val s = _settings.value
        engine.peakHold = s.peakHold
        engine.agc = s.agc
        if (s.verboseLog) { if (OnAirNative.loaded) OnAirNative.nativeSetVerbose(true); io.github.adswill.onair.diag.FrameStats.set(true) }
        val loaded = engine.libraryLoaded
        _state.update { it.copy(libraryLoaded = loaded) }
        if (!loaded) {
            AppLog.e("engine", "libonair.so did not load")
            _state.update { it.copy(error = ctx.getString(R.string.native_missing)) }
            return
        }
        try {
            val dir = ctx.filesDir.absolutePath + "/results"
            val (table, version) = engine.call("init") { core -> check(core.init(dir)) { "nativeInit returned false" }; core.modeTable() to core.version() }
            engine.ready = true
            _state.update { it.copy(initialised = true, modeTable = table, coreVersion = version) }
            AppLog.i("engine", "core $version, ${table.byStd.size} modes, ${table.bySub.size} protocols")
        } catch (e: Throwable) {
            _state.update { it.copy(error = ctx.getString(R.string.engine_init_failed, e.message ?: e.javaClass.simpleName)) }
            return
        }
        if (CrashLog.hasUnseen(ctx)) _state.update { it.copy(error = ctx.getString(R.string.previous_crash, CrashLog.file(ctx).absolutePath)) }
        val saved = s.modeIndex
        val idx = if (saved in Modes.all.indices) saved else Modes.all.indexOfFirst { it.family == 5 }.coerceAtLeast(0)
        applyMode(idx)
        radios.start()
        refreshCaps(s)
    }

    // ---- settings -------------------------------------------------------------------------------------------------------------
    /** Changes the settings in memory now and on disk soon. Ignored until the stored settings were read. */
    fun updateSettings(block: (Settings) -> Settings) {
        if (!settingsLoaded) return
        _settings.update(block)
        persist.trySend(Unit)
    }

    // ---- receive session ------------------------------------------------------------------------------------------------------
    fun toggle() { if (_state.value.active) stop() else start() }

    fun start() {
        val st = _state.value
        if (!st.initialised || st.phase != Phase.IDLE) return
        _state.update { it.copy(phase = Phase.STARTING, error = "") }
        ReceiverService.start(ctx)
        scope.launch { ops.withLock { doStart() } }
    }

    fun stop() {
        if (_state.value.phase == Phase.IDLE) return
        _state.update { it.copy(phase = Phase.STOPPING) }
        scope.launch { ops.withLock { doStop(); _state.update { it.copy(phase = Phase.IDLE) } } }
    }

    private suspend fun doStart() {
        if (_state.value.phase != Phase.STARTING) return          // stop was pressed before the start got its turn
        val s = _settings.value
        val st = _state.value
        // the radio-access sources: which radio, and why not when there is none
        val problem = when (s.source) {
            SourceKind.USB -> radios.usbProblem(s)
            SourceKind.NETWORK -> if (radios.networkFor(s) == null) "Add a network radio first (its address and port)." else ""
            SourceKind.SOUND_CARD -> radios.startSoundCard()
            else -> ""
        }
        if (problem.isNotEmpty()) {
            AppLog.w("engine", "not started: $problem")
            _state.update { it.copy(phase = Phase.IDLE, error = problem) }
            return
        }
        val (host, port) = endpoint(s)
        radios.activeUsbKey = if (s.source == SourceKind.USB) host else ""
        val cfg = StartConfig(
            s.source, host, port, st.file?.path ?: "", s.fileFormat, s.fileRateHz, st.freqMhz * 1e6, s.rateHz, s.gainDb, s.agc, s.biasTee,
            s.radioFor(s.source),
        )
        AppLog.i("engine", "start ${s.source} ${"%.6f".format(st.freqMhz)} MHz, mode ${st.modeIndex}")
        val err = try { engine.call("start") { it.start(cfg) } } catch (e: Throwable) { "Start failed: ${e.message ?: e.javaClass.simpleName}" }
        val running = err.isEmpty() && try { engine.call("running") { it.running() } } catch (_: Throwable) { false }
        if (running) {
            engine.agc = s.agc
            engine.polling = true
            applySound(s)
            applyGeneral(s)
            // a stop pressed during the start keeps STOPPING: its operation is queued behind this one and stops the engine
            _state.update { if (it.phase == Phase.STARTING) it.copy(phase = Phase.RUNNING) else it }
        } else {
            val why = err.ifEmpty { try { engine.call("note") { it.note() } } catch (_: Throwable) { "" } }
            AppLog.w("engine", "start failed: $why")
            if (err.isNotEmpty()) CrashLog.recordNative(ctx, "start", err)
            radios.activeUsbKey = ""
            radios.stopSoundCard()
            _state.update { it.copy(phase = Phase.IDLE, error = why) }
        }
    }

    private suspend fun doStop() {
        engine.polling = false
        try { engine.call("stop") { it.stop() } } catch (_: Throwable) {}
        radios.activeUsbKey = ""
        radios.stopSoundCard()
        AppLog.i("engine", "stopped")
    }

    /** The radio in use went away (USB unplugged): stop, and say why. Returns once the engine let go of it. */
    private suspend fun stopBecause(message: String) {
        ops.withLock {
            if (_state.value.phase != Phase.IDLE) doStop()
            _state.update { it.copy(phase = Phase.IDLE, error = message) }
        }
        AppLog.w("usb", message)
    }

    /** Stop, change, start again (when it ran): keeps the phase STARTING meanwhile, so the foreground service does not end. */
    private suspend fun restartAround(block: suspend () -> Unit) {
        val was = _state.value.phase == Phase.RUNNING || _state.value.phase == Phase.STARTING
        if (was) { _state.update { it.copy(phase = Phase.STARTING) }; doStop() }
        block()
        if (was && _state.value.phase == Phase.STARTING) doStart()
    }

    // ---- mode and source ------------------------------------------------------------------------------------------------------
    fun selectMode(idx: Int) {
        if (idx !in Modes.all.indices) return
        scope.launch { ops.withLock { restartAround { applyMode(idx) } } }
    }

    private suspend fun applyMode(idx: Int) {
        val m = Modes.all[idx]
        _state.update { it.copy(modeIndex = idx) }
        updateSettings { it.copy(modeIndex = idx) }
        if (!engine.ready) return
        val t = try { engine.call("setMode") { it.setMode(m.family, m.subId) } } catch (_: Throwable) { return }
        val f = t.clamp(_settings.value.freqFor(idx, t.defMhz))
        engine.clearTelemetry()
        _state.update { it.copy(tuning = t, freqMhz = f) }
        applyGeneral(_settings.value)
    }

    fun chooseSource(kind: SourceKind) {
        scope.launch {
            ops.withLock {
                if (_state.value.active) { doStop(); _state.update { it.copy(phase = Phase.IDLE) } }
                updateSettings { it.copy(source = kind) }
                refreshCaps(_settings.value)
            }
        }
    }

    /** Network endpoint changed (host or port typed): stored now, the capabilities are asked again. */
    fun setEndpoint(kind: SourceKind, host: String, port: Int) {
        updateSettings { if (kind == SourceKind.SPYSERVER) it.copy(spyHost = host, spyPort = port) else it.copy(rtlHost = host, rtlPort = port) }
    }

    fun refreshCaps() { scope.launch { refreshCaps(_settings.value) } }

    private suspend fun refreshCaps(s: Settings) {
        if (!engine.ready) return
        val (h, p) = endpoint(s)
        val caps = try { engine.call("deviceInfo") { it.deviceInfo(s.source, h, p) } } catch (_: Throwable) { DeviceCaps.none }
        _state.update { it.copy(caps = caps) }
    }

    private fun endpoint(s: Settings): Pair<String, Int> = when (s.source) {
        SourceKind.RTL_TCP -> s.rtlHost to s.rtlPort
        SourceKind.SPYSERVER -> s.spyHost to s.spyPort
        SourceKind.USB -> (radios.usbFor(s)?.key ?: "") to 0          // the descriptor's registry name
        SourceKind.NETWORK -> (radios.networkFor(s)?.spec ?: "") to 0 // the core's text form of the radio
        else -> "" to 0
    }

    // ---- tuning and radio -----------------------------------------------------------------------------------------------------
    fun tune(mhz: Double) {
        val st = _state.value
        val v = st.tuning?.clamp(mhz) ?: mhz
        _state.update { it.copy(freqMhz = v) }
        val idx = st.modeIndex
        updateSettings { it.copy(freqMhz = it.freqMhz + (idx to v)) }
        if (engine.ready) engine.post("retune") { if (it.running()) it.retune(v * 1e6) }
    }

    fun setGain(db: Double, agc: Boolean) {
        updateSettings { it.copy(gainDb = db, agc = agc) }
        engine.agc = agc
        if (engine.ready) engine.post("setGain") { if (it.running()) it.setGain(db, agc) }
    }

    fun setBiasTee(on: Boolean) {
        updateSettings { it.copy(biasTee = on) }
        val s = _settings.value
        if (engine.ready) engine.post("setRadio") { if (it.running()) it.setRadio(s.radioFor(s.source).encoded, on) }
    }

    fun setRadioSetting(key: String, value: String) {
        val kind = _settings.value.source
        updateSettings { it.copy(radio = it.radio + (kind to it.radioFor(kind).with(key, value))) }
        val s = _settings.value
        if (engine.ready) engine.post("setRadio") { if (it.running()) it.setRadio(s.radioFor(kind).encoded, s.biasTee) }
    }

    fun setRate(hz: Double) = updateSettings { it.copy(rateHz = hz) }

    // ---- sound, general receiver, display -------------------------------------------------------------------------------------
    fun setSound(volume: Float, muted: Boolean) {
        updateSettings { it.copy(volume = volume, muted = muted) }
        applySound(_settings.value)
    }

    private fun applySound(s: Settings) { if (engine.ready) engine.post("setVolume") { it.setVolume(s.volume, s.muted) } }

    fun setGeneral(demod: Demod, squelch: Boolean, squelchAuto: Boolean, squelchDb: Float) {
        updateSettings { it.copy(genDemod = demod, genSquelch = squelch, genSquelchAuto = squelchAuto, genSquelchDb = squelchDb) }
        applyGeneral(_settings.value)
    }

    private fun applyGeneral(s: Settings) {
        if (!engine.ready) return
        engine.post("general") { it.general(s.genDemod, 0.0, s.genSquelch, s.genSquelchAuto, s.genSquelchDb); it.setVolume(s.volume, s.muted) }
    }

    /** Verbose logging: the core writes the extra lines, this app logs the switch itself and the frame times of its own UI are in the same log. */
    fun setVerbose(on: Boolean) {
        if (OnAirNative.loaded) OnAirNative.nativeSetVerbose(on)
        io.github.adswill.onair.diag.FrameStats.set(on)
        AppLog.i("app", if (on) "verbose logging on" else "verbose logging off")
        updateSettings { it.copy(verboseLog = on) }
    }

    fun setPeakHold(on: Boolean) { engine.peakHold = on; updateSettings { it.copy(peakHold = on) } }

    /** Colour range from the last spectrum line: floor at the 20 % quantile. */
    fun autoRange() {
        val s = spectrum.value.db.copyOf().also { it.sort() }
        if (s.isEmpty()) return
        val floor = s[s.size / 5]
        updateSettings { it.copy(minDb = floor - 8f, maxDb = maxOf(s[s.size - 1] + 6f, floor + 30f)) }
    }

    fun clearResults() { if (engine.ready) engine.post("clearResults") { it.clearResults() } }

    // ---- misc -----------------------------------------------------------------------------------------------------------------
    /** An activity is visible (onStart/onStop): spectrum lines are only read while one is. */
    fun setUiVisible(visible: Boolean) { engine.uiVisible = visible }

    fun clearError() {
        if (CrashLog.hasUnseen(ctx)) CrashLog.markSeen(ctx)
        _state.update { it.copy(error = "") }
    }

    /** A recording chosen with the system picker (no storage permission): the fd stays open until the next pick. */
    fun openFile(uri: Uri) {
        try {
            val cr = ctx.contentResolver
            val name = cr.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
                ?: uri.lastPathSegment ?: "file"
            val pfd = cr.openFileDescriptor(uri, "r") ?: return
            val old = fileFd
            fileFd = pfd
            _state.update { it.copy(file = PickedFile(name, "/proc/self/fd/${pfd.fd}")) }
            if (old != null) { if (_state.value.active) stop(); scope.launch { ops.withLock { old.close() } } }   // after any running engine let go of it
            val fmt = FileFormat.guess(name)
            val rate = Regex("""(\d+(?:[.,]\d+)?)\s*([mk])sps""", RegexOption.IGNORE_CASE).find(name)?.let { m ->
                val v = m.groupValues[1].replace(',', '.').toDouble()
                if (m.groupValues[2].equals("m", true)) v * 1e6 else v * 1e3
            }
            updateSettings { it.copy(fileFormat = fmt ?: it.fileFormat, fileRateHz = rate ?: it.fileRateHz) }
            AppLog.i("app", "file $name (${fmt ?: "format unknown"})")
        } catch (e: Exception) {
            AppLog.e("app", "file open failed", e)
            _state.update { it.copy(error = ctx.getString(R.string.file_open_failed)) }
        }
    }
}
