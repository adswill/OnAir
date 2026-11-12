package io.github.adswill.onair.tv

import android.media.AudioAttributes
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.SystemClock
import android.view.Surface
import io.github.adswill.onair.diag.AppLog
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import android.media.AudioFormat as PcmFormat

/** The phone's decoders, from MediaCodecList (hardware ones are listed first by Android). Cached: the list does not change while running. */
class AndroidDecoderCatalog : DecoderCatalog {
    private val cache = HashMap<String, String?>()
    override fun decoderFor(mime: String): String? = synchronized(cache) {
        cache.getOrPut(mime) {
            try {
                MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.firstOrNull { ci ->
                    !ci.isEncoder && ci.supportedTypes.any { it.equals(mime, ignoreCase = true) }
                }?.name
            } catch (e: Throwable) { AppLog.e("tv", "MediaCodecList failed", e); null }
        }
    }
}

/** What the TV screen shows under the picture; published about twice a second. */
data class TvPlaybackStats(
    val state: String = "",
    val videoCodec: String = "",
    val videoDecoder: String = "",
    val width: Int = 0,
    val height: Int = 0,
    val audioCodec: String = "",
    val audioDecoder: String = "",
    val audioRate: Int = 0,
    val audioChannels: Int = 0,
    val shown: Long = 0,
    val dropped: Long = 0,
    val audioFrames: Long = 0,
    val audioBufferMs: Long = 0,
    val ringDropped: Long = 0,
    val ccErrors: Long = 0,
    val notes: List<String> = emptyList(),
)

/**
 * Plays one service of the received multiplex with the phone's own decoders: the packets of its PIDs come from the native ring
 * (TvNative), [TsDemux] makes PES packets, [VideoFramer] / [AudioFramer] cut them into access units, MediaCodec decodes (the picture
 * straight onto the Surface), AudioTrack plays the sound, and [AvSync] shows each picture at its time against the sound clock.
 *
 * Everything runs on one thread ("onair-tv"), MediaCodec in its synchronous mode, so there are no callbacks to order. The public
 * calls only hand values over.
 */
class TvPlayer(private val catalog: DecoderCatalog, private val onStats: (TvPlaybackStats) -> Unit) {
    data class Selection(val service: TvService, val audio: TvStream?)

    @Volatile private var wanted: Selection? = null
    @Volatile private var running = false
    @Volatile var volume = 1f
    @Volatile var muted = false
    private var thread: Thread? = null

    private class SurfaceSwap(val surface: Surface?, val done: CountDownLatch = CountDownLatch(1))
    @Volatile private var swap: SurfaceSwap? = null
    @Volatile private var surfaceForStart: Surface? = null

    @Volatile private var lastSurface: Surface? = null

    fun start(sel: Selection, surface: Surface?) {
        wanted = sel
        if (running) { if (surface !== lastSurface) setSurface(surface); return }
        surfaceForStart = surface
        lastSurface = surface
        running = true
        thread = Thread({ runLoop() }, "onair-tv").apply { isDaemon = true; start() }
    }

    fun select(sel: Selection) { wanted = sel }

    /** A new Surface (or none: the view is going away). Returns once the old one is no longer used (at most a second). */
    fun setSurface(s: Surface?) {
        lastSurface = s
        if (!running) { surfaceForStart = s; return }
        val sw = SurfaceSwap(s)
        swap = sw
        sw.done.await(1, TimeUnit.SECONDS)
    }

    fun stop() {
        running = false
        thread?.let { if (it !== Thread.currentThread()) it.join(1500) }
        thread = null
    }

    // ---- the player thread ----------------------------------------------------------------------------------------------------

    private var surface: Surface? = null
    private var current: Selection? = null
    private val unwrap = PtsUnwrapper()
    private var demux = TsDemux { onPes(it) }
    private var videoPid = -1
    private var audioPid = -1
    private var videoFramer: VideoFramer? = null
    private var audioFramer: AudioFramer? = null
    private var audioFormatTried = false
    private val videoQueue = ArrayDeque<Pair<VideoAu, VideoConfig>>()
    private val audioQueue = ArrayDeque<Pair<AudioFrame, AudioConfig>>()

