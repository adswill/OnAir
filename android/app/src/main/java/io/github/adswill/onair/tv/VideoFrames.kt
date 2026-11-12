package io.github.adswill.onair.tv

/*
 * Video access units for the phone's decoder: one PES of a broadcast video stream is one picture (ETSI TS 101 154 asks multiplexers for
 * that). Here they are only inspected: the parameter sets (H.264 SPS/PPS, HEVC VPS/SPS/PPS, the MPEG-2 sequence header) become the
 * decoder's configuration, key frames are found so decoding starts on a picture it can show, and the picture size is read for the
 * format. Pure Kotlin (VideoFramesTest). Nothing is decoded here.
 */

enum class VideoCodec(val mime: String, val label: String) {
    H264("video/avc", "H.264"), HEVC("video/hevc", "HEVC (H.265)"), MPEG2("video/mpeg2", "MPEG-2"), MPEG1("video/mpeg2", "MPEG-1")
}

/** What the decoder is configured with: [csd] go into csd-0 / csd-1 (each with Annex B start codes). */
class VideoConfig(val codec: VideoCodec, val width: Int, val height: Int, val csd: List<ByteArray>) {
    fun sameAs(o: VideoConfig?): Boolean =
        o != null && o.codec == codec && o.width == width && o.height == height && o.csd.size == csd.size && o.csd.indices.all { o.csd[it].contentEquals(csd[it]) }
}

/** One picture: Annex B bytes (start codes kept) as they came in the PES. */
class VideoAu(val data: ByteArray, val ptsUs: Long, val dtsUs: Long, val key: Boolean)

/** Finds the NAL units (start code 00 00 01 or 00 00 00 01) in Annex B data: (offset of the first byte after the start code, end). */
fun nalUnits(b: ByteArray, from: Int = 0, to: Int = b.size): List<IntRange> {
    val starts = ArrayList<Int>()
    var i = from
    while (i + 3 <= to) {
        if (b[i].toInt() == 0 && b[i + 1].toInt() == 0 && b[i + 2].toInt() == 1) { starts += i + 3; i += 3 } else i++
    }
    val out = ArrayList<IntRange>(starts.size)
    for (k in starts.indices) {
        var end = if (k + 1 < starts.size) starts[k + 1] - 3 else to
        while (end > starts[k] && b[end - 1].toInt() == 0) end--   // a 4-byte start code or trailing zeros belong to no NAL unit
        out += starts[k] until end
    }
    return out
}

/** The RBSP of a NAL unit: emulation-prevention bytes (00 00 03) removed. */
fun rbsp(b: ByteArray, r: IntRange): ByteArray {
    val out = java.io.ByteArrayOutputStream(r.last - r.first + 1)
    var zeros = 0
    for (i in r) {
        val v = b[i].toInt() and 0xFF
        if (zeros >= 2 && v == 3) { zeros = 0; continue }
        out.write(v)
        zeros = if (v == 0) zeros + 1 else 0
    }
    return out.toByteArray()
}

private val startCode = byteArrayOf(0, 0, 0, 1)
private fun withStartCode(b: ByteArray, r: IntRange) = startCode + b.copyOfRange(r.first, r.last + 1)

