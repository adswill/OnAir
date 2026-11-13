// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.model

import org.json.JSONArray
import org.json.JSONObject

/**
 * Typed models of everything the JNI API returns or takes. The JSON shapes are produced by native/src/jni_onair.cpp and
 * native/src/results_json.cpp; parse them here and nowhere else, so the rest of the app only sees these classes.
 * All classes are immutable: they are safe to hand from the receiver thread to the UI.
 */

/** Where the IQ samples come from. [code] is the number the JNI bridge uses (enum Source in jni_onair.cpp). */
enum class SourceKind(val code: Int) {
    TEST_SIGNAL(0), IQ_FILE(1), RTL_TCP(2), SPYSERVER(3),
    /** A radio on the phone's USB port (usb/, docs/USB.md); which one: Settings.usbDevice. */
    USB(4),
    /** A network radio of the list (rtl_tcp, SpyServer, KiwiSDR, TCI, ...; netradio/); which one: Settings.netSelected. */
    NETWORK(5),
    /** The phone's sound input (microphone, USB audio) through AudioRecord (audio/SoundCardInput.kt). */
    SOUND_CARD(6);

    /** The highest sample rate this kind of source can give, for the "needs a wider radio" note of the mode list (USB and network:
     *  the radio decides, see RadioInfo.maxRateHz). */
    val maxRateHz: Double get() = when (this) { RTL_TCP -> 2.56e6; SPYSERVER -> 10e6; SOUND_CARD -> 48e3; else -> 1e12 }
    val isNetwork: Boolean get() = this == RTL_TCP || this == SPYSERVER || this == NETWORK

    companion object {
        fun of(code: Int): SourceKind = entries.firstOrNull { it.code == code } ?: TEST_SIGNAL
    }
}

/** Sample formats of a raw IQ file; [code] as the JNI bridge numbers them. */
enum class FileFormat(val code: Int, val extensions: List<String>) {
    CS8(0, listOf(".cs8", ".s8")), CU8(1, listOf(".cu8", ".u8", ".iq")), CF32(2, listOf(".cf32", ".fc32", ".cfile")),
    CI16(3, listOf(".ci16", ".s16")), CU16(4, listOf(".cu16"));

    companion object {
        fun of(code: Int): FileFormat = entries.firstOrNull { it.code == code } ?: CU8
        fun guess(fileName: String): FileFormat? = fileName.lowercase().let { n -> entries.firstOrNull { f -> f.extensions.any { n.endsWith(it) } } }
    }
}

/** What the engine says about one mode (nativeModeTable / nativeSetMode). Frequencies in MHz, rates in Hz. */
data class Tuning(
    val std: Int, val name: String, val sub: String, val defMhz: Double, val minMhz: Double, val maxMhz: Double,
    val rate: Double, val bwMhz: Double, val minRate: Double,
) {
    /** [mhz] limited to what the mode accepts. */
    fun clamp(mhz: Double): Double = mhz.coerceIn(minMhz.coerceAtLeast(0.001), maxOf(maxMhz, minMhz + 1.0))

    companion object {
        fun of(o: JSONObject) = Tuning(
            o.optInt("std"), o.optString("name"), o.optString("sub"), o.optDouble("defMhz", 100.0), o.optDouble("minMhz", 0.01),
            o.optDouble("maxMhz", 6000.0), o.optDouble("rate", 2e6), o.optDouble("bwMhz", 1.0), o.optDouble("minRate", 0.0),
        )
        fun parse(json: String): Tuning = of(JSONObject(json))
    }
}

/** The engine's mode table: tunings by engine standard and by protocol id (sub-modes). */
data class ModeTable(val byStd: Map<Int, Tuning>, val bySub: Map<String, Tuning>) {
    companion object {
        val empty = ModeTable(emptyMap(), emptyMap())
        fun parse(json: String): ModeTable {
            val t = JSONObject(json)
            val std = HashMap<Int, Tuning>()
            val sub = HashMap<String, Tuning>()
            t.getJSONArray("modes").let { a -> for (i in 0 until a.length()) Tuning.of(a.getJSONObject(i)).let { std[it.std] = it } }
            t.getJSONArray("subs").let { a -> for (i in 0 until a.length()) Tuning.of(a.getJSONObject(i)).let { sub[it.sub] = it } }
            return ModeTable(std, sub)
        }
    }
}

