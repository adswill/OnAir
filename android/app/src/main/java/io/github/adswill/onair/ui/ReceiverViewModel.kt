package io.github.adswill.onair.ui

import android.app.Application
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Bitmap
import android.net.Uri
import android.os.BatteryManager
import android.os.PowerManager
import android.os.SystemClock
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.setValue
import androidx.core.content.ContextCompat
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import io.github.adswill.onair.AppGraph
import io.github.adswill.onair.Colormaps
import io.github.adswill.onair.Modes
import io.github.adswill.onair.TypeLook
import io.github.adswill.onair.data.ReceiverState
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.model.Demod
import io.github.adswill.onair.model.SourceKind
import io.github.adswill.onair.model.SpectrumFrame
import io.github.adswill.onair.model.Telemetry
import io.github.adswill.onair.settings.Settings
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChangedBy
import kotlinx.coroutines.launch

/** The level readings under the spectrum, refreshed a few times a second (not per frame: no recomposition storm). */
data class Meters(
    /** Estimated noise floor of the visible capture, dB (20th percentile of the spectrum bins). */
    val noiseFloorDb: Float = Float.NaN,
    /** Input level at the ADC, dBFS (the engine's RMS). */
    val adcDb: Float = Float.NaN,
    /** Fraction of clipped samples (0..1), from the engine. */
    val clip: Float = 0f,
)

/**
 * The UI's view of the receiver. State comes from the repository as StateFlows (collect them with collectAsStateWithLifecycle);
 * user actions go back as the intent functions below. Composables keep nothing but UI state (open dialogs, text being typed, zoom).
 * Survives rotation; there is one per activity.
 *
 * Contracts kept from the 0.3.0 architecture: [state], [settings], [telemetry], [agcGain], [log], [frame] and [waterfall] are the
 * way out; every function below returns at once. New in the UI rework: [ui] (interface preferences), [specVersion] and
 * [Waterfall.version] (what the spectrum and waterfall draws read), [meters], the frame cap on battery and the deep-link entry.
 */
class ReceiverViewModel(app: Application) : AndroidViewModel(app) {
    private val repo = AppGraph.repository
    private val uiStore = UiGraph.prefs

    val state: StateFlow<ReceiverState> = repo.state
    val settings: StateFlow<Settings> = repo.settings
    val telemetry: StateFlow<Telemetry?> = repo.telemetry
    val agcGain: StateFlow<Double> = repo.agcGain
    val log: StateFlow<List<AppLog.Entry>> = AppLog.entries
    val ui: StateFlow<UiPrefs> = uiStore.prefs
    val uiLoaded: StateFlow<Boolean> = uiStore.loaded

    /** The waterfall bitmap: written on the main thread only (here), drawn by SpectrumWaterfall. */
    val waterfall = Waterfall(SpectrumFrame.BINS, 256)
    private val _frame = MutableStateFlow(SpectrumFrame.empty)
    /** The newest spectrum line, after it went into [waterfall]: the spectrum view redraws on every new value. */
    val frame: StateFlow<SpectrumFrame> = _frame.asStateFlow()
    private val _specVersion = MutableStateFlow(0L)
    /** Bumped with every published frame; the spectrum draw reads it (with [Waterfall.version]) so each new line redraws. */
    val specVersion: StateFlow<Long> = _specVersion.asStateFlow()
    private val _meters = MutableStateFlow(Meters())
    val meters: StateFlow<Meters> = _meters.asStateFlow()

    private var lut = Colormaps.lut(settings.value.colormap)
    private var askedNotifications = false
    private val scratch = FloatArray(SpectrumFrame.BINS)
    private var lastMetersMs = 0L
    private var lastPublishMs = 0L

    /** Minimum time between redraws: none on mains power, 33 ms (30 fps) on battery or in power saving. */
    @Volatile private var minFrameMs = 0L
    private val power = object : BroadcastReceiver() {
        override fun onReceive(c: Context?, i: Intent?) = updatePowerCap()
    }