    private var vCodec: MediaCodec? = null
    private var vCfg: VideoConfig? = null
    private var vName = ""
    private var vFailures = 0
    private var held: Pair<Int, Long>? = null   // output buffer index, its PTS
    private var heldUntilMs = 0L
    private var width = 0
    private var height = 0

    private var aCodec: MediaCodec? = null
    private var aCfg: AudioConfig? = null
    private var aName = ""
    private var aFailures = 0
    private var track: AudioTrack? = null
    private var trackRate = 0
    private var trackChannels = 0
    private var pcm: ByteArray? = null
    private var pcmOff = 0
    private var pcmPtsUs = 0L
    private var pcmFrameBytes = 4

    private val mp2 = Mp2Decoder()
    private var softAudio = false
    private val audioClock = AudioClock()
    private val wallClock = WallClock()
    private val sync = AvSync()
    private var firstVideoOutMs = 0L
    private var audioFrames = 0L
    private val notes = LinkedHashSet<String>()
    private var lastStatsMs = 0L
    private var lastLogVolume = -1f

    private fun runLoop() {
        surface = surfaceForStart
        val buf = ByteArray(TS_PACKET * 700)
        try {
            while (running) {
                swap?.let { sw -> swap = null; releaseVideo(); surface = sw.surface; vFailures = 0; videoFramer?.reset(); sw.done.countDown() }
                val sel = wanted
                if (sel != null && sel !== current) apply(sel)
                val n = if (TvNative.loaded) TvNative.nativeTvRead(buf, if (videoQueue.isEmpty() && audioQueue.isEmpty() && held == null) 20 else 0) else -1
                if (n > 0) demux.feed(buf, n) else if (n < 0) Thread.sleep(20)
                pumpVideoIn(); pumpVideoOut(); pumpAudioIn(); pumpAudioOut(); writePcm()
                val now = SystemClock.uptimeMillis()
                if (now - lastStatsMs >= 500) { lastStatsMs = now; publish() }
            }
        } catch (e: InterruptedException) {
        } catch (e: Throwable) {
            AppLog.e("tv", "TV player stopped", e)
            notes += "The TV player stopped: ${e.message ?: e.javaClass.simpleName}"
            publish()
        } finally {
            releaseVideo(); releaseAudio()
            if (TvNative.loaded) try { TvNative.nativeTvSelect("") } catch (_: Throwable) {}
            swap?.done?.countDown()
            current = null
        }
    }

    private fun apply(sel: Selection) {
        current = sel
        releaseVideo(); releaseAudio()
        videoQueue.clear(); audioQueue.clear(); notes.clear()
        unwrap.reset(); audioClock.reset(); wallClock.reset(); sync.reset()
        firstVideoOutMs = 0; audioFrames = 0; vFailures = 0; aFailures = 0; width = 0; height = 0
        softAudio = false; mp2.reset()
        val v = sel.service.video
        videoPid = v?.takeIf { !it.scrambled }?.pid ?: -1
        audioPid = sel.audio?.takeIf { !it.scrambled }?.pid ?: -1
        videoFramer = v?.let { videoCodecOf(it) }?.let { VideoFramer(it) }
        if (videoFramer == null) videoPid = -1
        audioFramer = null; audioFormatTried = false
        notes += playabilityNotes(sel.service, sel.audio, catalog)
        demux = TsDemux { onPes(it) }
        demux.setPids(listOf(videoPid, audioPid).filter { it >= 0 })
        if (TvNative.loaded) TvNative.nativeTvSelect(pidsFor(sel.service, sel.audio).joinToString(","))
        AppLog.i("tv", "playing service ${sel.service.id} \"${sel.service.title}\": video pid $videoPid, audio pid $audioPid")
    }