/** Picture size from an H.264 sequence parameter set (ITU-T H.264 7.3.2.1.1), cropping applied; null when it cannot be read. */
fun h264SpsSize(nal: ByteArray): Pair<Int, Int>? = try {
    val br = BitReader(nal, 8)   // after the NAL header byte
    val profile = br.bits(8)
    br.bits(8); br.bits(8)   // constraint flags, level
    br.ue()   // seq_parameter_set_id
    var chroma = 1
    if (profile in intArrayOf(100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135)) {
        chroma = br.ue()
        if (chroma == 3) br.bits(1)
        br.ue(); br.ue(); br.bits(1)   // bit depths, qpprime_y_zero_transform_bypass_flag
        if (br.bit()) {   // seq_scaling_matrix_present_flag
            val lists = if (chroma != 3) 8 else 12
            for (k in 0 until lists) if (br.bit()) {
                val size = if (k < 6) 16 else 64
                var last = 8; var next = 8
                for (j in 0 until size) { if (next != 0) { next = (last + br.se() + 256) % 256 }; last = if (next == 0) last else next }
            }
        }
    }
    br.ue()   // log2_max_frame_num_minus4
    val pocType = br.ue()
    if (pocType == 0) br.ue() else if (pocType == 1) {
        br.bits(1); br.se(); br.se()
        val n = br.ue(); for (k in 0 until n) br.se()
    }
    br.ue(); br.bits(1)   // max_num_ref_frames, gaps
    val wMbs = br.ue() + 1
    val hMapUnits = br.ue() + 1
    val frameMbsOnly = br.bit()
    if (!frameMbsOnly) br.bits(1)
    br.bits(1)   // direct_8x8_inference_flag
    var w = wMbs * 16
    var h = hMapUnits * 16 * (if (frameMbsOnly) 1 else 2)
    if (br.bit()) {   // frame_cropping_flag
        val l = br.ue(); val r = br.ue(); val t = br.ue(); val bo = br.ue()
        val cx = if (chroma == 1 || chroma == 2) 2 else 1
        val cy = (if (chroma == 1) 2 else 1) * (if (frameMbsOnly) 1 else 2)
        w -= (l + r) * cx; h -= (t + bo) * cy
    }
    if (w in 16..8192 && h in 16..8192) w to h else null
} catch (e: RuntimeException) { null }

/** Picture size from an HEVC sequence parameter set (ITU-T H.265 7.3.2.2.1), conformance window applied. */
fun hevcSpsSize(nal: ByteArray): Pair<Int, Int>? = try {
    val br = BitReader(nal, 16)   // two-byte NAL header
    br.bits(4)   // sps_video_parameter_set_id
    val maxSubLayers = br.bits(3)   // sps_max_sub_layers_minus1
    br.bits(1)
    // profile_tier_level(1, maxSubLayers)
    br.skip(2 + 1 + 5 + 32 + 4 + 43 + 1 + 8)
    val subProfile = BooleanArray(maxSubLayers); val subLevel = BooleanArray(maxSubLayers)
    for (k in 0 until maxSubLayers) { subProfile[k] = br.bit(); subLevel[k] = br.bit() }
    if (maxSubLayers > 0) for (k in maxSubLayers until 8) br.bits(2)
    for (k in 0 until maxSubLayers) { if (subProfile[k]) br.skip(88); if (subLevel[k]) br.skip(8) }
    br.ue()   // sps_seq_parameter_set_id
    val chroma = br.ue()
    if (chroma == 3) br.bits(1)
    var w = br.ue(); var h = br.ue()
    if (br.bit()) {   // conformance_window_flag
        val l = br.ue(); val r = br.ue(); val t = br.ue(); val bo = br.ue()
        val cx = if (chroma == 1 || chroma == 2) 2 else 1
        val cy = if (chroma == 1) 2 else 1
        w -= (l + r) * cx; h -= (t + bo) * cy
    }
    if (w in 16..8192 && h in 16..8192) w to h else null
} catch (e: RuntimeException) { null }

/**
 * Turns video PES packets into access units and keeps the configuration the decoder needs. Decoding starts at the first key frame
 * after a complete set of parameter sets; until then [push] gives nothing (a decoder fed a P-frame first shows garbage or fails).
 */
class VideoFramer(val codec: VideoCodec) {
    var config: VideoConfig? = null; private set
    private var sps: ByteArray? = null
    private var pps: ByteArray? = null
    private var vps: ByteArray? = null
    private var seqHeader: ByteArray? = null
    private var started = false
    var dropped = 0L; private set
    var aus = 0L; private set

    fun reset() { started = false }

    /** [ptsUs] / [dtsUs] unwrapped, [NO_PTS] when the PES had none (then the decoder's order is used). */
    fun push(pes: ByteArray, ptsUs: Long, dtsUs: Long, damaged: Boolean, out: (VideoAu, VideoConfig) -> Unit) {
        if (pes.isEmpty()) return
        if (damaged) { started = false; dropped++; return }   // a broken picture would smear until the next key frame: wait for one
        val key = when (codec) {
            VideoCodec.H264 -> scanH264(pes)
            VideoCodec.HEVC -> scanHevc(pes)
            VideoCodec.MPEG2, VideoCodec.MPEG1 -> scanMpeg2(pes)
        }
        val cfg = currentConfig() ?: run { dropped++; return }
        if (!started) { if (!key) { dropped++; return }; started = true }
        config = cfg
        aus++
        out(VideoAu(pes, ptsUs, dtsUs, key), cfg)
    }

