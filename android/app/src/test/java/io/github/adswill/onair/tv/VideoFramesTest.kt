package io.github.adswill.onair.tv

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream

class VideoFramesTest {
    /** Adds emulation-prevention bytes as an encoder does (00 00 0x with x <= 3 becomes 00 00 03 0x). */
    private fun epb(b: ByteArray): ByteArray {
        val o = ByteArrayOutputStream()
        var zeros = 0
        for (x in b) {
            val v = x.toInt() and 0xFF
            if (zeros >= 2 && v <= 3) { o.write(3); zeros = 0 }
            o.write(v)
            zeros = if (v == 0) zeros + 1 else 0
        }
        return o.toByteArray()
    }

    private val sc4 = byteArrayOf(0, 0, 0, 1)
    private val sc3 = byteArrayOf(0, 0, 1)

    private fun h264Sps(frameMbsOnly: Boolean): ByteArray {
        val w = BitWriter().bits(0x67, 8).bits(100, 8).bits(0, 8).bits(40, 8).ue(0)
            .ue(1).ue(0).ue(0).bits(0, 1).bits(0, 1)   // chroma 4:2:0, 8 bit, no scaling matrices
            .ue(0).ue(0).ue(2).ue(1).bits(0, 1)
            .ue(119)
        if (frameMbsOnly) w.ue(67).bits(1, 1) else w.ue(33).bits(0, 1).bits(1, 1)
        w.bits(1, 1)   // direct_8x8_inference
        w.bits(1, 1).ue(0).ue(0).ue(0).ue(if (frameMbsOnly) 4 else 2)   // crop 8 lines at the bottom
        w.bits(0, 1)   // no VUI
        return epb(w.toBytes(stopBit = true))
    }

    private fun slice(type: Int, idr: Boolean): ByteArray =
        epb(BitWriter().bits(if (idr) 0x65 else 0x41, 8).ue(0).ue(type).ue(0).bits(0xA5, 8).toBytes(stopBit = true))

    private val pps = byteArrayOf(0x68, 0xCE.toByte(), 0x3C, 0x80.toByte())

    @Test fun h264SpsSizeProgressiveAndInterlaced() {
        for (prog in listOf(true, false)) {
            val sps = h264Sps(prog)
            assertEquals(1920 to 1080, h264SpsSize(rbsp(sps, sps.indices)))
        }
        assertNull(h264SpsSize(byteArrayOf(0x67, 100)))
    }

    @Test fun hevcSpsSize() {
        val w = BitWriter().bits(0x4201, 16).bits(0, 4).bits(0, 3).bits(1, 1)
            .bits(0, 2).bits(0, 1).bits(1, 5).bits(0x60000000L, 32).bits(0b1001, 4).bits(0, 43).bits(0, 1).bits(120, 8)
            .ue(0).ue(1).ue(1920).ue(1088).bits(1, 1).ue(0).ue(0).ue(0).ue(4)
        val sps = epb(w.toBytes(stopBit = true))
        assertEquals(1920 to 1080, hevcSpsSize(rbsp(sps, sps.indices)))
    }

    @Test fun nalUnitsWithThreeAndFourByteStartCodes() {
        val b = sc4 + byteArrayOf(9, 1, 2) + sc3 + byteArrayOf(7, 7) + sc4 + byteArrayOf(5)
        val r = nalUnits(b)
        assertEquals(3, r.size)
        assertArrayEquals(byteArrayOf(9, 1, 2), b.copyOfRange(r[0].first, r[0].last + 1))
        assertArrayEquals(byteArrayOf(7, 7), b.copyOfRange(r[1].first, r[1].last + 1))
        assertArrayEquals(byteArrayOf(5), b.copyOfRange(r[2].first, r[2].last + 1))
        assertArrayEquals(byteArrayOf(1, 0, 0, 2), rbsp(byteArrayOf(1, 0, 0, 3, 2), 0..4))
    }

