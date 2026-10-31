package io.github.adswill.onair.settings

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.SharedPreferencesMigration
import androidx.datastore.preferences.core.MutablePreferences
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.floatPreferencesKey
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.model.Demod
import io.github.adswill.onair.model.FileFormat
import io.github.adswill.onair.model.RadioOptions
import io.github.adswill.onair.model.SourceKind
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.map
import java.io.IOException

/**
 * Everything that stays between runs, as one immutable value. Nothing leaves the phone, there is no account.
 * Stored with Jetpack DataStore (file datastore/settings.preferences_pb). The 0.2.x app kept the same keys in the SharedPreferences
 * file "onair"; [SharedPreferencesMigration] copies them over once, with their names and types, and deletes the old file.
 */
data class Settings(
    val source: SourceKind = SourceKind.TEST_SIGNAL,
    val rtlHost: String = "127.0.0.1",
    val rtlPort: Int = 1234,
    val spyHost: String = "",
    val spyPort: Int = 5555,
    val fileFormat: FileFormat = FileFormat.CU8,
    val fileRateHz: Double = 2.4e6,
    /** Index into Modes.all; -1 = never chosen (the app picks FM broadcast). */
    val modeIndex: Int = -1,
    val gainDb: Double = 30.0,
    val agc: Boolean = false,
    val biasTee: Boolean = false,
    /** 0 = the mode's own rate. */
    val rateHz: Double = 0.0,
    val colormap: Int = 4,
    val minDb: Float = -88f,
    val maxDb: Float = -25f,
    val volume: Float = 0.8f,
    val muted: Boolean = false,
    val dark: Boolean = true,
    val peakHold: Boolean = true,
    val genDemod: Demod = Demod.NFM,
    val genSquelch: Boolean = true,
    val genSquelchAuto: Boolean = true,
    val genSquelchDb: Float = -60f,
    val sheetFraction: Float = 0.4f,
    /** Online map tiles: 0 not asked, 1 yes, 2 no. */
    val mapConsent: Int = 0,
    /** Last frequency per mode index, MHz. */
    val freqMhz: Map<Int, Double> = emptyMap(),
    /** The radio's own settings per source kind. */
    val radio: Map<SourceKind, RadioOptions> = emptyMap(),
    /** The USB radio of the USB source, "vid:pid:serial" (data/RadioRepository.prefOf); "" = the first one ready. */
    val usbDevice: String = "",
    /** The network radios (netradio/NetworkRadioList: the core's text form plus labels) and the one in use. */
    val netRadios: String = "",
    val netSelected: Int = 0,
    /** mDNS discovery of network radios: off unless the user turns it on (no network scans by default). */
    val netDiscovery: Boolean = false,
    /** Verbose logging (3.5): USB transfer counts, bytes/s, stall restarts, frame times into the log. Off unless the user turns it on. */
    val verboseLog: Boolean = false,
) {
    fun freqFor(modeIndex: Int, def: Double): Double = freqMhz[modeIndex]?.takeIf { it > 0 } ?: def
    fun radioFor(kind: Int): RadioOptions = radio[SourceKind.of(kind)] ?: RadioOptions("")
    fun radioFor(kind: SourceKind): RadioOptions = radio[kind] ?: RadioOptions("")
}

private val Context.settingsStore: DataStore<Preferences> by preferencesDataStore(
    name = "settings",
    produceMigrations = { ctx -> listOf(SharedPreferencesMigration(ctx, "onair")) },
)

/** Reads and writes [Settings]. One instance per process (AppGraph). Writes are suspend calls; DataStore serialises them. */
class SettingsStore(ctx: Context) {
    private val store = ctx.applicationContext.settingsStore