/** One setting a source offers (DeviceInfo::settings of the engine): shown generically on the Receiver tab. */
data class RadioSetting(
    val key: String, val label: String, val help: String, val type: String, val def: String, val min: Double, val max: Double,
    val step: Double, val unit: String, val values: List<String>, val names: List<String>,
)

/** What the selected source can do (nativeDeviceInfo). */
data class DeviceCaps(val gainMin: Double, val gainMax: Double, val maxRate: Double, val biasTee: Boolean, val settings: List<RadioSetting>) {
    companion object {
        val none = DeviceCaps(0.0, 0.0, 0.0, false, emptyList())
        fun parse(j: String): DeviceCaps = try {
            val o = JSONObject(j)
            val st = o.optJSONArray("settings") ?: JSONArray()
            DeviceCaps(o.optDouble("gainMin"), o.optDouble("gainMax"), o.optDouble("maxRate"), o.optBoolean("biasTee"),
                (0 until st.length()).map { i ->
                    val s = st.getJSONObject(i)
                    fun list(k: String) = s.optJSONArray(k)?.let { a -> (0 until a.length()).map { a.getString(it) } } ?: emptyList()
                    RadioSetting(s.optString("key"), s.optString("label"), s.optString("help"), s.optString("type"), s.optString("def"),
                        s.optDouble("min"), s.optDouble("max"), s.optDouble("step"), s.optString("unit"), list("values"), list("names"))
                })
        } catch (_: Exception) { none }
    }
}

/** The radio's own settings (ppm, direct sampling, ...) in the bridge's "key=value;key=value" form. */
@JvmInline
value class RadioOptions(val encoded: String) {
    fun value(key: String, def: String): String = encoded.split(';').firstOrNull { it.startsWith("$key=") }?.substring(key.length + 1) ?: def
    fun with(key: String, value: String): RadioOptions {
        val m = LinkedHashMap<String, String>()
        encoded.split(';').forEach { kv -> kv.indexOf('=').let { i -> if (i > 0) m[kv.substring(0, i)] = kv.substring(i + 1) } }
        m[key] = value
        return RadioOptions(m.entries.joinToString(";") { "${it.key}=${it.value}" })
    }
}

/** Everything nativeStart needs, in one value: the repository builds it, the engine thread passes it to the JNI call. */
data class StartConfig(
    val source: SourceKind, val host: String, val port: Int, val filePath: String, val fileFormat: FileFormat, val fileRate: Double,
    val freqHz: Double, val rateHz: Double, val gainDb: Double, val agc: Boolean, val biasTee: Boolean, val radio: RadioOptions,
)

/** A table of the active mode's results: column names as on the desktop, rows of text. */
data class ResultTable(val name: String, val cols: List<String>, val rows: List<List<String>>)

/** FM broadcast panel (telemetry "fm"). */
/** [rdsRows]: the other RDS items the station sends (PTYN, long name, country, AF list, clock, RT+, EON ...) as label / value pairs. */
data class FmInfo(val ps: String, val rt: String, val rdsSync: Boolean, val stereo: Boolean, val pi: Int, val pty: String, val rdsRows: List<Pair<String, String>> = emptyList())

/** General receiver panel (telemetry "general"). */
data class GeneralInfo(val squelchOpen: Boolean, val levelDb: Double, val audioDb: Double)

/** Receiver state flags of the telemetry (the engine's lock state: 0 searching, 1 converging, 2+ locked). */
enum class LockState { SEARCHING, CONVERGING, LOCKED }