    private fun currentConfig(): VideoConfig? {
        val c = config
        return when (codec) {
            VideoCodec.H264 -> {
                val s = sps ?: return null; val p = pps ?: return null
                val (w, h) = h264SpsSize(s.copyOfRange(4, s.size).let { rbsp(it, it.indices) }) ?: (1920 to 1080)
                VideoConfig(codec, w, h, listOf(s, p)).let { if (it.sameAs(c)) c else it }
            }
            VideoCodec.HEVC -> {
                val v = vps ?: return null; val s = sps ?: return null; val p = pps ?: return null
                val (w, h) = hevcSpsSize(s.copyOfRange(4, s.size).let { rbsp(it, it.indices) }) ?: (1920 to 1080)
                VideoConfig(codec, w, h, listOf(v + s + p)).let { if (it.sameAs(c)) c else it }
            }
            else -> {
                val sh = seqHeader?.takeIf { it.size >= 8 } ?: return null
                val w = ((sh[4].toInt() and 0xFF) shl 4) or ((sh[5].toInt() and 0xFF) shr 4)
                val h = ((sh[5].toInt() and 0x0F) shl 8) or (sh[6].toInt() and 0xFF)
                VideoConfig(codec, w, h, listOf(sh)).let { if (it.sameAs(c)) c else it }
            }
        }
    }

    private fun scanH264(b: ByteArray): Boolean {
        var key = false
        for (r in nalUnits(b)) {
            if (r.isEmpty()) continue
            when (b[r.first].toInt() and 0x1F) {
                7 -> sps = withStartCode(b, r)
                8 -> pps = withStartCode(b, r)
                5 -> key = true   // IDR
                1 -> if (!key && sps != null) {   // a non-IDR slice: an I slice also starts a picture the decoder can show (open GOP broadcasts)
                    try {
                        val br = BitReader(rbsp(b, r.first + 1..minOf(r.last, r.first + 16)), 0)
                        br.ue()
                        val t = br.ue() % 5
                        if (t == 2 || t == 4) key = true
                    } catch (_: RuntimeException) {}
                }
            }
        }
        return key
    }

    private fun scanHevc(b: ByteArray): Boolean {
        var key = false
        for (r in nalUnits(b)) {
            if (r.isEmpty()) continue
            when ((b[r.first].toInt() shr 1) and 0x3F) {
                32 -> vps = withStartCode(b, r)
                33 -> sps = withStartCode(b, r)
                34 -> pps = withStartCode(b, r)
                in 16..21 -> key = true   // IRAP: BLA, IDR, CRA
            }
        }
        return key
    }

    private fun scanMpeg2(b: ByteArray): Boolean {
        var key = false
        var i = 0
        while (i + 6 <= b.size) {
            if (b[i].toInt() == 0 && b[i + 1].toInt() == 0 && b[i + 2].toInt() == 1) {
                when (b[i + 3].toInt() and 0xFF) {
                    0xB3 -> {   // sequence header: keep it up to the next picture or GOP start (with its extension)
                        var j = i + 4
                        while (j + 4 <= b.size && !(b[j].toInt() == 0 && b[j + 1].toInt() == 0 && b[j + 2].toInt() == 1 &&
                                ((b[j + 3].toInt() and 0xFF) == 0x00 || (b[j + 3].toInt() and 0xFF) == 0xB8))) j++
                        if (j + 4 > b.size) j = b.size
                        seqHeader = b.copyOfRange(i, j)
                    }
                    0x00 -> if (((b[i + 5].toInt() and 0xFF) shr 3) and 7 == 1) key = true   // picture_coding_type 1 = I
                }
                i += 4
            } else i++
        }
        return key
    }
}
