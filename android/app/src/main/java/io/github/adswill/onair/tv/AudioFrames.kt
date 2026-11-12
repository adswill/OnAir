package io.github.adswill.onair.tv

/*
 * Audio elementary streams cut into the frames a decoder takes one at a time, each with its own presentation time: AAC in ADTS and in
 * LATM/LOAS (to raw AAC plus its AudioSpecificConfig, which every Android AAC decoder accepts), AC-3 and E-AC-3 (sync frames;
 * dependent E-AC-3 substreams stay with their independent frame), MPEG-1/2 audio layers I-III. Pure Kotlin (AudioFramesTest).
 *
 * Only the frame headers are read here: no audio is decoded. The decoders are the phone's own (MediaCodec).
 */

/** One frame for the decoder. [ptsUs] on the unwrapped program clock; [samples] per channel. */
class AudioFrame(val data: ByteArray, val ptsUs: Long, val sampleRate: Int, val channels: Int, val samples: Int) {
    val durationUs: Long get() = if (sampleRate > 0) samples * 1_000_000L / sampleRate else 0
}

/** What the decoder must be configured with; a change (another sample rate, an HE-AAC config) means a new decoder. */
data class AudioConfig(val mime: String, val sampleRate: Int, val channels: Int, val csd: ByteArray? = null) {
    override fun equals(other: Any?): Boolean =
        other is AudioConfig && other.mime == mime && other.sampleRate == sampleRate && other.channels == channels &&
            (other.csd?.contentEquals(csd ?: ByteArray(0)) ?: (csd == null))
    override fun hashCode(): Int = mime.hashCode() * 31 + sampleRate * 7 + channels + (csd?.contentHashCode() ?: 0)
}

enum class AudioFormat(val label: String) { AAC_ADTS("AAC"), AAC_LATM("AAC (LATM)"), AC3("AC-3"), EAC3("E-AC-3"), MPEG_AUDIO("MPEG audio") }

/** Cuts one audio stream into frames. Feed it PES packets in order; frames come out through [out] with their configuration. */
abstract class AudioFramer {
    protected var acc = ByteArray(16 * 1024)
    protected var accLen = 0
    /** PES starts inside [acc] (offset, PTS): the first frame that begins in a PES takes its PTS. */
    private val anchors = ArrayDeque<LongArray>()
    private var nextPtsUs = NO_PTS
    var config: AudioConfig? = null; protected set
    var frames = 0L; private set
    var skippedBytes = 0L; protected set

    /** [ptsUs]: the PES's unwrapped presentation time in microseconds, or [NO_PTS]. */
    fun push(data: ByteArray, ptsUs: Long, damaged: Boolean, out: (AudioFrame, AudioConfig) -> Unit) {
        if (damaged) { accLen = 0; anchors.clear(); nextPtsUs = NO_PTS }   // frames across a gap are broken: start at the next sync
        if (accLen + data.size > acc.size) acc = acc.copyOf(maxOf(acc.size * 2, accLen + data.size))
        if (ptsUs != NO_PTS) { anchors.addLast(longArrayOf(accLen.toLong(), ptsUs)); if (anchors.size > 64) anchors.removeFirst() }
        System.arraycopy(data, 0, acc, accLen, data.size)
        accLen += data.size
        var i = 0
        while (true) {
            val r = scan(i) ?: break   // null: needs more data
            if (r.skip > 0) { skippedBytes += r.skip; i += r.skip; continue }
            val frameStart = i
            i += r.consumed
            // the first frame that starts in a PES takes its PTS (13818-1 2.4.3.7); later ones follow on by the frame duration
            var anchor: LongArray? = null
            while (anchors.isNotEmpty() && anchors.first()[0] <= frameStart) anchor = anchors.removeFirst()
            val fr = r.frame
            if (fr == null) {   // nothing usable (LATM before its config): the next frame takes this PTS, a frame early at worst
                if (anchor != null) { anchor[0] = i.toLong(); anchors.addFirst(anchor) }
                continue
            }
            val pts = anchor?.get(1) ?: nextPtsUs
            if (pts == NO_PTS) continue   // no clock yet: wait for a PES with a PTS
            val f = AudioFrame(fr.data, pts, fr.rate, fr.channels, fr.samples)
            nextPtsUs = pts + f.durationUs
            config = fr.config
            frames++
            out(f, fr.config)
        }
        // keep the unread tail
        if (i > 0) {
            System.arraycopy(acc, i, acc, 0, accLen - i)
            accLen -= i
            for (a in anchors) a[0] = (a[0] - i).coerceAtLeast(0)
        }
        if (accLen > 1 shl 20) { skippedBytes += accLen; accLen = 0 }   // garbage that never synced
    }

