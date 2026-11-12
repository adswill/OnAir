package io.github.adswill.onair.tv

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.random.Random

class AudioFramesTest {
    private val rnd = Random(11)

    private fun adts(raw: ByteArray, profile: Int = 1, sfi: Int = 3, ch: Int = 2): ByteArray {
        val len = 7 + raw.size
        val h = BitWriter().bits(0xFFF, 12).bits(0, 1).bits(0, 2).bits(1, 1).bits(profile, 2).bits(sfi, 4).bits(0, 1).bits(ch, 3)
            .bits(0, 4).bits(len, 13).bits(0x7FF, 11).bits(0, 2).toBytes()
        return h + raw
    }

    private fun collect(f: AudioFramer, chunks: List<Pair<ByteArray, Long>>, damaged: Set<Int> = emptySet()): List<Pair<AudioFrame, AudioConfig>> {
        val out = ArrayList<Pair<AudioFrame, AudioConfig>>()
        chunks.forEachIndexed { k, (d, pts) -> f.push(d, pts, k in damaged) { fr, c -> out += fr to c } }
        return out
    }

    @Test fun adtsFramesGetTheirPesPtsOrFollowOn() {
        val raws = List(10) { ByteArray(293) { rnd.nextInt(256).toByte() } }
        val stream = raws.fold(ByteArray(0)) { a, r -> a + adts(r) }   // 300 bytes per frame
        val cuts = listOf(0, 450, 1000, 2000, 2600, stream.size)
        val pts = listOf(1_000_000L, 2_000_000L, 3_000_000L, 4_000_000L, 5_000_000L)
        val chunks = (0 until 5).map { k -> stream.copyOfRange(cuts[k], cuts[k + 1]) to pts[k] }
        val got = collect(AdtsFramer(), chunks)
        assertEquals(10, got.size)
        raws.forEachIndexed { j, r -> assertArrayEquals("frame $j", r, got[j].first.data) }
        val d = 1024L * 1_000_000 / 48000
        val want = listOf(pts[0], pts[0] + d, pts[1], pts[1] + d, pts[2], pts[2] + d, pts[2] + 2 * d, pts[3], pts[3] + d, pts[4])
        assertEquals(want, got.map { it.first.ptsUs })
        val c = got[0].second
        assertEquals("audio/mp4a-latm", c.mime)
        assertEquals(48000, c.sampleRate); assertEquals(2, c.channels)
        assertArrayEquals(byteArrayOf(0x11, 0x90.toByte()), c.csd)   // AAC LC, 48 kHz, stereo
    }

    @Test fun adtsResyncsAfterGarbageAndDropsAcrossADamagedPes() {
        val f1 = adts(ByteArray(100) { 1 }); val f2 = adts(ByteArray(100) { 2 }); val f3 = adts(ByteArray(100) { 3 })
        val got = collect(AdtsFramer(), listOf((byteArrayOf(0x12, 0xFF.toByte(), 0x00) + f1) to 10L, f2.copyOfRange(0, 50) to 20L, (f2.copyOfRange(50, f2.size) + f3) to 30L), damaged = setOf(2))
        // f1 after 3 bytes of garbage; f2's second half arrives in a damaged PES: f2 is lost, f3 is found again
        assertEquals(listOf(1.toByte(), 3.toByte()), got.map { it.first.data[0] })
    }

    /** LOAS/LATM with StreamMuxConfig (version 0) in the first element and useSameStreamMux in the second. */
    private fun latm(payload: ByteArray, withConfig: Boolean, asc: (BitWriter) -> Unit): ByteArray {
        val w = BitWriter()
        w.bit(!withConfig)
        if (withConfig) {
            w.bits(0, 1).bits(1, 1).bits(0, 6).bits(0, 4).bits(0, 3)
            asc(w)
            w.bits(0, 3).bits(0xFF, 8).bits(0, 1).bits(0, 1)
        }
        var n = payload.size
        while (n >= 255) { w.bits(255, 8); n -= 255 }
        w.bits(n, 8)
        w.bytes(payload)
        val el = w.toBytes()
        return BitWriter().bits(0x2B7, 11).bits(el.size, 13).toBytes() + el
    }