    private object K {
        // the key names of the 0.2.x SharedPreferences (migrated as they are): do not rename
        val source = intPreferencesKey("source")
        val host = stringPreferencesKey("host")
        val rtlPort = intPreferencesKey("rtlPort")
        val spyHost = stringPreferencesKey("spyHost")
        val spyPort = intPreferencesKey("spyPort")
        val fileFormat = intPreferencesKey("fileFormat")
        val fileRate = floatPreferencesKey("fileRate")
        val mode = intPreferencesKey("mode")
        val gain = floatPreferencesKey("gain")
        val agc = booleanPreferencesKey("agc")
        val bias = booleanPreferencesKey("bias")
        val rate = floatPreferencesKey("rate")
        val colormap = intPreferencesKey("colormap")
        val minDb = floatPreferencesKey("minDb")
        val maxDb = floatPreferencesKey("maxDb")
        val volume = floatPreferencesKey("volume")
        val muted = booleanPreferencesKey("muted")
        val dark = booleanPreferencesKey("dark")
        val peakHold = booleanPreferencesKey("peakHold")
        val genDemod = intPreferencesKey("genDemod")
        val genSquelch = booleanPreferencesKey("genSquelch")
        val genSquelchAuto = booleanPreferencesKey("genSquelchAuto")
        val genSquelchDb = floatPreferencesKey("genSquelchDb")
        val sheet = floatPreferencesKey("sheet")
        val mapConsent = intPreferencesKey("mapConsent")
        // new in 0.3.0: bumped when a key changes meaning, so a later migration knows what it reads
        val schema = intPreferencesKey("schema")
        // radio access (android-usb-030)
        val usbDevice = stringPreferencesKey("usbDevice")
        val netRadios = stringPreferencesKey("netRadios")
        val netSelected = intPreferencesKey("netSelected")
        val netDiscovery = booleanPreferencesKey("netDiscovery")
        val verboseLog = booleanPreferencesKey("verboseLog")
        fun freq(mode: Int) = stringPreferencesKey("f.$mode")
        fun radio(kind: SourceKind) = stringPreferencesKey("radio.${kind.code}")
    }

    val data: Flow<Settings> = store.data
        .catch { e -> if (e is IOException) { AppLog.e("settings", "settings unreadable, using defaults", e); emit(androidx.datastore.preferences.core.emptyPreferences()) } else throw e }
        .map(::read)

    private fun read(p: Preferences): Settings {
        val d = Settings()
        val freqs = HashMap<Int, Double>()
        val radios = HashMap<SourceKind, RadioOptions>()
        for ((k, v) in p.asMap()) {
            val n = k.name
            if (n.startsWith("f.")) n.substring(2).toIntOrNull()?.let { m -> (v as? String)?.toDoubleOrNull()?.let { freqs[m] = it } }
            else if (n.startsWith("radio.")) n.substring(6).toIntOrNull()?.let { c -> (v as? String)?.let { radios[SourceKind.of(c)] = RadioOptions(it) } }
        }
        return Settings(
            source = SourceKind.of(p[K.source] ?: d.source.code),
            rtlHost = p[K.host] ?: d.rtlHost,
            rtlPort = p[K.rtlPort] ?: d.rtlPort,
            spyHost = p[K.spyHost] ?: d.spyHost,
            spyPort = p[K.spyPort] ?: d.spyPort,
            fileFormat = FileFormat.of(p[K.fileFormat] ?: d.fileFormat.code),
            fileRateHz = p[K.fileRate]?.toDouble() ?: d.fileRateHz,
            modeIndex = p[K.mode] ?: d.modeIndex,
            gainDb = p[K.gain]?.toDouble() ?: d.gainDb,
            agc = p[K.agc] ?: d.agc,
            biasTee = p[K.bias] ?: d.biasTee,
            rateHz = p[K.rate]?.toDouble() ?: d.rateHz,
            colormap = p[K.colormap] ?: d.colormap,
            minDb = p[K.minDb] ?: d.minDb,
            maxDb = p[K.maxDb] ?: d.maxDb,
            volume = p[K.volume] ?: d.volume,
            muted = p[K.muted] ?: d.muted,
            dark = p[K.dark] ?: d.dark,
            peakHold = p[K.peakHold] ?: d.peakHold,
            genDemod = Demod.entries.getOrElse(p[K.genDemod] ?: 0) { Demod.NFM },
            genSquelch = p[K.genSquelch] ?: d.genSquelch,
            genSquelchAuto = p[K.genSquelchAuto] ?: d.genSquelchAuto,
            genSquelchDb = p[K.genSquelchDb] ?: d.genSquelchDb,
            sheetFraction = p[K.sheet] ?: d.sheetFraction,
            mapConsent = p[K.mapConsent] ?: d.mapConsent,
            freqMhz = freqs,
            radio = radios,
            usbDevice = p[K.usbDevice] ?: d.usbDevice,
            netRadios = p[K.netRadios] ?: d.netRadios,
            netSelected = p[K.netSelected] ?: d.netSelected,
            netDiscovery = p[K.netDiscovery] ?: d.netDiscovery,
            verboseLog = p[K.verboseLog] ?: d.verboseLog,
        )
    }

