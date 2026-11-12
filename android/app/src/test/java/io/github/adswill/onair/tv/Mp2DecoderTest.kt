package io.github.adswill.onair.tv

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.log10
import kotlin.math.sin

/**
 * A layer II round trip: a small encoder written here (the analysis filter bank of 11172-3 annex C.1.3 with C = D / 32, scale factors,
 * 16-bit quantisation of the lower subbands, the bit stream of 2.4.1) and the app's decoder. Two tones in, the same two tones out,
 * delayed by the filter banks' 481 samples.
 */
class Mp2DecoderTest {
    private class Encoder(val stereo: Boolean = false, val kbps: Int = 192, val usedBands: Int = 4) {
        private val x = Array(2) { FloatArray(512) }
        private val c = FloatArray(512) { Mp2Decoder.D[it] / 32f }
        private val m = Array(32) { k -> DoubleArray(64) { i -> cos((2 * k + 1) * (i - 16) * PI / 64) } }

        private fun analyse(ch: Int, input: FloatArray, off: Int, out: FloatArray) {
            val xx = x[ch]
            System.arraycopy(xx, 0, xx, 32, 480)
            for (i in 0 until 32) xx[i] = input[off + 31 - i]
            val y = DoubleArray(64)
            for (i in 0 until 64) { var s = 0.0; for (j in 0 until 8) s += c[i + 64 * j] * xx[i + 64 * j]; y[i] = s }
            for (k in 0 until 32) { var s = 0.0; for (i in 0 until 64) s += m[k][i] * y[i]; out[k] = s.toFloat() }
        }

        fun frame(pcm: Array<FloatArray>, off: Int): ByteArray {
            val nch = if (stereo) 2 else 1
            val s = Array(nch) { Array(36) { FloatArray(32) } }
            for (ch in 0 until nch) for (t in 0 until 36) analyse(ch, pcm[ch], off + t * 32, s[ch][t])
            val w = BitWriter()
            val bri = intArrayOf(0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384).indexOf(kbps)
            w.bits(0x7FF, 11).bits(3, 2).bits(2, 2).bits(1, 1).bits(bri, 4).bits(1, 2).bits(0, 1).bits(0, 1)
                .bits(if (stereo) 0 else 3, 2).bits(0, 2).bits(0, 4)
            val table = Mp2Decoder.selectTable(kbps, nch, 48000)
            assertEquals(0, table)
            val rowBits = IntArray(27) { if (it < 11) 4 else if (it < 23) 3 else 2 }
            for (i in 0 until 27) for (ch in 0 until nch) w.bits(if (i < usedBands) 15 else 0, rowBits[i])   // 15: 65535 steps, 16 bits
            for (i in 0 until usedBands) for (ch in 0 until nch) w.bits(0, 2)   // scfsi 0: three scale factors
            val sfi = Array(nch) { Array(usedBands) { IntArray(3) } }
            for (i in 0 until usedBands) for (ch in 0 until nch) for (part in 0 until 3) {   // subband by subband, then channel (2.4.1.6)
                var mx = 1e-9f
                for (t in part * 12 until part * 12 + 12) mx = maxOf(mx, abs(s[ch][t][i]))
                var idx = 62
                while (idx > 0 && Math.pow(2.0, 1.0 - idx / 3.0) < mx) idx--
                sfi[ch][i][part] = idx
                w.bits(idx, 6)
            }
            for (part in 0 until 3) for (l in 0 until 12 step 3) for (i in 0 until usedBands) for (ch in 0 until nch) for (k in 0 until 3) {
                val scale = Math.pow(2.0, 1.0 - sfi[ch][i][part] / 3.0)
                val xq = (s[ch][part * 12 + l + k][i] / scale).coerceIn(-1.0, 1.0)
                val v = Math.round((xq * 65535 + 65534) / 2).toInt().coerceIn(0, 65534)
                w.bits(v, 16)
            }
            val len = 144 * kbps * 1000 / 48000
            val b = w.toBytes()
            assertTrue("frame fits", b.size <= len)
            return b + ByteArray(len - b.size)
        }
    }

