// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.audio

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import io.github.adswill.onair.diag.AppLog
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** What xHE-AAC needs from the phone, as plain decisions (XheAacTest). The codec itself is the phone's: OnAir ships none. */
object XheSupport {
    const val NO_DECODER = "This phone has no xHE-AAC decoder; DRM audio of this service cannot play"
    /** MediaCodecInfo.CodecProfileLevel.AACObjectXHE (audio object type 42, USAC), written out because it is API 28 and later. */
    const val AAC_OBJECT_XHE = 42
    const val MIME = "audio/mp4a-latm"
    /** Android decodes xHE-AAC from API 28 (Android 9) on. */
    const val MIN_SDK = 28

    /** "" when playing is possible, else the line for the status. [hasUsacCodec]: a decoder with profile 42 is listed. */
    fun reason(sdk: Int, hasUsacCodec: Boolean): String = if (sdk < MIN_SDK || !hasUsacCodec) NO_DECODER else ""

    /** Whether a codec's profile list (profile numbers) contains xHE-AAC. */
    fun listsXhe(profiles: IntArray): Boolean = profiles.any { it == AAC_OBJECT_XHE }

    /** How long to wait for the first output of a frame: short while the decoder primes, longer once it answers one output per input. */
    fun outputWaitUs(primed: Boolean, inFlight: Long): Long = if (primed && inFlight > 0) 20_000L else 3_000L

    /** 16-bit samples from a PCM buffer: 16-bit as it is, float converted. [floatPcm]: the output format said ENCODING_PCM_FLOAT. */
    fun toShorts(buf: ByteBuffer, floatPcm: Boolean): ShortArray {
        buf.order(ByteOrder.nativeOrder())
        if (!floatPcm) { val n = buf.remaining() / 2; return ShortArray(n).also { buf.asShortBuffer().get(it) } }
        val n = buf.remaining() / 4
        val fb = buf.asFloatBuffer()
        return ShortArray(n) { (fb.get().coerceIn(-1f, 1f) * 32767f).toInt().toShort() }
    }
}

/** Counters for the log: frames in, decoded buffers out, calls that returned nothing, busy input buffers, drops, errors, flushes. */
class XheStats {
    var framesIn = 0L; var framesOut = 0L; var empty = 0L; var inputBusy = 0L; var inputDropped = 0L; var errors = 0L; var flushes = 0L
    var framesInAtFlush = 0L; var framesOutAtFlush = 0L
    /** Frames given to the codec whose output has not come back since the last flush (never below 0). */
    fun inFlight(): Long = maxOf(0L, (framesIn - framesInAtFlush) - (framesOut - framesOutAtFlush) - inputDropped.coerceAtMost(framesIn))
    fun line(): String = "xHE-AAC frames in $framesIn, out $framesOut, empty calls $empty, input busy $inputBusy, dropped $inputDropped, errors $errors, flushes $flushes"
}

/**
 * Plays DRM xHE-AAC (USAC) with the phone's own MediaCodec decoder, for the core (native/src/jni_xhe.cpp calls the methods below by name and
 * signature: keep them in step). The core cuts the DRM audio super frame into raw access units and builds the AudioSpecificConfig (audio
 * object type 42) from the SDC (core/src/drm/drm_audio.cpp xheAscFromSdc); this class only feeds MediaCodec ("csd-0" = that config) and hands the
 * 16-bit PCM back. Synchronous MediaCodec, one thread at a time (the methods are synchronized).
 */
class XheAacDecoder {
    private var codec: MediaCodec? = null
    private var rate = 0
    private var channels = 0
    private var floatPcm = false
    private var ptsUs = 0L
    private val info = MediaCodec.BufferInfo()
    private var asc: ByteArray? = null
    private var cfgRate = 0
    private var cfgChannels = 0
    private var pending = ShortArray(0)
    private val stats = XheStats()

    private fun codecName(): String? = try {
        MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.firstOrNull { ci ->
            !ci.isEncoder && ci.supportedTypes.any { it.equals(XheSupport.MIME, ignoreCase = true) } &&
                XheSupport.listsXhe(profilesOf(ci))
        }?.name
    } catch (e: Throwable) { AppLog.e("xhe", "MediaCodecList failed", e); null }

    private fun profilesOf(ci: MediaCodecInfo): IntArray =
        ci.getCapabilitiesForType(XheSupport.MIME).profileLevels.map { it.profile }.toIntArray()

    /** "" when this phone can decode xHE-AAC, else the status line. */
    @Synchronized fun unavailableReason(): String =
        XheSupport.reason(Build.VERSION.SDK_INT, Build.VERSION.SDK_INT >= XheSupport.MIN_SDK && codecName() != null)

