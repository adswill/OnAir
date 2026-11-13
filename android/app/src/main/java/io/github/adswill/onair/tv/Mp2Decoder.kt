package io.github.adswill.onair.tv

/*
 * MPEG-1 / MPEG-2 (LSF) audio layer II decoder (ISO/IEC 11172-3 2.4.3.3 and annex B; 13818-3 for the half sampling rates), the
 * sound format of most standard-definition DVB-T, DVB-S and DVB-C services. Android phones rarely have a layer II decoder in
 * MediaCodec, so this small one is in the app. Layer II is not covered by patents any more (the format is from 1992; the desktop
 * build already ships FFmpeg's MP2 decoder), so including it follows the codec rules in docs/CODECS.md. Only the stereo base
 * layer: an MPEG-2 multichannel extension is ignored, as most decoders do.
 *
 * The tables are those of the standard: the bit allocation tables B.2a-d, the quantisation classes B.4 and the synthesis window
 * D[i] of table B.3. They are taken in the form FFmpeg keeps them (libavcodec/mpegaudiodata.c and mpegaudiodsp_data.c,
 * LGPL-2.1-or-later; see THIRD_PARTY.md), the decoder itself is written here. Pure Kotlin (Mp2DecoderTest runs a round trip
 * through an encoder written in the test).
 */
class Mp2Decoder {
    class Pcm(val samples: ShortArray, val channels: Int, val sampleRate: Int) {
        val frames: Int get() = samples.size / channels.coerceAtLeast(1)
    }

    private val v = Array(2) { FloatArray(1024) }
    private val vOff = IntArray(2)
    private val sb = Array(2) { Array(36) { FloatArray(32) } }
    var frames = 0L; private set
    var errors = 0L; private set

    fun reset() { for (x in v) x.fill(0f); vOff.fill(0) }

    /** Decodes one whole frame (header included); null when it is not a valid layer II frame. */
    fun decode(frame: ByteArray): Pcm? = try { decodeFrame(frame) } catch (e: RuntimeException) { errors++; null }

