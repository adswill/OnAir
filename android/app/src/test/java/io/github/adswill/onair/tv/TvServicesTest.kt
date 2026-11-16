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

    // issue #517: DTMB multiplexes without a NIT (474, 506, 554 MHz) showed nothing as the header; the SDT's provider name stands in, for any original network id
    private fun svc(id: Int, provider: String, vararg st: TvStream) = TvService(id, "S$id", provider, 1, 4096 + id, 256, false, false, 0, true, "", "", st.toList())
    private fun stream(pid: Int, type: Int, codec: String, kind: String, lang: String = "") = TvStream(pid, type, codec, kind, lang, 0.0, false)

    @Test fun providerNameStandsInForAMissingNetworkName() {
        val noNit = TvMux(1, "", 4, listOf(svc(41, "SZTC"), svc(42, "SZMG"), svc(43, "SZMG"), svc(44, "")), onid = 1)
        assertEquals("SZTC, SZMG", muxLabel(noNit))
        val other = TvMux(1, "", 7, listOf(svc(1, "TVB")), onid = 0x2025)   // any original network id, not only 1
        assertEquals("TVB", muxLabel(other))
        assertEquals("SZTVM", muxLabel(TvMux(1, "  SZTVM ", 1, listOf(svc(1, "Other")), onid = 1)))
        assertEquals("", muxLabel(TvMux(1, "", 1, listOf(svc(1, "")))))
        assertEquals("SZMG", providerNote(svc(42, "SZMG"), ""))
        assertEquals("", providerNote(svc(42, "SZMG"), "szmg"))
        assertEquals("", providerNote(svc(42, ""), "x"))
        assertEquals(0x2025, TvMux.parse("""{"packets":1,"network":"","tsid":1,"onid":8229,"services":[]}""").onid)
    }

    @Test fun subtitlesAndDataAreListedEvenWhenThePhoneCannotShowThem() {
        val tvb = svc(1, "TVB", stream(256, 0x1B, "H.264", "video"), stream(257, 0x81, "AC-3", "audio", "chi"), stream(258, 0x81, "AC-3", "audio", "eng"),
            stream(259, 0x06, "DVB subtitles", "subtitles", "chi"), stream(260, 0x0B, "DSM-CC", "data"), stream(261, 0x05, "private", "data"))
        assertEquals(listOf(259), tvb.subtitles.map { it.pid })
        assertEquals(listOf(260), tvb.interactive.map { it.pid })
        assertEquals("CHI - DVB subtitles - not supported yet", subtitleTrackText(tvb.subtitleTracks[0], "Unknown language", "hearing impaired", "not supported yet"))
        assertTrue(interactiveLabel(tvb.interactive[0]).contains("not supported on this phone"))
        assertEquals("chi AC-3 (needs a decoder: this phone has none)", audioLabel(tvb.audio[0], typical))
        assertEquals("chi AC-3", audioLabel(tvb.audio[0], all.let { s -> DecoderCatalog { m -> if (m in s) "dec" else null } }))
    }

    private fun sub(pid: Int, lang: String, hi: Boolean = false, ttx: Boolean = false, type: String = "DVB subtitles", page: Int = 0) =
        TvStream(pid, 0x06, type, "subtitles", lang, 0.0, false, listOf(TvSubEntry(lang, "", type, hi, ttx, page)))

    @Test fun subtitleTracksComeFromTheDescriptorsAndTheLanguageIsPicked() {
        val json = """{"packets":1,"services":[{"id":1,"name":"TVB","streams":[
            {"pid":300,"type":6,"codec":"DVB subtitles","kind":"subtitles","lang":"chs","subs":[
              {"lang":"chs","name":"Chinese (simplified)","kind":"DVB subtitles","hi":false,"ttx":false,"page":1},
              {"lang":"cht","name":"Chinese (traditional)","kind":"DVB subtitles","hi":true,"ttx":false,"page":2}]},
            {"pid":301,"type":6,"codec":"Teletext","kind":"teletext","lang":"eng","subs":[
              {"lang":"eng","name":"English","kind":"Teletext subtitles","hi":false,"ttx":true,"page":888},
              {"lang":"eng","name":"English","kind":"Teletext initial page","hi":false,"ttx":true,"page":100}]}]}]}"""
        val tracks = TvMux.parse(json).services[0].subtitleTracks
        assertEquals(3, tracks.size)   // the teletext initial page is not a subtitle
        assertEquals("Chinese (traditional) - DVB subtitles (hearing impaired) - not supported yet", subtitleTrackText(tracks[1], "?", "hearing impaired", "not supported yet"))
        assertEquals("English - Teletext subtitles 888 - not supported yet", subtitleTrackText(tracks[2], "?", "hearing impaired", "not supported yet"))
        assertNull(pickSubtitle(tracks, emptyList(), null))                       // nothing preferred: off
        assertEquals(tracks[0].key, pickSubtitle(tracks, listOf("zho"), null)?.key)   // CHS satisfies a preferred Chinese, normal before hearing impaired
        assertEquals(tracks[2].key, pickSubtitle(tracks, listOf("fra", "eng"), null)?.key)
        assertNull(pickSubtitle(tracks, listOf("eng"), ""))                       // the user chose Off
        assertEquals(tracks[1].key, pickSubtitle(tracks, listOf("eng"), tracks[1].key)?.key)
        assertEquals("zho", subtitleLangKey("CHT")); assertEquals("deu", subtitleLangKey("ger"))
        val plain = svc(2, "X", sub(5, "ger", hi = true))
        assertEquals(1, plain.subtitleTracks.size)
    }
}