    fun reset() { accLen = 0; anchors.clear(); nextPtsUs = NO_PTS }

    protected class Cut(val frame: ParsedFrame?, val consumed: Int, val skip: Int = 0)
    protected class ParsedFrame(val data: ByteArray, val rate: Int, val channels: Int, val samples: Int, val config: AudioConfig)

    /** Looks at [acc] from [at]: a frame (consumed bytes), bytes to skip to the next possible sync, or null for "need more data". */
    protected abstract fun scan(at: Int): Cut?

    protected fun u8(i: Int): Int = acc[i].toInt() and 0xFF

    companion object {
        fun forFormat(f: AudioFormat): AudioFramer = when (f) {
            AudioFormat.AAC_ADTS -> AdtsFramer()
            AudioFormat.AAC_LATM -> LatmFramer()
            AudioFormat.AC3, AudioFormat.EAC3 -> Ac3Framer()
            AudioFormat.MPEG_AUDIO -> MpegAudioFramer()
        }
        val aacRates = intArrayOf(96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350)
    }
}

/** MSB-first bit reader over a byte array. */
class BitReader(private val b: ByteArray, private var pos: Long, private val endBit: Long = b.size * 8L) {
    val bitPos: Long get() = pos
    fun left(): Long = endBit - pos
    fun bits(n: Int): Int {
        require(n in 0..32)
        if (pos + n > endBit) throw IndexOutOfBoundsException("bit reader ran past the end")
        var v = 0L
        for (k in 0 until n) {
            val byte = b[(pos shr 3).toInt()].toInt()
            v = (v shl 1) or ((byte shr (7 - (pos and 7).toInt())) and 1).toLong()
            pos++
        }
        return v.toInt()
    }
    fun bit(): Boolean = bits(1) == 1
    fun skip(n: Long) { if (pos + n > endBit) throw IndexOutOfBoundsException("skip past the end"); pos += n }
    /** Copies [nBits] bits from here into a new array, left-aligned (the last byte padded with zeros). */
    fun copyBits(nBits: Long): ByteArray {
        val out = ByteArray(((nBits + 7) / 8).toInt())
        for (k in 0 until nBits) {
            if (bit()) out[(k shr 3).toInt()] = (out[(k shr 3).toInt()].toInt() or (0x80 ushr (k and 7).toInt())).toByte()
        }
        return out
    }
    /** Unsigned Exp-Golomb (H.264 / HEVC parameter sets). */
    fun ue(): Int {
        var z = 0
        while (!bit()) { z++; if (z > 31) throw IllegalStateException("bad exp-golomb") }
        return if (z == 0) 0 else ((1 shl z) - 1 + bits(z))
    }
    fun se(): Int { val k = ue(); return if (k and 1 == 1) (k + 1) / 2 else -(k / 2) }
}

/** The 2-byte AudioSpecificConfig of a plain AAC stream (ISO/IEC 14496-3 1.6.2.1): object type, rate index, channel configuration. */
fun aacConfigBytes(objectType: Int, rateIndex: Int, channelConfig: Int): ByteArray {
    val v = (objectType shl 11) or (rateIndex shl 7) or (channelConfig shl 3)
    return byteArrayOf((v shr 8).toByte(), v.toByte())
}

/** ADTS (ISO/IEC 13818-7 / 14496-3 1.A.2): the header is removed, the raw frame goes to the decoder with the matching config. */
class AdtsFramer : AudioFramer() {
    override fun scan(at: Int): Cut? {
        if (accLen - at < 9) return null
        if (u8(at) != 0xFF || (u8(at + 1) and 0xF6) != 0xF0) return Cut(null, 0, 1)   // 12 sync bits and layer 00
        val protAbsent = u8(at + 1) and 1 == 1
        val profile = u8(at + 2) shr 6
        val sfi = (u8(at + 2) shr 2) and 15
        val ch = ((u8(at + 2) and 1) shl 2) or (u8(at + 3) shr 6)
        val len = ((u8(at + 3) and 3) shl 11) or (u8(at + 4) shl 3) or (u8(at + 5) shr 5)
        val blocks = (u8(at + 6) and 3) + 1
        val hl = if (protAbsent) 7 else 9
        if (sfi >= 13 || len < hl + 1) return Cut(null, 0, 1)
        if (accLen - at < len) return null
        // a second header right behind this frame confirms the sync (when the data is there; else trust this one)
        if (accLen - at >= len + 2 && (u8(at + len) != 0xFF || (u8(at + len + 1) and 0xF6) != 0xF0)) return Cut(null, 0, 1)
        val chOut = if (ch == 7) 8 else if (ch == 0) 2 else ch   // 0: set by a program_config_element in the stream; the decoder reports the real count
        val cfg = AudioConfig("audio/mp4a-latm", aacRates[sfi], chOut, aacConfigBytes(profile + 1, sfi, ch))
        val raw = acc.copyOfRange(at + hl, at + len)
        return Cut(ParsedFrame(raw, aacRates[sfi], chOut, 1024 * blocks, cfg), len)
    }
}