    private fun decodeFrame(frame: ByteArray): Pcm? {
        if (frame.size < 4) return null
        val h = MpegAudioFramer.mpegHeader(frame, 0) ?: return null
        if (h.layer != 2 || frame.size < h.length) return null
        val b2 = frame[1].toInt() and 0xFF
        val b4 = frame[3].toInt() and 0xFF
        val lsf = (b2 shr 3) and 1 == 0
        val crc = b2 and 1 == 0
        val mode = b4 shr 6
        val modeExt = (b4 shr 4) and 3
        val nch = if (mode == 3) 1 else 2
        val table = if (lsf) 4 else selectTable(h.kbps, nch, h.rate)
        val sblimit = SBLIMIT[table]
        val alloc = ALLOC[table]
        val bound = if (mode == 1) minOf((modeExt + 1) * 4, sblimit) else sblimit
        val br = BitReader(frame, if (crc) 48 else 32, h.length * 8L)

        val bitAlloc = Array(2) { IntArray(32) }
        val rowStart = IntArray(32)
        run {
            var j = 0
            for (i in 0 until sblimit) {
                rowStart[i] = j
                val bits = alloc[j]
                if (i < bound) for (ch in 0 until nch) bitAlloc[ch][i] = br.bits(bits)
                else { val a = br.bits(bits); bitAlloc[0][i] = a; bitAlloc[1][i] = a }
                j += 1 shl bits
            }
        }
        val scfsi = Array(2) { IntArray(32) }
        for (i in 0 until sblimit) for (ch in 0 until nch) if (bitAlloc[ch][i] != 0) scfsi[ch][i] = br.bits(2)
        val scale = Array(2) { Array(32) { FloatArray(3) } }
        for (i in 0 until sblimit) for (ch in 0 until nch) if (bitAlloc[ch][i] != 0) {
            val s = scale[ch][i]
            when (scfsi[ch][i]) {
                0 -> { s[0] = sf(br.bits(6)); s[1] = sf(br.bits(6)); s[2] = sf(br.bits(6)) }
                1 -> { s[0] = sf(br.bits(6)); s[1] = s[0]; s[2] = sf(br.bits(6)) }
                2 -> { s[0] = sf(br.bits(6)); s[1] = s[0]; s[2] = s[0] }
                else -> { s[0] = sf(br.bits(6)); s[2] = sf(br.bits(6)); s[1] = s[2] }
            }
        }
        val tmp = IntArray(3)
        for (k in 0 until 3) {
            for (l in 0 until 12 step 3) {
                for (i in 0 until 32) {
                    if (i >= sblimit) { for (ch in 0 until nch) for (m in 0 until 3) sb[ch][k * 12 + l + m][i] = 0f; continue }
                    val chans = if (i < bound) nch else 1
                    for (ch in 0 until chans) {
                        val a = bitAlloc[ch][i]
                        if (a == 0) {
                            for (c in (if (i < bound) ch..ch else 0 until nch)) for (m in 0 until 3) sb[c][k * 12 + l + m][i] = 0f
                            continue
                        }
                        val q = alloc[rowStart[i] + a]
                        val steps = QUANT_STEPS[q]
                        val bits = QUANT_BITS[q]
                        if (bits < 0) {   // three samples in one code word (3, 5 and 9 levels)
                            var code = br.bits(-bits)
                            for (m in 0 until 3) { tmp[m] = code % steps; code /= steps }
                        } else for (m in 0 until 3) tmp[m] = br.bits(bits)
                        for (m in 0 until 3) {
                            val x = (2 * tmp[m] + 1 - steps).toFloat() / steps
                            if (i < bound) sb[ch][k * 12 + l + m][i] = x * scale[ch][i][k]
                            else for (c in 0 until nch) sb[c][k * 12 + l + m][i] = x * scale[c][i][k]   // joint stereo: one sample, each channel its own scale
                        }
                    }
                }
            }
        }
        val out = ShortArray(1152 * nch)
        val pcm = FloatArray(32)
        for (ch in 0 until nch) for (t in 0 until 36) {
            synth(ch, sb[ch][t], pcm)
            for (j in 0 until 32) {
                val s = (pcm[j] * 32767f).let { if (it > 32767f) 32767f else if (it < -32768f) -32768f else it }
                out[(t * 32 + j) * nch + ch] = s.toInt().toShort()
            }
        }
        frames++
        return Pcm(out, nch, h.rate)
    }

    /** The polyphase synthesis of 11172-3 annex A, figure A.2: 32 subband samples in, 32 output samples. */
    private fun synth(ch: Int, s: FloatArray, out: FloatArray) {
        val vv = v[ch]
        var off = (vOff[ch] - 64) and 1023
        vOff[ch] = off
        for (i in 0 until 64) {
            var acc = 0f
            val row = i * 32
            for (k in 0 until 32) acc += N[row + k] * s[k]
            vv[(off + i) and 1023] = acc
        }
        for (j in 0 until 32) {
            var acc = 0f
            for (i in 0 until 8) {
                acc += vv[(off + i * 128 + j) and 1023] * D[i * 64 + j]
                acc += vv[(off + i * 128 + 96 + j) and 1023] * D[i * 64 + 32 + j]
            }
            out[j] = acc
        }
    }

