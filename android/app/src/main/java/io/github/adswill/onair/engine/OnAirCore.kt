// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.engine

import io.github.adswill.onair.OnAirNative
import io.github.adswill.onair.model.DeviceCaps
import io.github.adswill.onair.model.Demod
import io.github.adswill.onair.model.ModeTable
import io.github.adswill.onair.model.SourceKind
import io.github.adswill.onair.model.StartConfig
import io.github.adswill.onair.model.Telemetry
import io.github.adswill.onair.model.Tuning

/**
 * The JNI API ([OnAirNative]) with typed arguments and results. Pure translation, no state, no threads.
 *
 * THREADING RULE: call these only from the receiver thread, i.e. inside [ReceiverEngine.call] / [ReceiverEngine.post] blocks. The C++
 * bridge has one engine per process and a control mutex: a call from the main thread could block it for seconds (nativeStart waits
 * for a network radio). Every new native entry point gets a typed method here and is called the same way.
 */
class OnAirCore internal constructor() {
    val libraryLoaded: Boolean get() = OnAirNative.loaded

    /** Also hands the core the phone's xHE-AAC decoder for DRM sound (audio/XheAacDecoder.kt); a phone without one gets a status line instead. */
    fun init(resultsDir: String): Boolean = OnAirNative.nativeInit(resultsDir).also { if (it) io.github.adswill.onair.audio.XheAacDecoder.register() }
    fun modeTable(): ModeTable = ModeTable.parse(OnAirNative.nativeModeTable())
    fun setMode(family: Int, subId: String?): Tuning = Tuning.parse(OnAirNative.nativeSetMode(family, subId ?: ""))
    fun deviceInfo(kind: SourceKind, host: String, port: Int): DeviceCaps = DeviceCaps.parse(OnAirNative.nativeDeviceInfo(kind.code, host, port))

    /** Starts the source and the receiver; "" when it runs, else the reason it did not start. Blocks while a network radio answers. */
    fun start(c: StartConfig): String = OnAirNative.nativeStart(
        c.source.code, c.host, c.port, c.filePath, c.fileFormat.code, c.fileRate, c.freqHz, c.rateHz, c.gainDb, c.agc, c.biasTee, c.radio.encoded,
    )

    fun stop() = OnAirNative.nativeStop()
    fun running(): Boolean = OnAirNative.nativeRunning()
    fun note(): String = OnAirNative.nativeNote()
    fun retune(hz: Double) = OnAirNative.nativeRetune(hz)
    fun setGain(db: Double, agc: Boolean) = OnAirNative.nativeSetGain(db, agc)
    fun setRadio(radio: String, biasTee: Boolean) = OnAirNative.nativeSetRadio(radio, biasTee)

    /** Fills [out] (dB per bin) and [meta] (centre Hz, span Hz, rms dB, clip fraction); false when there is no new line. */
    fun spectrum(out: FloatArray, meta: DoubleArray): Boolean = OnAirNative.nativeSpectrum(out, meta)
    fun gainNow(): Double = OnAirNative.nativeGainNow()

    /** null when nothing changed since the last call (unless [force]). */
    fun telemetry(force: Boolean): Telemetry? = OnAirNative.nativeTelemetry(force).takeIf { it.isNotEmpty() }?.let { Telemetry.parse(it) }

    /** New lines of the C++ log since [since]; returns the new total and the lines. */
    fun log(since: Long): Pair<Long, List<String>> {
        val parts = OnAirNative.nativeLog(since).split('\n')
        val total = parts.firstOrNull()?.toLongOrNull() ?: since
        return total to parts.drop(1).filter { it.isNotEmpty() }
    }

    fun clearResults() = OnAirNative.nativeClearResults()
    fun setVolume(v: Float, mute: Boolean) = OnAirNative.nativeSetVolume(v, mute)
    fun general(demod: Demod, widthHz: Double, squelch: Boolean, squelchAuto: Boolean, squelchDb: Float) =
        OnAirNative.nativeGeneral(demod.ordinal, widthHz, squelch, squelchAuto, squelchDb)
    fun fmDeemphasis(us: Double) = OnAirNative.nativeFmDeemphasis(us)

    /** Starts an IQ recording into [dir]; Result with the file path, or the reason. Only on explicit user action. */
    fun recStart(dir: String, hw: String, hasGeo: Boolean, lat: Double, lon: Double, maxBytes: Long): Result<String> {
        val r = OnAirNative.nativeRecStart(dir, hw, hasGeo, lat, lon, maxBytes).split('\t', limit = 2)
        return if (r[0] == "ok") Result.success(r.getOrElse(1) { "" }) else Result.failure(IllegalStateException(r.getOrElse(1) { "recording failed" }))
    }
    fun recStats(): RecStats = RecStats.parse(OnAirNative.nativeRecStats())
    fun recStop() = OnAirNative.nativeRecStop()
    fun version(): String = OnAirNative.nativeVersion()