/**
 * LATM in LOAS (ISO/IEC 14496-3 1.7.2-1.7.3, AudioSyncStream with muxConfigPresent = 1), the form DVB uses for HE-AAC. One program,
 * one layer (all broadcast streams); the payload is copied out bit by bit (it need not start on a byte) and the AudioSpecificConfig
 * from the StreamMuxConfig becomes the decoder's config.
 */
class LatmFramer : AudioFramer() {
    private var asc: ByteArray? = null
    private var rate = 0
    private var channels = 0
    private var frameLengthType = 0
    private var aot = 0

    override fun scan(at: Int): Cut? {
        if (accLen - at < 3) return null
        if (u8(at) != 0x56 || (u8(at + 1) and 0xE0) != 0xE0) return Cut(null, 0, 1)   // 0x2B7, 11 bits
        val muxLen = ((u8(at + 1) and 0x1F) shl 8) or u8(at + 2)
        val total = 3 + muxLen
        if (accLen - at < total) return null
        if (accLen - at >= total + 2 && (u8(at + total) != 0x56 || (u8(at + total + 1) and 0xE0) != 0xE0)) return Cut(null, 0, 1)
        val payload = try { audioMuxElement(at + 3, muxLen) } catch (e: RuntimeException) { null }
        val a = asc
        if (payload == null || a == null || rate == 0) return Cut(null, total)   // nothing usable yet (no config seen): drop this element
        val samples = if (aot == 5 || aot == 29) 2048 else 1024
        val outRate = if (aot == 5 || aot == 29) rate * 2 else rate
        return Cut(ParsedFrame(payload, outRate, channels, samples, AudioConfig("audio/mp4a-latm", rate, channels, a)), total)
    }

    private fun audioMuxElement(from: Int, n: Int): ByteArray? {
        val br = BitReader(acc, from * 8L, (from + n) * 8L)
        val useSame = br.bit()
        if (!useSame) streamMuxConfig(br)
        if (asc == null) return null
        if (frameLengthType != 0) return null   // only the variable frame length (type 0) is used in broadcasting
        var len = 0
        while (true) { val t = br.bits(8); len += t; if (t != 255) break }
        if (len * 8L > br.left()) return null
        return br.copyBits(len * 8L)
    }

    private fun latmValue(br: BitReader): Int {
        val bytes = br.bits(2)
        var v = 0
        for (k in 0..bytes) v = (v shl 8) or br.bits(8)
        return v
    }

    private fun streamMuxConfig(br: BitReader) {
        val version = br.bits(1)
        val versionA = if (version == 1) br.bits(1) else 0
        if (versionA != 0) { asc = null; return }
        if (version == 1) latmValue(br)   // taraBufferFullness
        br.bits(1)   // allStreamsSameTimeFraming
        br.bits(6)   // numSubFrames
        val numProgram = br.bits(4)
        val numLayer = br.bits(3)
        if (numProgram != 0 || numLayer != 0) { asc = null; return }   // more than one program or layer: not in broadcast use
        if (version == 0) {
            val start = br.bitPos
            parseAsc(br)
            val ascBits = br.bitPos - start
            asc = BitReader(acc, start, br.bitPos).copyBits(ascBits)
        } else {
            val ascLen = latmValue(br)
            val start = br.bitPos
            parseAsc(br)
            val used = br.bitPos - start
            asc = BitReader(acc, start, start + used).copyBits(used)
            br.skip(ascLen - used)   // fill bits
        }
        frameLengthType = br.bits(3)
        when (frameLengthType) {
            0 -> br.bits(8)   // latmBufferFullness
            1 -> br.bits(9)
            3, 4, 5 -> br.bits(6)
            6, 7 -> br.bits(1)
        }
        if (br.bit()) {   // otherDataPresent
            if (version == 1) latmValue(br) else { while (true) { val esc = br.bit(); br.bits(8); if (!esc) break } }
        }
        if (br.bit()) br.bits(8)   // crcCheckPresent, crcCheckSum
    }