    companion object {
        private fun sf(i: Int): Float = SCALE[i.coerceIn(0, 62)]
        private val SCALE = FloatArray(63) { Math.pow(2.0, 1.0 - it / 3.0).toFloat() }
        /** cos((16 + i)(2k + 1) pi / 64), i 0..63, k 0..31 */
        private val N = FloatArray(64 * 32) { val i = it / 32; val k = it % 32; Math.cos((16 + i) * (2 * k + 1) * Math.PI / 64).toFloat() }

        fun selectTable(kbps: Int, nch: Int, rate: Int): Int {
            val c = kbps / nch
            return when {
                (rate == 48000 && c >= 56) || (c in 56..80) -> 0
                rate != 48000 && c >= 96 -> 1
                rate != 32000 && c <= 48 -> 2
                else -> 3
            }
        }

        private val SBLIMIT = intArrayOf(27, 30, 8, 12, 30)
        private val QUANT_STEPS = intArrayOf(3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767, 65535)
        /** Bits per sample, negative when three samples are grouped in one code of that many bits. */
        private val QUANT_BITS = intArrayOf(-5, -7, 3, -10, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16)

        private fun rows(vararg r: IntArray): IntArray = r.fold(IntArray(0)) { a, b -> a + b }
        private fun rep(n: Int, row: IntArray) = Array(n) { row }
        private val A1 = rows(
            *rep(3, intArrayOf(4, 0, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16)),
            *rep(8, intArrayOf(4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16)),
            *rep(12, intArrayOf(3, 0, 1, 2, 3, 4, 5, 16)),
            *rep(7, intArrayOf(2, 0, 1, 16)),
        )
        private val A3 = rows(
            *rep(2, intArrayOf(4, 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)),
            *rep(10, intArrayOf(3, 0, 1, 3, 4, 5, 6, 7)),
        )
        private val A4 = rows(
            *rep(4, intArrayOf(4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14)),
            *rep(7, intArrayOf(3, 0, 1, 3, 4, 5, 6, 7)),
            *rep(19, intArrayOf(2, 0, 1, 3)),
        )
        private val ALLOC = arrayOf(A1, A1, A3, A3, A4)

        /** Half of the window (table B.3) as FFmpeg's ff_mpa_enwindow keeps it, in units of 2^-16; the other half follows by symmetry. */
        private val HALF = intArrayOf(
            0, -1, -1, -1, -1, -1, -1, -2, -2, -2, -2, -3, -3, -4, -4, -5,
            -5, -6, -7, -7, -8, -9, -10, -11, -13, -14, -16, -17, -19, -21, -24, -26,
            -29, -31, -35, -38, -41, -45, -49, -53, -58, -63, -68, -73, -79, -85, -91, -97,
            -104, -111, -117, -125, -132, -139, -147, -154, -161, -169, -176, -183, -190, -196, -202, -208,
            213, 218, 222, 225, 227, 228, 228, 227, 224, 221, 215, 208, 200, 189, 177, 163,
            146, 127, 106, 83, 57, 29, -2, -36, -72, -111, -153, -197, -244, -294, -347, -401,
            -459, -519, -581, -645, -711, -779, -848, -919, -991, -1064, -1137, -1210, -1283, -1356, -1428, -1498,
            -1567, -1634, -1698, -1759, -1817, -1870, -1919, -1962, -2001, -2032, -2057, -2075, -2085, -2087, -2080, -2063,
            2037, 2000, 1952, 1893, 1822, 1739, 1644, 1535, 1414, 1280, 1131, 970, 794, 605, 402, 185,
            -45, -288, -545, -814, -1095, -1388, -1692, -2006, -2330, -2663, -3004, -3351, -3705, -4063, -4425, -4788,
            -5153, -5517, -5879, -6237, -6589, -6935, -7271, -7597, -7910, -8209, -8491, -8755, -8998, -9219, -9416, -9585,
            -9727, -9838, -9916, -9959, -9966, -9935, -9863, -9750, -9592, -9389, -9139, -8840, -8492, -8092, -7640, -7134,
            6574, 5959, 5288, 4561, 3776, 2935, 2037, 1082, 70, -998, -2122, -3300, -4533, -5818, -7154, -8540,
            -9975, -11455, -12980, -14548, -16155, -17799, -19478, -21189, -22929, -24694, -26482, -28289, -30112, -31947, -33791, -35640,
            -37489, -39336, -41176, -43006, -44821, -46617, -48390, -50137, -51853, -53534, -55178, -56778, -58333, -59838, -61289, -62684,
            -64019, -65290, -66494, -67629, -68692, -69679, -70590, -71420, -72169, -72835, -73415, -73908, -74313, -74630, -74856, -74992,
            75038,
        )
        /** The synthesis window D[0..511] (table B.3). */
        val D = FloatArray(512).also { d ->
            for (i in 0..256) {
                var x = HALF[i] / 65536f
                d[i] = x
                if (i and 63 != 0) x = -x
                if (i != 0) d[512 - i] = x
            }
        }
    }
}
