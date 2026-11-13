// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.tv

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class TvServicesTest {
    /** The shape of native/src/jni_tv.cpp nativeTvServices. */
    private val json = """
        {"packets":123456,"network":"Dubai \"DTV\"","tsid":12,"onid":12421,"services":[
         {"id":1001,"name":"News HD","provider":"P","type":25,"pmt":4096,"pcr":256,"ca":false,"scrambled":false,"caState":1,"lcn":3,"havePmt":true,"now":"Bulletin","next":"","streams":[
           {"pid":256,"type":36,"codec":"HEVC","kind":"video","lang":"","kbps":4200.5,"scrambled":false},
           {"pid":257,"type":6,"codec":"AC-3","kind":"audio","lang":"eng","kbps":384,"scrambled":false},
           {"pid":258,"type":17,"codec":"HE-AAC","kind":"audio","lang":"ara","kbps":96,"scrambled":false}]},
         {"id":1002,"name":"Movies","provider":"P","type":1,"pmt":4097,"pcr":300,"ca":true,"scrambled":true,"caState":3,"lcn":1,"havePmt":true,"now":"","next":"","streams":[
           {"pid":300,"type":27,"codec":"H.264","kind":"video","lang":"","kbps":3000,"scrambled":true},
           {"pid":301,"type":15,"codec":"AAC","kind":"audio","lang":"eng","kbps":128,"scrambled":true}]},
         {"id":1003,"name":"","provider":"","type":1,"pmt":4098,"pcr":400,"ca":false,"scrambled":false,"caState":1,"lcn":0,"havePmt":true,"now":"","next":"","streams":[
           {"pid":400,"type":2,"codec":"MPEG-2","kind":"video","lang":"","kbps":3500,"scrambled":false},
           {"pid":401,"type":3,"codec":"MPEG-1 audio","kind":"audio","lang":"deu","kbps":192,"scrambled":false}]},
         {"id":1004,"name":"Radio One","provider":"","type":2,"pmt":4099,"pcr":500,"ca":false,"scrambled":false,"caState":1,"lcn":40,"havePmt":true,"now":"","next":"","streams":[
           {"pid":500,"type":15,"codec":"AAC","kind":"audio","lang":"eng","kbps":64,"scrambled":false}]},
         {"id":1005,"name":"Data","provider":"","type":12,"pmt":4100,"pcr":8191,"ca":false,"scrambled":false,"caState":0,"lcn":0,"havePmt":false,"now":"","next":"","streams":[]}
        ]}
    """.trimIndent()

    private val all = setOf("video/avc", "video/hevc", "video/mpeg2", "audio/mp4a-latm", "audio/ac3", "audio/eac3", MpegAudioFramer.MIME_MPEG_L2)
    private fun cat(vararg have: String) = DecoderCatalog { m -> if (m in have) "dec.$m" else null }
    private val typical = cat("video/avc", "video/hevc", "audio/mp4a-latm")   // a phone without MPEG-2, Dolby or MP2 decoders

    @Test fun parsesTheNativeJson() {
        val m = TvMux.parse(json)
        assertEquals(123456L, m.packets)
        assertEquals("Dubai \"DTV\"", m.network)
        assertEquals(5, m.services.size)
        val s = m.services[0]
        assertEquals(1001, s.id); assertEquals(3, s.lcn); assertEquals(256, s.pcrPid)
        assertEquals(3, s.streams.size)
        assertEquals(36, s.video!!.streamType)
        assertEquals("ara", s.audio[1].lang)
        assertEquals("Service 1003", m.services[2].title)
        assertTrue(m.services[3].radio)
        assertEquals(TvMux.empty.services, TvMux.parse("not json").services)
    }

    @Test fun listsByLcnAndDropsServicesWithoutStreams() {
        val l = listedServices(TvMux.parse(json))
        assertEquals(listOf(1002, 1001, 1004, 1003), l.map { it.id })
    }

    @Test fun picksAClearServiceThePhoneCanShow() {
        val l = listedServices(TvMux.parse(json))
        assertEquals(1001, pickService(l, null, typical)!!.id)                  // 1002 (LCN 1) is scrambled
        assertEquals(1003, pickService(l, 1003, typical)!!.id)                  // the remembered one wins
        assertEquals(1001, pickService(l, 9999, typical)!!.id)                  // remembered but gone
        assertEquals(1001, pickService(l, null, cat())!!.id)                    // no decoders at all: still the first clear TV service
        assertNull(pickService(emptyList(), null, typical))
    }

    @Test fun picksADecodableSoundTrackThenTheLanguage() {
        val news = TvMux.parse(json).services[0]
        assertEquals(258, pickAudio(news, typical, listOf("eng"))!!.pid)      // AC-3 cannot play here: the AAC one even in Arabic
        assertEquals(257, pickAudio(news, cat(*all.toTypedArray()), listOf("eng"))!!.pid)
        assertEquals(258, pickAudio(news, cat(*all.toTypedArray()), listOf("ara", "eng"))!!.pid)
        assertEquals(257, pickAudio(news, cat(*all.toTypedArray()), emptyList())!!.pid)   // PMT order
        assertEquals(257, pickAudio(news, typical, listOf("ara"), wantedPid = 257)!!.pid)  // the user's choice
        assertEquals(listOf(256, 258), pidsFor(news, pickAudio(news, typical, listOf("eng"))).take(2))
        assertTrue(256 in pidsFor(news, null))
    }

    @Test fun formatsOfTheStreams() {
        val s = TvMux.parse(json).services
        assertEquals(VideoCodec.HEVC, videoCodecOf(s[0].video!!))
        assertEquals(VideoCodec.MPEG2, videoCodecOf(s[2].video!!))
        assertEquals(AudioFormat.AC3, audioFormatOf(s[0].audio[0]))
        assertEquals(AudioFormat.AAC_LATM, audioFormatOf(s[0].audio[1]))
        assertEquals(AudioFormat.MPEG_AUDIO, audioFormatOf(s[2].audio[0]))
        val privAac = TvStream(1, 6, "AAC", "audio", "", 0.0, false)
        assertEquals(AudioFormat.AAC_LATM, audioFormatOf(privAac, byteArrayOf(0x56, 0xE1.toByte())))
        assertEquals(AudioFormat.AAC_ADTS, audioFormatOf(privAac, byteArrayOf(0xFF.toByte(), 0xF1.toByte())))
        assertNull(audioFormatOf(TvStream(1, 6, "AC-4", "audio", "", 0.0, false)))
        assertTrue(audioPlayability(TvStream(1, 6, "AC-4", "audio", "", 0.0, false), typical) is Playability.NotHandled)
        assertTrue(videoPlayability(TvStream(1, 0x42, "AVS", "video", "", 0.0, false), typical) is Playability.NotHandled)
    }

    @Test fun plainNotesForWhatThePhoneCannotPlay() {
        val s = TvMux.parse(json).services
        // MPEG-2 video without a phone decoder: said plainly; its MP2 sound plays with the app's own layer II decoder
        val mpeg2 = playabilityNotes(s[2], s[2].audio[0], typical)
        assertEquals(1, mpeg2.size)
        assertTrue(mpeg2[0], mpeg2[0].contains("no MPEG-2 video decoder"))
        assertEquals(BUILTIN_MP2, (audioPlayability(s[2].audio[0], typical) as Playability.Plays).decoder)
        assertEquals("dec.audio/mpeg-L2", (audioPlayability(s[2].audio[0], cat(MpegAudioFramer.MIME_MPEG_L2)) as Playability.Plays).decoder)
        assertTrue(playabilityNotes(s[0], s[0].audio[1], typical).isEmpty())
        val dolby = playabilityNotes(s[0], s[0].audio[0], typical)
        assertTrue(dolby.single().contains("AC-3") && dolby.single().contains("Dolby"))
        assertTrue(playabilityNotes(s[1], s[1].audio[0], typical).single().contains("scrambled"))
        assertTrue(playabilityNotes(s[3], s[3].audio[0], typical).isEmpty())   // radio: no picture is not a problem
    }
}