    /** Starts a decoder for the stream: [asc] is the AudioSpecificConfig. "" when it runs, else why not. */
    @Synchronized fun open(asc: ByteArray, rateHz: Int, channelCount: Int): String {
        close()
        val name = (if (Build.VERSION.SDK_INT >= XheSupport.MIN_SDK) codecName() else null) ?: return XheSupport.NO_DECODER
        return try {
            val fmt = MediaFormat.createAudioFormat(XheSupport.MIME, rateHz, channelCount)
            fmt.setInteger(MediaFormat.KEY_AAC_PROFILE, XheSupport.AAC_OBJECT_XHE)
            fmt.setByteBuffer("csd-0", ByteBuffer.wrap(asc))
            val c = MediaCodec.createByCodecName(name)
            try {
                c.configure(fmt, null, null, 0)
                c.start()
            } catch (t: Throwable) { try { c.release() } catch (_: Throwable) {}; throw t }
            codec = c; rate = rateHz; channels = channelCount; floatPcm = false; ptsUs = 0
            this.asc = asc.copyOf(); cfgRate = rateHz; cfgChannels = channelCount; pending = ShortArray(0)
            AppLog.i("xhe", "xHE-AAC decoder $name started: $rateHz Hz, $channelCount channel(s)")
            ""
        } catch (t: Throwable) {
            AppLog.e("xhe", "xHE-AAC decoder $name did not start", t)
            "The phone's xHE-AAC decoder ($name) did not accept this stream: ${t.message ?: t.javaClass.simpleName}"
        }
    }

    /**
     * One access unit in; the PCM that came out (16-bit interleaved, maybe empty while the decoder primes), or null only when no decoder is open.
     * Output is never discarded: whatever the codec produced during a failed or late call is returned with the next one.
     */
    @Synchronized fun decode(au: ByteArray): ShortArray? {
        val c = codec ?: return null
        stats.framesIn++
        try {
            var queued = false
            for (attempt in 0 until 4) {
                val idx = c.dequeueInputBuffer(if (attempt == 0) 10_000L else 30_000L)
                if (idx >= 0) {
                    val b = c.getInputBuffer(idx)!!
                    b.clear(); b.put(au)
                    c.queueInputBuffer(idx, 0, au.size, ptsUs, 0)
                    ptsUs += 21_333L   // 1024 samples at 48 kHz; only has to grow
                    queued = true
                    break
                }
                stats.inputBusy++
                pending += drain(c, 0L)   // an input buffer may be busy until some output has been taken
            }
            if (!queued) stats.inputDropped++
            pending += drain(c, XheSupport.outputWaitUs(stats.framesOut > 0, stats.inFlight()))
        } catch (t: Throwable) {
            AppLog.e("xhe", "xHE-AAC decode failed", t)
            stats.errors++
            restart()
        }
        val out = pending
        pending = ShortArray(0)
        if (out.isEmpty()) stats.empty++
        if (stats.framesIn % 500L == 0L) AppLog.i("xhe", stats.line())
        return out
    }

    /** A codec error (IllegalStateException, CodecException): start a fresh codec with the same configuration; the next frame is independent. */
    private fun restart() {
        val a = asc ?: return
        val r = rate; val ch = channels
        val why = open(a, if (r > 0) r else cfgRate, if (ch > 0) ch else cfgChannels)
        if (why.isNotEmpty()) AppLog.e("xhe", "xHE-AAC decoder restart failed: $why", null)
    }

    private fun drain(c: MediaCodec, firstWaitUs: Long): ShortArray {
        var all = ShortArray(0)
        var wait = firstWaitUs
        while (true) {
            val i = c.dequeueOutputBuffer(info, wait)
            wait = 0L
            when {
                i >= 0 -> {
                    val b = c.getOutputBuffer(i)
                    if (b != null && info.size > 0) {
                        b.position(info.offset); b.limit(info.offset + info.size)
                        all += XheSupport.toShorts(b, floatPcm)
                        stats.framesOut++
                    }
                    c.releaseOutputBuffer(i, false)
                }
                i == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    val f = c.outputFormat
                    if (f.containsKey(MediaFormat.KEY_SAMPLE_RATE)) rate = f.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                    if (f.containsKey(MediaFormat.KEY_CHANNEL_COUNT)) channels = f.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                    floatPcm = f.containsKey(MediaFormat.KEY_PCM_ENCODING) && f.getInteger(MediaFormat.KEY_PCM_ENCODING) == 4   // ENCODING_PCM_FLOAT
                }
                else -> return all   // INFO_TRY_AGAIN_LATER (and the deprecated buffers-changed)
            }
        }
    }

    /** Sample rate and channel count of the PCM that decode() returned (the decoder's own output format, which can differ from the SDC's). */
    @Synchronized fun outRate(): Int = rate
    @Synchronized fun outChannels(): Int = channels

    /** Frames were lost: drop what is inside; the next access unit is independent. */
    @Synchronized fun flush() {
        pending = ShortArray(0)
        stats.flushes++; stats.framesInAtFlush = stats.framesIn; stats.framesOutAtFlush = stats.framesOut
        try { codec?.flush() } catch (t: Throwable) { AppLog.e("xhe", "xHE-AAC flush failed", t) }
    }

    @Synchronized fun close() {
        val c = codec ?: return
        codec = null
        AppLog.i("xhe", stats.line() + " (closed)")
        try { c.stop() } catch (_: Throwable) {}
        try { c.release() } catch (_: Throwable) {}
    }

    companion object {
        /** Gives the core this phone's decoder; called once after the library is initialised. Safe to call again. */
        fun register(): Boolean = try {
            XheNative.loaded && XheNative.nativeXheRegister(XheAacDecoder())
        } catch (t: Throwable) { AppLog.e("xhe", "could not register the xHE-AAC decoder", t); false }
    }
}