    @Test fun latmGivesRawAacAndTheConfigOfTheStreamMuxConfig() {
        val lc: (BitWriter) -> Unit = { it.bits(2, 5).bits(3, 4).bits(2, 4).bits(0, 3) }
        val p1 = ByteArray(300) { rnd.nextInt(256).toByte() }
        val p2 = ByteArray(17) { rnd.nextInt(256).toByte() }
        val s = latm(p1, true, lc) + latm(p2, false, lc)
        val got = collect(LatmFramer(), listOf(s to 500L))
        assertEquals(2, got.size)
        assertArrayEquals(p1, got[0].first.data)
        assertArrayEquals(p2, got[1].first.data)
        assertArrayEquals(byteArrayOf(0x11, 0x90.toByte()), got[0].second.csd)
        assertEquals(500L + 1024L * 1_000_000 / 48000, got[1].first.ptsUs)
    }

    @Test fun latmHeAacExplicitSbr() {
        // AOT 5 (SBR), core 24 kHz stereo, extension 48 kHz, core AOT 2
        val he: (BitWriter) -> Unit = { it.bits(5, 5).bits(6, 4).bits(2, 4).bits(3, 4).bits(2, 5).bits(0, 3) }
        val got = collect(LatmFramer(), listOf(latm(ByteArray(64) { 9 }, true, he) to 0L))
        assertEquals(1, got.size)
        assertEquals(2048, got[0].first.samples)
        assertEquals(48000, got[0].first.sampleRate)
        assertEquals(24000, got[0].second.sampleRate)
        assertEquals(1024L * 1_000_000 / 24000, got[0].first.durationUs)
    }

    @Test fun latmWithoutAConfigYetGivesNothing() {
        val lc: (BitWriter) -> Unit = { it.bits(2, 5).bits(3, 4).bits(2, 4).bits(0, 3) }
        val got = collect(LatmFramer(), listOf((latm(ByteArray(20), false, lc) + latm(ByteArray(20) { 5 }, true, lc)) to 0L))
        assertEquals(1, got.size)
        assertEquals(5.toByte(), got[0].first.data[0])
    }

    private fun ac3(fscod: Int, frmsizecod: Int, acmod: Int, lfe: Boolean, bytes: Int): ByteArray {
        val w = BitWriter().bits(0x0B77, 16).bits(0, 16).bits(fscod, 2).bits(frmsizecod, 6).bits(8, 5).bits(0, 3).bits(acmod, 3)
        if (acmod and 1 != 0 && acmod != 1) w.bits(0, 2)
        if (acmod and 4 != 0) w.bits(0, 2)
        if (acmod == 2) w.bits(0, 2)
        w.bit(lfe)
        val h = w.toBytes()
        return h + ByteArray(bytes - h.size) { 0x55 }
    }

    @Test fun ac3FrameSizesAndChannels() {
        // 384 kbit/s: 1536 bytes at 48 kHz, 835/836 words at 44.1 kHz, 2304 bytes at 32 kHz
        for ((fs, code, len) in listOf(Triple(0, 28, 1536), Triple(1, 28, 1670), Triple(1, 29, 1672), Triple(2, 28, 2304), Triple(0, 0, 128), Triple(0, 37, 2560))) {
            val f = ac3(fs, code, 7, true, len)
            val got = collect(Ac3Framer(), listOf((f + f) to 0L))
            assertEquals("fscod $fs code $code", 2, got.size)
            assertEquals(len, got[0].first.data.size)
            assertEquals(6, got[0].first.channels)   // 3/2 + LFE
            assertEquals("audio/ac3", got[0].second.mime)
            assertEquals(1536, got[0].first.samples)
        }
        assertEquals(2, collect(Ac3Framer(), listOf((ac3(0, 10, 2, false, 320) + ac3(0, 10, 2, false, 320)) to 0L))[0].first.channels)   // 80 kbit/s, 2/0
    }