    /** Applies [block] to the stored settings and writes the result. Only the keys whose value changed are written. */
    suspend fun update(block: (Settings) -> Settings) {
        store.edit { p ->
            val old = read(p)
            val s = block(old)
            if (s == old) return@edit
            write(p, s, old)
        }
    }

    private fun write(p: MutablePreferences, s: Settings, old: Settings) {
        fun <T> put(k: Preferences.Key<T>, v: T, o: T) { if (v != o) p[k] = v }
        put(K.source, s.source.code, old.source.code)
        put(K.host, s.rtlHost, old.rtlHost)
        put(K.rtlPort, s.rtlPort, old.rtlPort)
        put(K.spyHost, s.spyHost, old.spyHost)
        put(K.spyPort, s.spyPort, old.spyPort)
        put(K.fileFormat, s.fileFormat.code, old.fileFormat.code)
        put(K.fileRate, s.fileRateHz.toFloat(), old.fileRateHz.toFloat())
        put(K.mode, s.modeIndex, old.modeIndex)
        put(K.gain, s.gainDb.toFloat(), old.gainDb.toFloat())
        put(K.agc, s.agc, old.agc)
        put(K.bias, s.biasTee, old.biasTee)
        put(K.rate, s.rateHz.toFloat(), old.rateHz.toFloat())
        put(K.colormap, s.colormap, old.colormap)
        put(K.minDb, s.minDb, old.minDb)
        put(K.maxDb, s.maxDb, old.maxDb)
        put(K.volume, s.volume, old.volume)
        put(K.muted, s.muted, old.muted)
        put(K.dark, s.dark, old.dark)
        put(K.peakHold, s.peakHold, old.peakHold)
        put(K.genDemod, s.genDemod.ordinal, old.genDemod.ordinal)
        put(K.genSquelch, s.genSquelch, old.genSquelch)
        put(K.genSquelchAuto, s.genSquelchAuto, old.genSquelchAuto)
        put(K.genSquelchDb, s.genSquelchDb, old.genSquelchDb)
        put(K.sheet, s.sheetFraction, old.sheetFraction)
        put(K.mapConsent, s.mapConsent, old.mapConsent)
        put(K.usbDevice, s.usbDevice, old.usbDevice)
        put(K.netRadios, s.netRadios, old.netRadios)
        put(K.netSelected, s.netSelected, old.netSelected)
        put(K.netDiscovery, s.netDiscovery, old.netDiscovery)
        put(K.verboseLog, s.verboseLog, old.verboseLog)
        p[K.schema] = SCHEMA
        for ((m, f) in s.freqMhz) if (old.freqMhz[m] != f) p[K.freq(m)] = f.toString()
        for ((k, r) in s.radio) if (old.radio[k] != r) p[K.radio(k)] = r.encoded
    }

    companion object { const val SCHEMA = 1 }
}