    private fun tone(n: Int, f1: Double, a1: Double, f2: Double, a2: Double) =
        FloatArray(n) { (a1 * sin(2 * PI * f1 * it / 48000) + a2 * sin(2 * PI * f2 * it / 48000)).toFloat() }

    private fun snrAfterDelay(input: FloatArray, output: FloatArray, from: Int, n: Int): Triple<Int, Double, Double> {
        var best = Triple(0, 0.0, -1e9)
        for (lag in 400..560) {
            var xy = 0.0; var xx = 0.0
            for (i in from until from + n) { xy += input[i] * output[i + lag]; xx += input[i] * input[i] }
            val g = xy / xx
            var e = 0.0
            for (i in from until from + n) { val d = output[i + lag] - g * input[i]; e += d * d }
            val snr = 10 * log10(g * g * xx / maxOf(e, 1e-30))
            if (snr > best.third) best = Triple(lag, g, snr)
        }
        return best
    }

    @Test fun monoRoundTripTwoTones() {
        val frames = 24
        val input = tone(1152 * frames + 2048, 1000.0, 0.5, 310.0, 0.2)
        val enc = Encoder()
        val dec = Mp2Decoder()
        val out = FloatArray(1152 * frames)
        for (f in 0 until frames) {
            val pcm = dec.decode(enc.frame(arrayOf(input), f * 1152))!!
            assertEquals(1, pcm.channels); assertEquals(48000, pcm.sampleRate); assertEquals(1152, pcm.frames)
            for (i in 0 until 1152) out[f * 1152 + i] = pcm.samples[i] / 32767f
        }
        val (lag, gain, snr) = snrAfterDelay(input, out, 2000, 20000)
        assertTrue("filter bank delay $lag (481 expected)", lag in 450..520)
        assertTrue("gain $gain", gain in 0.97..1.03)
        assertTrue("SNR $snr dB", snr > 50)
    }

    @Test fun stereoChannelsStaySeparate() {
        val frames = 12
        val left = tone(1152 * frames + 2048, 1000.0, 0.4, 0.0, 0.0)
        val right = tone(1152 * frames + 2048, 440.0, 0.3, 0.0, 0.0)
        val enc = Encoder(stereo = true, kbps = 384)
        val dec = Mp2Decoder()
        val l = FloatArray(1152 * frames); val r = FloatArray(1152 * frames)
        for (f in 0 until frames) {
            val pcm = dec.decode(enc.frame(arrayOf(left, right), f * 1152))!!
            assertEquals(2, pcm.channels)
            for (i in 0 until 1152) { l[f * 1152 + i] = pcm.samples[2 * i] / 32767f; r[f * 1152 + i] = pcm.samples[2 * i + 1] / 32767f }
        }
        assertTrue(snrAfterDelay(left, l, 2000, 9000).third > 45)
        assertTrue(snrAfterDelay(right, r, 2000, 9000).third > 45)
    }

    @Test fun silenceAndRejects() {
        val enc = Encoder(usedBands = 0)
        val pcm = Mp2Decoder().decode(enc.frame(arrayOf(FloatArray(4096)), 0))!!
        assertTrue(pcm.samples.all { it.toInt() == 0 })
        assertNull(Mp2Decoder().decode(ByteArray(10)))
        // a layer III header is not decoded here
        val l3 = BitWriter().bits(0x7FF, 11).bits(3, 2).bits(1, 2).bits(1, 1).bits(9, 4).bits(1, 2).bits(0, 10).toBytes() + ByteArray(500)
        assertNull(Mp2Decoder().decode(l3))
        // the allocation table choice of 11172-3 table B.2 (by bit rate per channel and sampling rate)
        assertEquals(0, Mp2Decoder.selectTable(384, 2, 48000))
        assertEquals(2, Mp2Decoder.selectTable(64, 2, 48000))
        assertEquals(1, Mp2Decoder.selectTable(256, 2, 44100))
        assertEquals(0, Mp2Decoder.selectTable(64, 1, 32000))
        assertEquals(3, Mp2Decoder.selectTable(96, 2, 32000))
    }
}