    private fun eac3(strmtyp: Int, frmsiz: Int): ByteArray {
        val h = BitWriter().bits(0x0B77, 16).bits(strmtyp, 2).bits(0, 3).bits(frmsiz, 11).bits(0, 2).bits(3, 2).bits(2, 3).bits(0, 1).bits(16, 5).toBytes()
        return h + ByteArray((frmsiz + 1) * 2 - h.size) { strmtyp.toByte() }
    }

    @Test fun eac3KeepsDependentSubstreamsWithTheirFrame() {
        val i = eac3(0, 255); val d = eac3(1, 127)
        val got = collect(Ac3Framer(), listOf((i + d + i + d + i + d) to 0L))
        assertEquals(2, got.size)   // the third waits for the next header to know whether more dependent substreams follow
        assertEquals(512 + 256, got[0].first.data.size)
        assertEquals("audio/eac3", got[0].second.mime)
        assertEquals(1536, got[0].first.samples)   // six blocks of 256
        assertEquals(32_000L, got[1].first.ptsUs - got[0].first.ptsUs)
    }

    private fun mpa(bri: Int, sri: Int, pad: Int, layerBits: Int = 2, ver: Int = 3): ByteArray {
        val h = BitWriter().bits(0x7FF, 11).bits(ver, 2).bits(layerBits, 2).bits(1, 1).bits(bri, 4).bits(sri, 2).bits(pad, 1).bits(0, 1).bits(0, 2).bits(0, 2).bits(0, 4).toBytes()
        val len = MpegAudioFramer.mpegHeader(h, 0)!!.length
        return h + ByteArray(len - 4) { 0x11 }
    }

    @Test fun mpegAudioLayerTwoLengths() {
        assertEquals(576, MpegAudioFramer.mpegHeader(mpa(10, 1, 0), 0)!!.length)    // 192 kbit/s, 48 kHz
        assertEquals(417, MpegAudioFramer.mpegHeader(mpa(8, 0, 0), 0)!!.length)     // 128 kbit/s, 44.1 kHz
        assertEquals(418, MpegAudioFramer.mpegHeader(mpa(8, 0, 1), 0)!!.length)
        assertEquals(1152, MpegAudioFramer.mpegHeader(mpa(10, 1, 0), 0)!!.samples)
        assertEquals(2, MpegAudioFramer.mpegHeader(mpa(10, 1, 0), 0)!!.layer)
        assertEquals(3, MpegAudioFramer.mpegHeader(mpa(9, 1, 0, layerBits = 1), 0)!!.layer)
        assertNull(MpegAudioFramer.mpegHeader(BitWriter().bits(0x7FF, 11).bits(3, 2).bits(2, 2).bits(1, 1).bits(15, 4).bits(1, 2).bits(0, 10).toBytes(), 0))
        val s = mpa(10, 1, 0) + mpa(10, 1, 0) + mpa(10, 1, 0)
        val got = collect(MpegAudioFramer(), listOf(s.copyOfRange(0, 1000) to 90_000L, s.copyOfRange(1000, s.size) to 200_000L))
        assertEquals(3, got.size)
        assertEquals(MpegAudioFramer.MIME_MPEG_L2, got[0].second.mime)
        assertEquals(listOf(90_000L, 114_000L, 200_000L), got.map { it.first.ptsUs })   // frame 2 starts at 1152, in the second PES
    }

    @Test fun bitReaderExpGolombAndCopy() {
        val b = BitWriter().ue(0).ue(1).ue(7).ue(255).bits(0b101, 3).toBytes(true)
        val r = BitReader(b, 0)
        assertEquals(0, r.ue()); assertEquals(1, r.ue()); assertEquals(7, r.ue()); assertEquals(255, r.ue())
        assertEquals(0b101, r.bits(3))
        val c = BitReader(byteArrayOf(0x0F, 0xF0.toByte()), 4).copyBits(8)
        assertArrayEquals(byteArrayOf(0xFF.toByte()), c)
        assertNotNull(AudioFramer.forFormat(AudioFormat.EAC3))
        assertTrue(detectAac(byteArrayOf(0x56, 0xE5.toByte())) == AudioFormat.AAC_LATM && detectAac(byteArrayOf(0xFF.toByte(), 0xF1.toByte())) == AudioFormat.AAC_ADTS)
    }
}