    private fun onPes(p: Pes) {
        val pts = if (p.pts == NO_PTS) NO_PTS else ptsToUs(unwrap.unwrap(p.pts))
        val dts = if (p.dts == NO_PTS || p.dts == p.pts) pts else ptsToUs(unwrap.unwrap(p.dts))
        if (p.pid == videoPid) {
            val vf = videoFramer ?: return
            vf.push(p.data, pts, dts, p.damaged) { au, cfg ->
                if (videoQueue.size >= 120) { videoQueue.clear(); vf.reset() }   // the decoder does not keep up: start again at a key frame
                videoQueue.addLast(au to cfg)
            }
        } else if (p.pid == audioPid) {
            if (audioFramer == null && !audioFormatTried) {
                audioFormatTried = true
                val st = current?.audio
                val f = st?.let { audioFormatOf(it, p.data) }
                audioFramer = f?.let { AudioFramer.forFormat(it) }
            }
            audioFramer?.push(p.data, pts, p.damaged) { fr, cfg ->
                if (audioClock.bufferedUs > 1_500_000) return@push   // the sound runs far behind the stream (clock drift): catch up
                if (audioQueue.size >= 300) audioQueue.removeFirst()
                audioQueue.addLast(fr to cfg)
            }
        }
    }

    // ---- video ----

    private fun ensureVideo(cfg: VideoConfig): MediaCodec? {
        vCodec?.let { if (cfg.sameAs(vCfg)) return it }
        if (vFailures >= 3) return null
        releaseVideo()
        val s = surface ?: return null
        val name = catalog.decoderFor(cfg.codec.mime) ?: return null
        return try {
            val f = MediaFormat.createVideoFormat(cfg.codec.mime, cfg.width, cfg.height)
            cfg.csd.forEachIndexed { i, b -> f.setByteBuffer("csd-$i", ByteBuffer.wrap(b)) }
            f.setInteger(MediaFormat.KEY_MAX_WIDTH, maxOf(cfg.width, 1920))
            f.setInteger(MediaFormat.KEY_MAX_HEIGHT, maxOf(cfg.height, 1088))
            val c = MediaCodec.createByCodecName(name)
            c.configure(f, s, null, 0)
            c.start()
            vCodec = c; vCfg = cfg; vName = name
            width = cfg.width; height = cfg.height
            AppLog.i("tv", "video decoder $name for ${cfg.codec.label} ${cfg.width}x${cfg.height}")
            c
        } catch (e: Exception) {
            vFailures++
            AppLog.e("tv", "video decoder $name failed to start", e)
            notes += "The phone's ${cfg.codec.label} decoder ($name) did not start: ${e.message ?: e.javaClass.simpleName}"
            releaseVideo()
            null
        }
    }

    private fun pumpVideoIn() {
        while (videoQueue.isNotEmpty()) {
            val (au, cfg) = videoQueue.first()
            val c = ensureVideo(cfg)
            if (c == null) {   // no surface or no decoder: the pictures go nowhere
                if (surface == null || vFailures >= 3 || catalog.decoderFor(cfg.codec.mime) == null) videoQueue.clear()
                return
            }
            val idx = try { c.dequeueInputBuffer(0) } catch (e: Exception) { videoError(e); return }
            if (idx < 0) return
            videoQueue.removeFirst()
            try {
                val b = c.getInputBuffer(idx) ?: return
                b.clear()
                val n = minOf(au.data.size, b.remaining())
                b.put(au.data, 0, n)
                c.queueInputBuffer(idx, 0, n, if (au.ptsUs == NO_PTS) 0 else au.ptsUs, 0)
            } catch (e: Exception) { videoError(e); return }
        }
    }

    private val info = MediaCodec.BufferInfo()

    private fun videoClockUs(nowNs: Long): Long {
        val audioOn = track != null && audioClock.writtenFrames > 0
        if (audioOn) {
            audioClock.playedFrames = (track?.playbackHeadPosition ?: 0).toLong() and 0xFFFFFFFFL
            return audioClock.nowUs(nowNs)
        }
        // sound is expected but has not started: wait up to 1.5 s for it, then run on the wall clock
        val audioExpected = audioPid >= 0 && aFailures < 3 && audioFramer != null && (aCfg == null || aCodec != null || softAudio)
        if (audioExpected && SystemClock.uptimeMillis() - firstVideoOutMs < 1500) return WAIT_FOR_SOUND
        return wallClock.nowUs(nowNs)
    }