    /** AudioSpecificConfig up to the end of GASpecificConfig (as decoders read it from LATM version 0: no sync extension). */
    private fun parseAsc(br: BitReader) {
        fun objType(): Int { var t = br.bits(5); if (t == 31) t = 32 + br.bits(6); return t }
        fun rateOf(): Int { val i = br.bits(4); return if (i == 15) br.bits(24) else AudioFramer.aacRates.getOrElse(i) { 0 } }
        aot = objType()
        rate = rateOf()
        val chCfg = br.bits(4)
        channels = if (chCfg == 7) 8 else chCfg
        var core = aot
        if (aot == 5 || aot == 29) {   // explicit SBR / PS: the extension rate, then the core object type
            rateOf()
            core = objType()
        }
        if (core in intArrayOf(1, 2, 3, 4, 6, 7, 17, 19, 20, 21, 22, 23)) {   // GASpecificConfig
            br.bits(1)   // frameLengthFlag
            if (br.bit()) br.bits(14)   // dependsOnCoreCoder: coreCoderDelay
            val ext = br.bit()
            if (chCfg == 0) throw IllegalStateException("program_config_element not handled")
            if (core == 6 || core == 20) br.bits(3)
            if (ext) {
                if (core == 22) { br.bits(5); br.bits(11) }
                if (core == 17 || core == 19 || core == 20 || core == 23) br.bits(3)
                br.bits(1)
            }
        }
        if (channels == 0) channels = 2
    }
}

/** AC-3 (ATSC A/52 5.4.1) and E-AC-3 (A/52 annex E). E-AC-3 dependent substreams are appended to their independent frame. */
class Ac3Framer : AudioFramer() {
    override fun scan(at: Int): Cut? {
        val f = frameAt(at) ?: return if (accLen - at < 8) null else Cut(null, 0, 1)
        val (len, frame, _) = f
        if (accLen - at < len) return null
        if (accLen - at >= len + 2 && (u8(at + len) != 0x0B || u8(at + len + 1) != 0x77)) return Cut(null, 0, 1)
        var total = len
        if (frame.config.mime == "audio/eac3") {   // take the dependent substreams that follow (strmtyp 1): one access unit for the decoder
            while (true) {
                if (accLen - (at + total) < 8) return null   // the next header decides: wait for it
                val d = frameAt(at + total) ?: break
                if (!d.third) break
                if (accLen - (at + total) < d.first) return null
                total += d.first
            }
        }
        return Cut(ParsedFrame(acc.copyOfRange(at, at + total), frame.rate, frame.channels, frame.samples, frame.config), total)
    }

    /** (length, header values, dependent) of the sync frame at [at], or null when there is none. Needs 8 bytes. */
    private fun frameAt(at: Int): Triple<Int, ParsedFrame, Boolean>? {
        if (accLen - at < 8) return null
        if (u8(at) != 0x0B || u8(at + 1) != 0x77) return null
        val bsid = u8(at + 5) shr 3
        return if (bsid <= 10) ac3(at) else if (bsid in 11..16) eac3(at) else null
    }

    private fun channelsOf(acmod: Int, lfe: Boolean): Int = intArrayOf(2, 1, 2, 3, 3, 4, 4, 5)[acmod] + if (lfe) 1 else 0

    private fun ac3(at: Int): Triple<Int, ParsedFrame, Boolean>? {
        val fscod = u8(at + 4) shr 6
        val frmsizecod = u8(at + 4) and 0x3F
        if (fscod == 3 || frmsizecod > 37) return null
        val kbps = intArrayOf(32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640)[frmsizecod / 2]
        val words = when (fscod) {
            0 -> kbps * 2
            1 -> kbps * 1_000 * 1536 / 44100 / 16 + (frmsizecod and 1)
            else -> kbps * 3
        }
        val rate = intArrayOf(48000, 44100, 32000)[fscod]
        val br = BitReader(acc, (at + 5) * 8L, (at + 8) * 8L)
        br.bits(5); br.bits(3)   // bsid, bsmod
        val acmod = br.bits(3)
        if (acmod and 1 != 0 && acmod != 1) br.bits(2)
        if (acmod and 4 != 0) br.bits(2)
        if (acmod == 2) br.bits(2)
        val lfe = br.bit()
        val ch = channelsOf(acmod, lfe)
        return Triple(words * 2, ParsedFrame(ByteArray(0), rate, ch, 1536, AudioConfig("audio/ac3", rate, ch)), false)
    }