    init {
        viewModelScope.launch { settings.distinctUntilChangedBy { it.colormap }.collect { lut = Colormaps.lut(it.colormap) } }
        viewModelScope.launch {
            var last = -1L
            repo.spectrum.collect { f ->
                if (f.seq == last || f.seq == 0L) return@collect
                last = f.seq
                waterfall.push(f.db, f.wfFloorDb, f.wfCeilDb, lut)
                val now = SystemClock.uptimeMillis()
                if (now - lastMetersMs >= 250) { lastMetersMs = now; _meters.value = measure(f) }
                if (minFrameMs > 0 && now - lastPublishMs < minFrameMs) return@collect   // the next line redraws with this row in it
                lastPublishMs = now
                waterfall.commit()
                _frame.value = f
                _specVersion.value = f.seq
            }
        }
        try {
            val f = IntentFilter().apply {
                addAction(Intent.ACTION_POWER_CONNECTED); addAction(Intent.ACTION_POWER_DISCONNECTED)
                addAction(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED)
            }
            ContextCompat.registerReceiver(app, power, f, ContextCompat.RECEIVER_NOT_EXPORTED)
        } catch (e: Exception) { AppLog.w("app", "power state not followed", e) }
        updatePowerCap()
    }

    override fun onCleared() {
        try { getApplication<Application>().unregisterReceiver(power) } catch (_: Exception) {}
    }

    private fun updatePowerCap() {
        val ctx = getApplication<Application>()
        val pm = ctx.getSystemService(Context.POWER_SERVICE) as? PowerManager
        val bm = ctx.getSystemService(Context.BATTERY_SERVICE) as? BatteryManager
        val saving = pm?.isPowerSaveMode == true
        val charging = bm?.isCharging ?: true
        minFrameMs = if (saving || !charging) 33L else 0L
    }

    /** Noise floor as the 20th percentile of the bins (sorted copy into a reused array: no allocation per frame). */
    private fun measure(f: SpectrumFrame): Meters {
        val n = minOf(scratch.size, f.db.size)
        if (n == 0) return Meters()
        System.arraycopy(f.db, 0, scratch, 0, n)
        java.util.Arrays.sort(scratch, 0, n)
        return Meters(scratch[n / 5], f.rmsDb.toFloat(), f.clip.toFloat().coerceIn(0f, 1f))
    }

    /** True once per process: the first start asks for the notification permission (the foreground service shows one). */
    fun shouldAskNotifications(): Boolean = !askedNotifications.also { askedNotifications = true }

    // receive session
    fun toggle() = repo.toggle()
    fun selectMode(idx: Int) = repo.selectMode(idx)
    fun tune(mhz: Double) = repo.tune(mhz)
    fun clearError() = repo.clearError()
    fun clearResults() = repo.clearResults()
    fun setUiVisible(v: Boolean) = repo.setUiVisible(v)

    /** Mode and frequency together (deep links, bookmarks): the frequency is stored for the mode first, so the switch lands on it. */
    fun tuneTo(modeIdx: Int?, mhz: Double?) {
        val st = state.value
        if (modeIdx == null || modeIdx == st.modeIndex || modeIdx !in Modes.all.indices) { if (mhz != null && mhz > 0) tune(mhz); return }
        if (mhz != null && mhz > 0) repo.updateSettings { it.copy(freqMhz = it.freqMhz + (modeIdx to mhz)) }
        repo.selectMode(modeIdx)
    }

    // source and tuner
    fun chooseSource(kind: SourceKind) = repo.chooseSource(kind)
    fun setEndpoint(kind: SourceKind, host: String, port: Int) = repo.setEndpoint(kind, host, port)
    fun refreshCaps() = repo.refreshCaps()
    fun openFile(uri: Uri) = repo.openFile(uri)
    fun setFileFormat(code: Int) = repo.updateSettings { it.copy(fileFormat = io.github.adswill.onair.model.FileFormat.of(code)) }
    fun setFileRate(hz: Double) = repo.updateSettings { it.copy(fileRateHz = hz) }
    fun setRate(hz: Double) = repo.setRate(hz)
    fun setGain(db: Double, agc: Boolean) = repo.setGain(db, agc)
    fun setBiasTee(on: Boolean) = repo.setBiasTee(on)
    fun setRadioSetting(key: String, value: String) = repo.setRadioSetting(key, value)

    /** Use a saved network radio: its kind becomes the source and its address the endpoint. */
    fun useNetworkRadio(r: NetworkRadio) {
        repo.setEndpoint(r.kind, r.host, r.port)
        if (settings.value.source != r.kind) repo.chooseSource(r.kind) else repo.refreshCaps()
    }
    fun addNetworkRadio(r: NetworkRadio) = uiStore.update { u -> u.copy(networkRadios = u.networkRadios.filter { it.id != r.id } + r) }
    fun removeNetworkRadio(r: NetworkRadio) = uiStore.update { u -> u.copy(networkRadios = u.networkRadios.filter { it.id != r.id }) }