    private fun pumpVideoOut() {
        val c = vCodec ?: return
        repeat(4) {
            val nowNs = System.nanoTime()
            val h = held
            val idx: Int
            val pts: Long
            if (h != null) {
                if (SystemClock.uptimeMillis() < heldUntilMs) return
                idx = h.first; pts = h.second; held = null
            } else {
                val r = try { c.dequeueOutputBuffer(info, 0) } catch (e: Exception) { videoError(e); return }
                when {
                    r == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        val f = c.outputFormat
                        val w = f.getIntegerOr("crop-right", -1).let { if (it >= 0) it - f.getIntegerOr("crop-left", 0) + 1 else f.getIntegerOr(MediaFormat.KEY_WIDTH, width) }
                        val hh = f.getIntegerOr("crop-bottom", -1).let { if (it >= 0) it - f.getIntegerOr("crop-top", 0) + 1 else f.getIntegerOr(MediaFormat.KEY_HEIGHT, height) }
                        width = w; height = hh
                        return@repeat
                    }
                    r < 0 -> return
                }
                idx = r; pts = info.presentationTimeUs
                if (firstVideoOutMs == 0L) firstVideoOutMs = SystemClock.uptimeMillis()
            }
            var clock = videoClockUs(nowNs)
            if (clock == WAIT_FOR_SOUND) { held = idx to pts; heldUntilMs = SystemClock.uptimeMillis() + 20; return }   // waiting for the sound
            if (clock == NO_PTS) { wallClock.start(pts, nowNs); clock = pts }
            try {
                when (val a = sync.decide(pts, clock, nowNs)) {
                    is FrameAction.Show -> c.releaseOutputBuffer(idx, a.atWallNs)
                    FrameAction.Drop -> c.releaseOutputBuffer(idx, false)
                    is FrameAction.Hold -> { held = idx to pts; heldUntilMs = SystemClock.uptimeMillis() + a.retryMs; return }
                }
            } catch (e: Exception) { videoError(e); return }
        }
    }

    private fun videoError(e: Exception) {
        vFailures++
        AppLog.e("tv", "video decoder $vName failed", e)
        if (vFailures >= 3) notes += "The phone's video decoder ($vName) keeps failing on this stream; the picture is off. ${e.message ?: ""}".trim()
        releaseVideo()
        videoFramer?.reset()
        videoQueue.clear()
    }

    private fun releaseVideo() {
        held = null
        vCodec?.let { c -> try { c.stop() } catch (_: Exception) {}; try { c.release() } catch (_: Exception) {} }
        vCodec = null; vCfg = null
    }

    // ---- sound ----

    private fun ensureAudio(cfg: AudioConfig): MediaCodec? {
        aCodec?.let { if (cfg == aCfg) return it }
        if (aFailures >= 3) return null
        val name = catalog.decoderFor(cfg.mime)
        if (name == null) {
            notes += "This phone has no decoder for ${labelOfMime(cfg.mime)} sound, so there is no sound."
            aFailures = 3
            return null
        }
        releaseAudioCodec()
        return try {
            val f = MediaFormat.createAudioFormat(cfg.mime, cfg.sampleRate, cfg.channels.coerceAtLeast(1))
            cfg.csd?.let { f.setByteBuffer("csd-0", ByteBuffer.wrap(it)) }
            if (cfg.mime == "audio/mp4a-latm") f.setInteger(MediaFormat.KEY_IS_ADTS, 0)
            val c = MediaCodec.createByCodecName(name)
            c.configure(f, null, null, 0)
            c.start()
            aCodec = c; aCfg = cfg; aName = name
            AppLog.i("tv", "sound decoder $name for ${cfg.mime} ${cfg.sampleRate} Hz ${cfg.channels} ch")
            c
        } catch (e: Exception) {
            aFailures++
            AppLog.e("tv", "sound decoder $name failed to start", e)
            notes += "The phone's sound decoder ($name) did not start: ${e.message ?: e.javaClass.simpleName}"
            releaseAudioCodec()
            null
        }
    }

    private fun pumpAudioIn() {
        while (audioQueue.isNotEmpty()) {
            val (fr, cfg) = audioQueue.first()
            if (cfg.mime == MpegAudioFramer.MIME_MPEG_L2 && catalog.decoderFor(cfg.mime) == null) {   // MP2: the app's own decoder (Mp2Decoder)
                if (pcm != null) return   // the sink is full: wait
                audioQueue.removeFirst()
                val out = mp2.decode(fr.data) ?: continue
                if (track == null || trackRate != out.sampleRate || trackChannels != out.channels) openTrack(out.sampleRate, out.channels, PcmFormat.ENCODING_PCM_16BIT)
                val bytes = ByteArray(out.samples.size * 2)
                for (k in out.samples.indices) { val v = out.samples[k].toInt(); bytes[2 * k] = v.toByte(); bytes[2 * k + 1] = (v shr 8).toByte() }
                pcm = bytes; pcmOff = 0; pcmPtsUs = fr.ptsUs
                aCfg = cfg; aName = BUILTIN_MP2; softAudio = true
                audioFrames++
                writePcm()
                continue
            }
            val c = ensureAudio(cfg) ?: run { if (aFailures >= 3) audioQueue.clear(); return }
            val idx = try { c.dequeueInputBuffer(0) } catch (e: Exception) { audioError(e); return }
            if (idx < 0) return
            audioQueue.removeFirst()
            try {
                val b = c.getInputBuffer(idx) ?: return
                b.clear()
                val n = minOf(fr.data.size, b.remaining())
                b.put(fr.data, 0, n)
                c.queueInputBuffer(idx, 0, n, fr.ptsUs, 0)
                audioFrames++
            } catch (e: Exception) { audioError(e); return }
        }
    }

    private fun pumpAudioOut() {
        val c = aCodec ?: return
        repeat(8) {
            if (pcm != null) return   // the sink is full: let it drain first
            val r = try { c.dequeueOutputBuffer(info, 0) } catch (e: Exception) { audioError(e); return }
            when {
                r == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> { openTrack(c.outputFormat); return@repeat }
                r < 0 -> return
            }
            try {
                if (track == null) openTrack(c.outputFormat)
                val ob = c.getOutputBuffer(r)
                if (ob != null && info.size > 0) {
                    ob.position(info.offset); ob.limit(info.offset + info.size)
                    val bytes = ByteArray(info.size)
                    ob.get(bytes)
                    pcm = bytes; pcmOff = 0; pcmPtsUs = info.presentationTimeUs
                }
                c.releaseOutputBuffer(r, false)
            } catch (e: Exception) { audioError(e); return }
            writePcm()
        }
    }

    private fun openTrack(f: MediaFormat) =
        openTrack(f.getIntegerOr(MediaFormat.KEY_SAMPLE_RATE, 48000), f.getIntegerOr(MediaFormat.KEY_CHANNEL_COUNT, 2),
            f.getIntegerOr(MediaFormat.KEY_PCM_ENCODING, PcmFormat.ENCODING_PCM_16BIT))

    private fun openTrack(rate: Int, ch: Int, enc: Int) {
        if (track != null && rate == trackRate && ch == trackChannels) return
        releaseTrack()
        val mask = when (ch) {
            1 -> PcmFormat.CHANNEL_OUT_MONO
            2 -> PcmFormat.CHANNEL_OUT_STEREO
            4 -> PcmFormat.CHANNEL_OUT_QUAD
            6 -> PcmFormat.CHANNEL_OUT_5POINT1
            8 -> PcmFormat.CHANNEL_OUT_7POINT1_SURROUND
            else -> PcmFormat.CHANNEL_OUT_STEREO
        }
        val bytesPerSample = if (enc == PcmFormat.ENCODING_PCM_FLOAT) 4 else 2
        val min = AudioTrack.getMinBufferSize(rate, mask, enc).coerceAtLeast(4096)
        val t = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MOVIE).build())
            .setAudioFormat(PcmFormat.Builder().setSampleRate(rate).setChannelMask(mask).setEncoding(enc).build())
            .setBufferSizeInBytes(maxOf(min * 2, rate * ch * bytesPerSample / 4))   // about 250 ms
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        t.setVolume(if (muted) 0f else volume)
        t.play()
        track = t; trackRate = rate; trackChannels = ch
        pcmFrameBytes = ch * bytesPerSample
        audioClock.reset(); audioClock.sampleRate = rate
        AppLog.i("tv", "sound out $rate Hz, $ch channels")
    }

    private fun writePcm() {
        val t = track ?: return
        val v = if (muted) 0f else volume
        if (v != lastLogVolume) { t.setVolume(v); lastLogVolume = v }
        val b = pcm ?: return
        val n = try { t.write(b, pcmOff, b.size - pcmOff, AudioTrack.WRITE_NON_BLOCKING) } catch (e: Exception) { -1 }
        if (n < 0) { pcm = null; return }
        val frames = n / pcmFrameBytes
        if (frames > 0) {
            audioClock.wrote(frames, pcmPtsUs)
            pcmPtsUs += frames * 1_000_000L / trackRate.coerceAtLeast(1)
        }
        pcmOff += frames * pcmFrameBytes
        if (pcmOff >= b.size - pcmFrameBytes + 1) pcm = null
    }

    private fun audioError(e: Exception) {
        aFailures++
        AppLog.e("tv", "sound decoder $aName failed", e)
        if (aFailures >= 3) notes += "The phone's sound decoder ($aName) keeps failing on this stream; the sound is off."
        releaseAudioCodec()
        audioFramer?.reset()
        audioQueue.clear()
    }

    private fun releaseAudioCodec() {
        aCodec?.let { c -> try { c.stop() } catch (_: Exception) {}; try { c.release() } catch (_: Exception) {} }
        aCodec = null; aCfg = null
    }

    private fun releaseTrack() {
        track?.let { t -> try { t.pause(); t.flush(); t.release() } catch (_: Exception) {} }
        track = null; pcm = null; trackRate = 0; trackChannels = 0; lastLogVolume = -1f
    }

    private fun releaseAudio() { releaseAudioCodec(); releaseTrack() }

    private fun publish() {
        val stats = if (TvNative.loaded) TvNative.nativeTvStats().split('\t') else emptyList()
        val sel = current
        val state = when {
            sel == null -> "Choose a service"
            vCodec != null && sync.shown > 0 -> "Playing"
            track != null && audioClock.writtenFrames > 0 -> if (videoPid >= 0) "Sound playing, waiting for a picture" else "Playing (radio)"
            demux.packets == 0L -> "Waiting for the stream"
            else -> "Starting the decoders"
        }
        val av = if (track != null && audioClock.writtenFrames > 0) {
            audioClock.playedFrames = (track?.playbackHeadPosition ?: 0).toLong() and 0xFFFFFFFFL
            audioClock.bufferedUs / 1000
        } else 0
        onStats(
            TvPlaybackStats(
                state = state,
                videoCodec = videoFramer?.codec?.label ?: "",
                videoDecoder = if (vCodec != null) vName else "",
                width = width, height = height,
                audioCodec = aCfg?.let { labelOfMime(it.mime) } ?: "",
                audioDecoder = if (aCodec != null || softAudio) aName else "",
                audioRate = trackRate, audioChannels = trackChannels,
                shown = sync.shown, dropped = sync.dropped + (videoFramer?.dropped ?: 0),
                audioFrames = audioFrames, audioBufferMs = av,
                ringDropped = stats.getOrNull(2)?.toLongOrNull() ?: 0,
                ccErrors = demux.ccErrors(),
                notes = notes.toList(),
            ),
        )
    }

    private fun MediaFormat.getIntegerOr(key: String, def: Int): Int = try { if (containsKey(key)) getInteger(key) else def } catch (_: Exception) { def }

    companion object {
        private const val WAIT_FOR_SOUND = Long.MAX_VALUE

        fun labelOfMime(m: String): String = when (m) {
            "audio/mp4a-latm" -> "AAC"
            "audio/ac3" -> "AC-3 (Dolby Digital)"
            "audio/eac3" -> "E-AC-3 (Dolby Digital Plus)"
            MpegAudioFramer.MIME_MPEG_L2 -> "MPEG audio layer II (MP2)"
            MpegAudioFramer.MIME_MPEG_L1 -> "MPEG audio layer I"
            MpegAudioFramer.MIME_MPEG_L3 -> "MP3"
            else -> m
        }
    }
}