    @Test fun h264StartsAtTheFirstKeyFrameAfterParameterSets() {
        val f = VideoFramer(VideoCodec.H264)
        val out = ArrayList<Pair<VideoAu, VideoConfig>>()
        val sink: (VideoAu, VideoConfig) -> Unit = { a, c -> out += a to c }
        val sps = h264Sps(true)
        f.push(sc4 + slice(0, false), 1, 1, false, sink)                                    // P before anything: dropped
        f.push(sc4 + sps + sc4 + pps + sc4 + slice(0, false), 2, 2, false, sink)             // parameter sets but a P slice: dropped
        f.push(sc4 + sps + sc4 + pps + sc4 + slice(5, true), 3, 3, false, sink)              // IDR: starts
        f.push(sc4 + slice(0, false), 4, 4, false, sink)
        f.push(sc4 + slice(0, false), 5, 5, true, sink)                                      // damaged: stop until a key frame
        f.push(sc4 + slice(0, false), 6, 6, false, sink)
        f.push(sc4 + slice(7, false), 7, 7, false, sink)                                     // an I slice (open GOP) restarts
        assertEquals(listOf(3L, 4L, 7L), out.map { it.first.ptsUs })
        assertEquals(listOf(true, false, true), out.map { it.first.key })
        val cfg = out[0].second
        assertEquals(1920, cfg.width); assertEquals(1080, cfg.height)
        assertArrayEquals(sc4 + sps, cfg.csd[0])
        assertArrayEquals(sc4 + pps, cfg.csd[1])
        assertTrue(out[1].second === cfg)   // unchanged config: the same object, so the player keeps its decoder
        assertEquals(4, f.dropped)
    }

    @Test fun hevcNeedsVpsSpsPpsAndAnIrap() {
        val f = VideoFramer(VideoCodec.HEVC)
        val out = ArrayList<VideoAu>()
        val vps = byteArrayOf(0x40, 0x01, 0x0C); val sps = byteArrayOf(0x42, 0x01, 0x01); val ppsH = byteArrayOf(0x44, 0x01, 0xC1.toByte())
        val cra = byteArrayOf(0x2A, 0x01, 0xAF.toByte()); val trail = byteArrayOf(0x02, 0x01, 0xD0.toByte())
        f.push(sc4 + sps + sc4 + ppsH + sc4 + cra, 1, 1, false) { a, _ -> out += a }          // no VPS yet
        f.push(sc4 + vps + sc4 + sps + sc4 + ppsH + sc4 + trail, 2, 2, false) { a, _ -> out += a }   // not an IRAP
        f.push(sc4 + vps + sc4 + sps + sc4 + ppsH + sc4 + cra, 3, 3, false) { a, _ -> out += a }
        f.push(sc4 + trail, 4, 4, false) { a, _ -> out += a }
        assertEquals(listOf(3L, 4L), out.map { it.ptsUs })
        assertEquals(1, f.config!!.csd.size)
        assertArrayEquals(sc4 + vps + sc4 + sps + sc4 + ppsH, f.config!!.csd[0])
        assertEquals(1920 to 1080, f.config!!.width to f.config!!.height)   // unreadable SPS: the nominal size, the decoder takes the real one
    }

    @Test fun mpeg2SequenceHeaderAndIPictures() {
        val seq = sc3 + byteArrayOf(0xB3.toByte()) + BitWriter().bits(720, 12).bits(576, 12).bits(3, 4).bits(3, 4).bits(15000, 18).bits(1, 1).bits(112, 10).bits(0, 3).toBytes()
        val gop = sc3 + byteArrayOf(0xB8.toByte(), 0, 8, 0, 0)
        fun pic(type: Int) = sc3 + byteArrayOf(0) + BitWriter().bits(5, 10).bits(type, 3).bits(0xFFFF, 16).bits(0, 3).toBytes() + sc3 + byteArrayOf(1, 0x11, 0x22)
        val f = VideoFramer(VideoCodec.MPEG2)
        val out = ArrayList<VideoAu>()
        f.push(pic(1), 1, 1, false) { a, _ -> out += a }               // I picture but no sequence header yet
        f.push(seq + gop + pic(2), 2, 2, false) { a, _ -> out += a }   // P
        f.push(seq + gop + pic(1), 3, 3, false) { a, _ -> out += a }
        f.push(pic(3), 4, 4, false) { a, _ -> out += a }
        assertEquals(listOf(3L, 4L), out.map { it.ptsUs })
        val c = f.config
        assertNotNull(c)
        assertEquals(720, c!!.width); assertEquals(576, c.height)
        assertArrayEquals(seq, c.csd[0])   // up to the GOP header
        assertFalse(out[1].key)
    }
}