    // sound, general receiver
    fun setSound(volume: Float, muted: Boolean) = repo.setSound(volume, muted)
    fun setGeneral(demod: Demod, squelch: Boolean, squelchAuto: Boolean, squelchDb: Float) = repo.setGeneral(demod, squelch, squelchAuto, squelchDb)

    // display
    fun setColormap(i: Int) = repo.updateSettings { it.copy(colormap = i) }
    fun setRange(lo: Float, hi: Float) = repo.updateSettings { it.copy(minDb = lo, maxDb = hi) }
    fun autoRange() = repo.autoRange()
    fun setPeakHold(on: Boolean) = repo.setPeakHold(on)
    fun setVerbose(on: Boolean) = repo.setVerbose(on)
    fun setDark(on: Boolean) = repo.updateSettings { it.copy(dark = on) }
    fun setSheetFraction(f: Float) = repo.updateSettings { it.copy(sheetFraction = f) }
    fun setMapConsent(v: Int) = repo.updateSettings { it.copy(mapConsent = v) }

    // interface preferences
    fun toggleFavourite(idx: Int) = uiStore.update { u -> u.copy(favourites = if (idx in u.favourites) u.favourites - idx else u.favourites + idx) }
    fun setHidden(idx: Int, hidden: Boolean) = uiStore.update { u -> u.copy(hidden = if (hidden) u.hidden + idx else u.hidden - idx) }
    fun setHideUnused(on: Boolean) = uiStore.update { it.copy(hideUnused = on) }
    fun setOnlyCapable(on: Boolean) = uiStore.update { it.copy(onlyCapable = on) }
    fun setCentreTuning(on: Boolean) = uiStore.update { it.copy(centreTuning = on) }
    fun setStepHz(hz: Double) = uiStore.update { it.copy(stepHz = hz) }
    fun setDiscovery(on: Boolean) = uiStore.update { it.copy(discoveryOptIn = on) }
    fun setTypeLook(l: TypeLook) = uiStore.update { it.copy(typeLook = l) }
    fun setReduceMotion(on: Boolean) = uiStore.update { it.copy(reduceMotion = on) }
    fun setFollowSystemTheme(on: Boolean) = uiStore.update { it.copy(followSystemTheme = on) }
    fun finishOnboarding() = uiStore.update { it.copy(onboardingDone = true) }
    fun restartOnboarding() = uiStore.update { it.copy(onboardingDone = false) }
    fun markLesson(id: String) = uiStore.update { it.copy(lessonsDone = it.lessonsDone + id) }

    fun clearLog() = AppLog.clear()
}

/**
 * The waterfall: a ring of rows in one bitmap, newest row first. Main thread only. The bitmap is allocated once; each line writes one
 * row into it ([Bitmap.setPixels] from a reused IntArray), so nothing is allocated per frame. [version] is snapshot state: the
 * waterfall draw reads it, so a committed row redraws the canvas and nothing else.
 */
class Waterfall(val w: Int = 1024, val h: Int = 256) {
    val bmp: Bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888).also { it.eraseColor(0xFF000000.toInt()) }
    var top = 0
        private set
    /** Seconds per row, smoothed: the time axis of the waterfall. */
    var lineSec = 0.1f
        private set
    var version by mutableLongStateOf(0L)
        private set
    private val row = IntArray(w)
    private var lastNs = 0L

    fun push(db: FloatArray, minDb: Float, maxDb: Float, lut: IntArray) {
        val inv = 255f / maxOf(1f, maxDb - minDb)
        for (x in 0 until minOf(w, db.size)) {
            val v = (db[x] - minDb) * inv
            row[x] = lut[if (v <= 0f) 0 else if (v >= 255f) 255 else v.toInt()]
        }
        val now = System.nanoTime()
        if (lastNs != 0L) { val d = (now - lastNs) / 1e9f; if (d in 0.001f..5f) lineSec += (d - lineSec) * 0.1f }
        lastNs = now
        top = (top - 1 + h) % h
        bmp.setPixels(row, 0, w, 0, top, w, 1)
    }

    /** Makes the rows pushed so far visible: bumps [version], which the draw reads. Called once per published frame. */
    fun commit() { version++ }
}