    private fun eac3(at: Int): Triple<Int, ParsedFrame, Boolean>? {
        val br = BitReader(acc, (at + 2) * 8L, (at + 8) * 8L)
        val strmtyp = br.bits(2)
        br.bits(3)   // substreamid
        val frmsiz = br.bits(11)
        val fscod = br.bits(2)
        val rate: Int
        val blocks: Int
        if (fscod == 3) {
            val fscod2 = br.bits(2)
            if (fscod2 == 3) return null
            rate = intArrayOf(24000, 22050, 16000)[fscod2]
            blocks = 6
        } else {
            rate = intArrayOf(48000, 44100, 32000)[fscod]
            blocks = intArrayOf(1, 2, 3, 6)[br.bits(2)]
        }
        val acmod = br.bits(3)
        val lfe = br.bit()
        val ch = channelsOf(acmod, lfe)
        return Triple((frmsiz + 1) * 2, ParsedFrame(ByteArray(0), rate, ch, 256 * blocks, AudioConfig("audio/eac3", rate, ch)), strmtyp == 1)
    }
}

/** MPEG-1 / MPEG-2 / MPEG-2.5 audio, layers I, II and III (ISO/IEC 11172-3 2.4.2.3, 13818-3). */
class MpegAudioFramer : AudioFramer() {
    override fun scan(at: Int): Cut? {
        if (accLen - at < 4) return null
        val h = mpegHeader(acc, at) ?: return Cut(null, 0, 1)
        if (accLen - at < h.length) return null
        if (accLen - at >= h.length + 4) {
            val n = mpegHeader(acc, at + h.length)
            if (n == null || n.layer != h.layer || n.rate != h.rate) return Cut(null, 0, 1)
        }
        val mime = when (h.layer) { 1 -> MIME_MPEG_L1; 2 -> MIME_MPEG_L2; else -> MIME_MPEG_L3 }
        val data = acc.copyOfRange(at, at + h.length)
        return Cut(ParsedFrame(data, h.rate, h.channels, h.samples, AudioConfig(mime, h.rate, h.channels)), h.length)
    }

    class Header(val layer: Int, val rate: Int, val channels: Int, val length: Int, val samples: Int, val kbps: Int)

    companion object {
        const val MIME_MPEG_L1 = "audio/mpeg-L1"
        const val MIME_MPEG_L2 = "audio/mpeg-L2"
        const val MIME_MPEG_L3 = "audio/mpeg"
        private val v1 = arrayOf(
            intArrayOf(0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448),
            intArrayOf(0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384),
            intArrayOf(0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320),
        )
        private val v2l1 = intArrayOf(0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256)
        private val v2l23 = intArrayOf(0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160)

        /** The header at [o], or null when it is not one (free format, bad index, reserved values). */
        fun mpegHeader(b: ByteArray, o: Int): Header? {
            val h = ((b[o].toInt() and 0xFF) shl 24) or ((b[o + 1].toInt() and 0xFF) shl 16) or ((b[o + 2].toInt() and 0xFF) shl 8) or (b[o + 3].toInt() and 0xFF)
            if (h ushr 21 != 0x7FF) return null
            val ver = (h shr 19) and 3          // 0 MPEG-2.5, 2 MPEG-2, 3 MPEG-1
            val layer = 4 - ((h shr 17) and 3)  // 1..3 (4 = reserved)
            val bri = (h shr 12) and 15
            val sri = (h shr 10) and 3
            val pad = (h shr 9) and 1
            val mode = (h shr 6) and 3
            if (ver == 1 || layer == 4 || bri == 0 || bri == 15 || sri == 3) return null
            val kbps = if (ver == 3) v1[layer - 1][bri] else if (layer == 1) v2l1[bri] else v2l23[bri]
            val rate = intArrayOf(44100, 48000, 32000)[sri] shr (if (ver == 3) 0 else if (ver == 2) 1 else 2)
            val samples = if (layer == 1) 384 else if (layer == 3 && ver != 3) 576 else 1152
            val len = if (layer == 1) (12 * kbps * 1000 / rate + pad) * 4 else samples / 8 * kbps * 1000 / rate + pad
            return Header(layer, rate, if (mode == 3) 1 else 2, len, samples, kbps)
        }
    }
}