    /** CDR (#463): the service whose sound plays; -1 the first audio service, 0 none. */
    fun cdrListen(serviceId: Int) = OnAirNative.nativeCdrListen(serviceId)
    /** DRM (#645): the newest slide show picture when it is newer than [lastSeq], else null. */
    fun drmSlide(lastSeq: Long): DrmSlide? = DrmSlide.parse(OnAirNative.nativeDrmSlide(lastSeq))
    /** DRM (#645): one Journaline page ([objectId] < 0: the root); null when it has not arrived. */
    fun drmJournaline(objectId: Int): JlPage? = JlPage.parse(OnAirNative.nativeDrmJournaline(objectId))
    /** Points the core at the app's codecs folder ("" keeps the one set before) and looks for the DRA+ add-on again. */
    fun codecs(dir: String): CodecStatus = CodecStatus.parse(OnAirNative.nativeCodecs(dir))
}

/** The DRA+ add-on as the core found it: [found] usable; [path] the file; [problem] why a file that was found cannot be used. */
data class CodecStatus(val found: Boolean, val path: String, val problem: String) {
    companion object {
        val none = CodecStatus(false, "", "")
        fun parse(s: String): CodecStatus {
            val p = s.split('\t')
            return CodecStatus(p.getOrNull(0) == "1", p.getOrElse(1) { "" }, p.getOrElse(2) { "" })
        }
    }
}

/** The IQ recorder's state: seconds and bytes so far; [capReached] when it stopped at the size cap. */
data class RecStats(val active: Boolean, val seconds: Double, val bytes: Long, val dropped: Long, val capReached: Boolean, val error: String, val path: String) {
    companion object {
        val idle = RecStats(false, 0.0, 0L, 0L, false, "", "")
        fun parse(s: String): RecStats {
            val p = s.split('\t')
            if (p.size < 7) return idle
            return RecStats(p[0] == "1", p[1].toDoubleOrNull() ?: 0.0, p[2].toLongOrNull() ?: 0L, p[3].toLongOrNull() ?: 0L, p[4] == "1", p[5], p[6])
        }
    }
}

/** One MOT slide show picture of a DRM service (#645): [argb] w * h pixels for Bitmap.createBitmap. */
class DrmSlide(val seq: Long, val w: Int, val h: Int, val type: String, val caption: String, val argb: IntArray) {
    companion object {
        /** Layout of native/src/jni_onair.cpp nativeDrmSlide; null when the bytes do not fit. */
        fun parse(b: ByteArray): DrmSlide? {
            var o = 0
            fun u(n: Int): Long { var v = 0L; for (i in 0 until n) v = v or ((b[o + i].toLong() and 0xFF) shl (8 * i)); o += n; return v }
            fun str(): String { val n = u(2).toInt(); val r = String(b, o, n, Charsets.UTF_8); o += n; return r }
            if (b.size < 18) return null
            val seq = u(8); val w = u(4).toInt(); val h = u(4).toInt()
            val type = str(); val cap = str()
            if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || b.size - o < w * h * 4) return null
            val px = IntArray(w * h) { i ->
                val k = o + i * 4
                (0xFF shl 24) or ((b[k].toInt() and 0xFF) shl 16) or ((b[k + 1].toInt() and 0xFF) shl 8) or (b[k + 2].toInt() and 0xFF)
            }
            return DrmSlide(seq, w, h, type, cap, px)
        }
    }
}

/** One Journaline page (#645): type 1 menu, 2 text, 3 title only, 4 list. [links] are (object id, text); [lines] the text or list items. */
class JlPage(val type: Int, val title: String, val links: List<Pair<Int, String>>, val lines: List<String>) {
    companion object {
        fun parse(s: String): JlPage? {
            if (s.isEmpty()) return null
            val rows = s.split('\n')
            val head = rows[0].split('\t', limit = 2)
            val links = ArrayList<Pair<Int, String>>(); val lines = ArrayList<String>()
            for (r in rows.drop(1)) {
                val f = r.split('\t', limit = 3)
                if (f[0] == "L" && f.size == 3) f[1].toIntOrNull()?.let { links.add(it to f[2]) }
                else if (f[0] == "T" && f.size >= 2) lines.add(r.substring(2))
            }
            return JlPage(head[0].toIntOrNull() ?: 0, head.getOrElse(1) { "" }, links, lines)
        }
    }
}