/** One telemetry snapshot of the active mode (nativeTelemetry). */
data class Telemetry(
    val running: Boolean, val available: Boolean, val standard: Int, val state: Int, val locked: Boolean, val snrDb: Double?,
    val summary: String, val cards: List<Pair<String, String>>, val tables: List<ResultTable>, val fm: FmInfo?, val general: GeneralInfo?,
    /** CDR only (#463): the services and the DRA+ add-on state. */
    val cdr: CdrInfo? = null,
    /** A plain warning when a live radio loses part of the signal (#435), else "". */
    val linkNote: String = "",
) {
    val lock: LockState get() = when { locked -> LockState.LOCKED; state == 1 -> LockState.CONVERGING; else -> LockState.SEARCHING }

    companion object {
        const val STD_FM = 6
        const val STD_GENERAL = 56

        fun parse(json: String): Telemetry {
            val raw = JSONObject(json)
            val cards = raw.optJSONArray("cards")?.let { a -> (0 until a.length()).map { a.getJSONArray(it).let { c -> c.getString(0) to c.getString(1) } } } ?: emptyList()
            val tables = raw.optJSONArray("tables")?.let { a ->
                (0 until a.length()).map { i ->
                    val t = a.getJSONObject(i)
                    val cols = t.getJSONArray("cols").let { c -> (0 until c.length()).map { c.getString(it) } }
                    val rows = t.getJSONArray("rows").let { r -> (0 until r.length()).map { j -> r.getJSONArray(j).let { row -> (0 until row.length()).map { row.getString(it) } } } }
                    ResultTable(t.optString("name"), cols, rows)
                }
            } ?: emptyList()
            val fm = raw.optJSONObject("fm")?.let { FmInfo(it.optString("ps"), it.optString("rt"), it.optBoolean("rdsSync"), it.optBoolean("stereo"), it.optInt("pi"), it.optString("pty"),
                it.optJSONArray("rdsRows")?.let { a -> (0 until a.length()).mapNotNull { i -> a.optJSONArray(i)?.let { r -> r.optString(0) to r.optString(1) } } } ?: emptyList()) }
            val gen = raw.optJSONObject("general")?.let { GeneralInfo(it.optBoolean("squelchOpen"), it.optDouble("levelDb"), it.optDouble("audioDb")) }
            return Telemetry(
                raw.optBoolean("running"), raw.optBoolean("available"), raw.optInt("standard", -1), raw.optInt("state"), raw.optBoolean("locked"),
                if (!raw.has("snrDb") || raw.isNull("snrDb")) null else raw.optDouble("snrDb"),
                raw.optString("summary"), cards, tables, fm, gen, raw.optJSONObject("cdr")?.let { CdrInfo.parse(it) }, raw.optString("linkNote"),
            )
        }
    }
}

/** One CDR service as the bridge lists it. */
data class CdrService(val id: Int, val audio: Boolean, val name: String, val format: String)

/** CDR: the DRA+ add-on (found, or why not), the service that plays ([playing], 0 none) and the asked-for one ([listen], -1 automatic). */
data class CdrInfo(
    val draFound: Boolean, val draProblem: String, val draPath: String, val anyAudio: Boolean, val listen: Int, val playing: Int, val pcmFrames: Long,
    val services: List<CdrService>,
) {
    companion object {
        fun parse(o: JSONObject): CdrInfo {
            val a = o.optJSONArray("services")
            val svc = if (a == null) emptyList() else (0 until a.length()).map { i ->
                val x = a.getJSONObject(i)
                CdrService(x.optInt("id"), x.optBoolean("audio"), x.optString("name"), x.optString("format"))
            }
            return CdrInfo(o.optBoolean("draFound"), o.optString("draProblem"), o.optString("draPath"), o.optBoolean("anyAudio"), o.optInt("listen", -1),
                o.optInt("playing"), o.optLong("pcmFrames"), svc)
        }
    }
}

/** General receiver demodulators, in the order of the bridge's nativeGeneral(demod, ...). */
enum class Demod { NFM, WFM, AM, USB, LSB, CW }

/** One spectrum line from nativeSpectrum, plus the auto levels the waterfall is coloured with. Arrays are owned by the frame. */
class SpectrumFrame(
    val seq: Long, val db: FloatArray, val peak: FloatArray, val centreHz: Double, val spanHz: Double, val rmsDb: Double, val clip: Double,
    val wfFloorDb: Float, val wfCeilDb: Float,
) {
    val bins: Int get() = db.size

    companion object {
        const val BINS = 1024
        val empty = SpectrumFrame(0, FloatArray(BINS) { -120f }, FloatArray(BINS) { -120f }, 0.0, 0.0, -120.0, 0.0, -88f, -25f)
    }
}
