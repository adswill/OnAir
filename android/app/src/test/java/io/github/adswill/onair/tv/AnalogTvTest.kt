package io.github.adswill.onair.tv

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The pure parts of the 4.1 Watch panel (docs/ANDROID-PLAYER-4.1.md): analogue picture facts and the lines for formats a phone cannot play. */
class AnalogTvTest {
    @Test fun analogueModesHaveAPictureSource() {
        assertEquals(AnalogSource.ANALOG_TV, analogSourceOf(8))
        assertEquals(AnalogSource.MAC, analogSourceOf(47))
        assertEquals(AnalogSource.FPV, analogSourceOf(101))
        // the digital TV modes (DVB-T 0, DVB-T2 1, ISDB-T 3, DTMB 7 ...) play the transport stream instead
        for (f in listOf(0, 1, 2, 3, 4, 6, 7, 40)) assertNull("family $f", analogSourceOf(f))
        assertEquals(listOf(0, 1, 2), AnalogSource.values().map { it.code })   // the `which` of nativeAnalogFrame
    }

    @Test fun theModeTableStillHasThoseFamiliesInTheTvGroup() {
        val tv = io.github.adswill.onair.Modes.all.filter { it.group == 0 && !it.removed }.map { it.family }.toSet()
        for (f in listOf(AnalogFamilies.ANALOG_TV, AnalogFamilies.MAC, AnalogFamilies.FPV)) assertTrue("family $f in the TV group", f in tv)
    }

    @Test fun frameInfoFromMeta() {
        val pal = AnalogFrameInfo.from(doubleArrayOf(42.0, 768.0, 576.0, 1.0, 0.0, 50.0))!!
        assertEquals(42L, pal.seq); assertEquals(768 * 576 * 4, pal.bytes); assertEquals(625, pal.lines)
        assertTrue(pal.colour); assertFalse(pal.untuned)
        val ntsc = AnalogFrameInfo.from(doubleArrayOf(1.0, 640.0, 480.0, 0.0, 1.0, 59.94))!!
        assertEquals(525, ntsc.lines); assertTrue(ntsc.untuned)
        assertNull(AnalogFrameInfo.from(doubleArrayOf(0.0, 0.0, 0.0, 0.0, 0.0, 0.0)))
        assertNull(AnalogFrameInfo.from(doubleArrayOf(1.0, 768.0)))
        assertNull(AnalogFrameInfo.from(doubleArrayOf(1.0, Double.NaN, 576.0, 0.0, 0.0, 50.0)))
        assertNull(AnalogFrameInfo.from(doubleArrayOf(1.0, 100000.0, 576.0, 0.0, 0.0, 50.0)))
    }

    @Test fun aspectOfTheReceiversPictures() {
        assertEquals(4f / 3f, analogAspect(768, 576), 1e-4f)
        assertEquals(4f / 3f, analogAspect(640, 480), 1e-4f)
        assertEquals(16f / 9f, analogAspect(1024, 576), 1e-4f)   // MAC 16:9
        assertEquals(4f / 3f, analogAspect(0, 0), 1e-4f)
        assertEquals(1f, analogAspect(100, 1000), 1e-4f)
        assertEquals(2.4f, analogAspect(5000, 10), 1e-4f)
    }

    @Test fun lineUnderThePicture() {
        assertEquals("Analog TV: waiting for a picture", analogLine(AnalogSource.ANALOG_TV, null))
        val l = analogLine(AnalogSource.ANALOG_TV, AnalogFrameInfo(1, 768, 576, true, false, 50.0))
        assertEquals("Analog TV 768x576, 625 lines, colour, 50 fields/s", l)
        val s = analogLine(AnalogSource.FPV, AnalogFrameInfo(1, 640, 480, false, true, 59.94))
        assertTrue(s, s.contains("59.94 fields/s") && s.contains("black and white") && s.contains("snow"))
    }

    @Test fun pollRate() {
        assertEquals(200L, analogPollMs(0.0))
        assertEquals(200L, analogPollMs(Double.NaN))
        val pal = analogPollMs(50.0)   // 25 pictures/s: asked about 30 times a second
        assertTrue("$pal", pal in 33..40)
        assertEquals(33L, analogPollMs(1000.0))   // never faster than 30 a second
        assertEquals(200L, analogPollMs(2.0))     // never slower than 5 a second
    }

    private val none = DecoderCatalog { null }

    @Test fun dtmbFormatsSayTheyAreUserInstalledOnTheDesktop() {
        val avs = notHandledNote("AVS+", 0x42, video = true)
        assertTrue(avs, avs.startsWith("AVS+ video") && avs.contains("ships none") && avs.contains("add-on you install yourself"))
        assertTrue(notHandledNote("type 0x42", 0x42, video = true).contains("AVS+ add-on"))
        assertTrue(notHandledNote("AVS2", 0xD2, video = true).startsWith("AVS2 video"))
        val dra = notHandledNote("DRA", 0x06, video = false)
        assertTrue(dra, dra.startsWith("DRA sound") && dra.contains("DRA add-on"))
        val ac4 = notHandledNote("AC-4", 0x06, video = false)
        assertTrue(ac4, ac4.startsWith("AC-4 sound is not handed") && !ac4.contains("add-on"))
    }

    @Test fun aDtmbServiceWithAvsAndDraExplainsBoth() {
        val svc = TvService(1, "CCTV-1", "", 1, 4096, 256, false, false, 1, true, "", "",
            listOf(TvStream(256, 0x42, "AVS+", "video", "", 0.0, false), TvStream(257, 0x06, "DRA", "audio", "chi", 0.0, false)))
        val notes = playabilityNotes(svc, svc.audio.first(), none)
        assertEquals(notes.toString(), 2, notes.size)
        assertTrue(notes[0].startsWith("AVS+ video"))
        assertTrue(notes[1].startsWith("DRA sound"))
    }

    @Test fun h264HevcMpeg2AacMp2Ac3AreHandedToMediaCodec() {
        val all = DecoderCatalog { m -> "dec.$m" }
        for ((t, c) in listOf(0x1B to "H.264", 0x24 to "HEVC", 0x02 to "MPEG-2"))
            assertTrue(c, videoPlayability(TvStream(1, t, c, "video", "", 0.0, false), all) is Playability.Plays)
        for ((t, c) in listOf(0x0F to "AAC", 0x03 to "MPEG-1 audio", 0x81 to "AC-3"))
            assertTrue(c, audioPlayability(TvStream(1, t, c, "audio", "", 0.0, false), all) is Playability.Plays)
        // a phone without an MP2 decoder still plays MP2 (the built-in layer II decoder); without AC-3 it says so
        assertTrue(audioPlayability(TvStream(1, 0x03, "MPEG-1 audio", "audio", "", 0.0, false), none) is Playability.Plays)
        assertTrue(audioPlayability(TvStream(1, 0x81, "AC-3", "audio", "", 0.0, false), none) is Playability.NoDecoder)
    }
}
