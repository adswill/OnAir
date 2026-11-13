package io.github.adswill.onair.tv

import io.github.adswill.onair.update.UpdateLogic

/*
 * The services of the received multiplex (from the core's PAT/PMT/SDT, native/src/jni_tv.cpp nativeTvServices), which streams of a
 * service the phone can play, and which service and sound track to start with. Pure Kotlin (TvServicesTest): the phone's decoder
 * list comes in through [DecoderCatalog], which Android fills from MediaCodecList (AndroidDecoderCatalog).
 */

class TvStream(val pid: Int, val streamType: Int, val codec: String, val kind: String, val lang: String, val kbps: Double, val scrambled: Boolean) {
    val isVideo: Boolean get() = kind == "video"
    val isAudio: Boolean get() = kind == "audio"
}

class TvService(
    val id: Int, val name: String, val provider: String, val type: Int, val pmtPid: Int, val pcrPid: Int,
    val caFlag: Boolean, val scrambled: Boolean, val lcn: Int, val havePmt: Boolean, val now: String, val next: String,
    val streams: List<TvStream>,
) {
    val video: TvStream? get() = streams.firstOrNull { it.isVideo }
    val audio: List<TvStream> get() = streams.filter { it.isAudio }
    /** Shown in lists: the name, else "Service <id>". */
    val title: String get() = name.ifBlank { "Service $id" }
    /** DVB service types 0x02 (digital radio), 0x0A (advanced codec radio), or no video stream at all. */
    val radio: Boolean get() = type == 0x02 || type == 0x0A || (havePmt && video == null && audio.isNotEmpty())
}

class TvMux(val packets: Long, val network: String, val tsid: Int, val services: List<TvService>) {
    companion object {
        val empty = TvMux(0, "", 0, emptyList())

        /** Reads the JSON of nativeTvServices; [empty] when it cannot. */
        fun parse(json: String): TvMux {
            val root = UpdateLogic.parseJson(json) as? Map<*, *> ?: return empty
            fun Map<*, *>.i(k: String) = (this[k] as? Double)?.toInt() ?: 0
            fun Map<*, *>.s(k: String) = this[k] as? String ?: ""
            fun Map<*, *>.b(k: String) = this[k] as? Boolean ?: false
            val list = (root["services"] as? List<*>)?.mapNotNull { sv ->
                val m = sv as? Map<*, *> ?: return@mapNotNull null
                val streams = (m["streams"] as? List<*>)?.mapNotNull { st ->
                    val x = st as? Map<*, *> ?: return@mapNotNull null
                    TvStream(x.i("pid"), x.i("type"), x.s("codec"), x.s("kind"), x.s("lang"), (x["kbps"] as? Double) ?: 0.0, x.b("scrambled"))
                } ?: emptyList()
                TvService(m.i("id"), m.s("name"), m.s("provider"), m.i("type"), m.i("pmt"), m.i("pcr"), m.b("ca"), m.b("scrambled"),
                    m.i("lcn"), m.b("havePmt"), m.s("now"), m.s("next"), streams)
            } ?: emptyList()
            return TvMux(((root["packets"] as? Double) ?: 0.0).toLong(), root.s("network"), root.i("tsid"), list)
        }
    }
}

/** What the phone's decoders are (MediaCodecList on Android, a fixed list in tests). */
fun interface DecoderCatalog {
    /** The name of a decoder for [mime] ("c2.android.avc.decoder"), or null when the phone has none. */
    fun decoderFor(mime: String): String?
}

/** How a stream of the service will be handled on this phone. */
sealed class Playability {
    /** Plays with [decoder]. */
    data class Plays(val codecLabel: String, val mime: String, val decoder: String) : Playability()
    /** The format is one the app handles, but the phone has no decoder for it. */
    data class NoDecoder(val codecLabel: String, val mime: String) : Playability()
    /** The app does not hand this format to a decoder (yet): AVS, DRA, AC-4 ... */
    data class NotHandled(val codecLabel: String) : Playability()
    /** It arrives scrambled (pay TV): nothing can be shown. OnAir does not descramble. */
    object Scrambled : Playability()
}

/** The format the app's framers use for a stream, or null when it has none for it. */
fun videoCodecOf(s: TvStream): VideoCodec? = when {
    s.streamType == 0x1B || s.codec == "H.264" -> VideoCodec.H264
    s.streamType == 0x24 || s.codec == "HEVC" -> VideoCodec.HEVC
    s.streamType == 0x02 || s.codec == "MPEG-2" -> VideoCodec.MPEG2
    s.streamType == 0x01 || s.codec == "MPEG-1 video" -> VideoCodec.MPEG1
    else -> null
}

/** [firstPayload]: the start of the stream's first PES payload when known; it tells ADTS from LATM for an "AAC" in private data (0x06). */
fun audioFormatOf(s: TvStream, firstPayload: ByteArray? = null): AudioFormat? = when {
    s.streamType == 0x0F -> AudioFormat.AAC_ADTS
    s.streamType == 0x11 -> AudioFormat.AAC_LATM
    s.streamType == 0x03 || s.streamType == 0x04 -> AudioFormat.MPEG_AUDIO
    s.streamType == 0x81 || s.codec == "AC-3" -> AudioFormat.AC3
    s.streamType == 0x87 || s.codec == "E-AC-3" -> AudioFormat.EAC3
    s.codec == "AAC" || s.codec == "HE-AAC" -> detectAac(firstPayload) ?: AudioFormat.AAC_ADTS
    else -> null
}

/** ADTS (12 sync bits 0xFFF) or LOAS (11 bits 0x2B7) at the start of [b]; null when neither or unknown. */
fun detectAac(b: ByteArray?): AudioFormat? {
    if (b == null || b.size < 2) return null
    val x = b[0].toInt() and 0xFF; val y = b[1].toInt() and 0xFF
    return when {
        x == 0xFF && (y and 0xF0) == 0xF0 -> AudioFormat.AAC_ADTS
        x == 0x56 && (y and 0xE0) == 0xE0 -> AudioFormat.AAC_LATM
        else -> null
    }
}

/** The decoder name shown for MPEG audio layer II when the phone has none: the app's own (Mp2Decoder). */
const val BUILTIN_MP2 = "OnAir MP2 decoder (built in)"

/** The MIME type the phone's decoder must offer for an audio format (MPEG audio: layer II, by far the common one in broadcasting). */
fun audioMimeOf(f: AudioFormat): String = when (f) {
    AudioFormat.AAC_ADTS, AudioFormat.AAC_LATM -> "audio/mp4a-latm"
    AudioFormat.AC3 -> "audio/ac3"
    AudioFormat.EAC3 -> "audio/eac3"
    AudioFormat.MPEG_AUDIO -> MpegAudioFramer.MIME_MPEG_L2
}

fun videoPlayability(s: TvStream, cat: DecoderCatalog): Playability {
    if (s.scrambled) return Playability.Scrambled
    val c = videoCodecOf(s) ?: return Playability.NotHandled(s.codec.ifBlank { "type 0x%02X".format(s.streamType) })
    val d = cat.decoderFor(c.mime) ?: return Playability.NoDecoder(c.label, c.mime)
    return Playability.Plays(c.label, c.mime, d)
}

fun audioPlayability(s: TvStream, cat: DecoderCatalog, firstPayload: ByteArray? = null): Playability {
    if (s.scrambled) return Playability.Scrambled
    val f = audioFormatOf(s, firstPayload) ?: return Playability.NotHandled(s.codec.ifBlank { "type 0x%02X".format(s.streamType) })
    val mime = audioMimeOf(f)
    // MPEG audio layer II: the phone's decoder when it has one, else the app's own (Mp2Decoder: layer II is free of patents)
    val d = cat.decoderFor(mime) ?: (if (f == AudioFormat.MPEG_AUDIO) BUILTIN_MP2 else null)
        ?: return Playability.NoDecoder(f.label, mime)
    return Playability.Plays(f.label, mime, d)
}

/** Plain-language lines for the TV screen: what will and will not play, and why. Empty when everything plays. */
fun playabilityNotes(service: TvService, audio: TvStream?, cat: DecoderCatalog): List<String> {
    val out = ArrayList<String>()
    if (service.scrambled) {
        out += "${service.title} is scrambled (pay TV). OnAir does not descramble, so it cannot be shown."
        return out
    }
    val v = service.video
    if (v != null) when (val p = videoPlayability(v, cat)) {
        is Playability.NoDecoder -> out += "This phone has no ${p.codecLabel} video decoder, so there is no picture. OnAir uses the phone's own decoders and ships none."
        is Playability.NotHandled -> out += notHandledNote(p.codecLabel, v.streamType, video = true)
        Playability.Scrambled -> out += "The video of ${service.title} is scrambled: no picture."
        is Playability.Plays -> {}
    }
    if (audio == null) {
        if (service.audio.isEmpty()) out += "This service has no sound track." else out += "None of the sound tracks of ${service.title} can be played on this phone."
    } else when (val p = audioPlayability(audio, cat)) {
        is Playability.NoDecoder -> out += "This phone has no ${p.codecLabel} sound decoder, so there is no sound." +
            (if (p.mime == "audio/ac3" || p.mime == "audio/eac3") " Dolby decoders come with some phones only." else "")
        is Playability.NotHandled -> out += notHandledNote(p.codecLabel, audio.streamType, video = false)
        Playability.Scrambled -> out += "The sound of ${service.title} is scrambled: no sound."
        is Playability.Plays -> {}
    }
    return out
}

/**
 * The line for a format the app does not hand to the phone's decoders. The Chinese DTMB formats (AVS, AVS+, AVS2, AVS3 video, DRA sound) get
 * their own words: Android has no decoder for them and OnAir ships none (codecs are user-installed, docs/CODECS.md); on the desktop the
 * user-installed AVS+ / DRA add-ons play them. Everything else (AC-4, MPEG-H ...) says the same in general terms.
 */
fun notHandledNote(codecLabel: String, streamType: Int, video: Boolean): String {
    val c = codecLabel.uppercase()
    val avs = c.startsWith("AVS") || streamType == 0x42 || streamType == 0xD2 || streamType == 0xD4
    val dra = c.startsWith("DRA")
    val what = if (video) "picture" else "sound"
    return when {
        avs && video -> "${if (c.startsWith("AVS")) codecLabel else "AVS/AVS+"} video: Android phones have no decoder for it and OnAir ships none, so there is no picture here. " +
            "On the desktop, OnAir shows it with the AVS+ add-on you install yourself (docs/CODECS.md)."
        dra && !video -> "DRA sound: Android phones have no decoder for it and OnAir ships none, so there is no sound here. " +
            "On the desktop, OnAir plays it with the DRA add-on you install yourself (docs/CODECS.md)."
        else -> "$codecLabel ${if (video) "video" else "sound"} is not handed to the phone's decoders (Android has no decoder for it): no $what."
    }
}

/** The services worth listing: with a PMT and at least one sound or picture stream, by logical channel number, then service id. */
fun listedServices(mux: TvMux): List<TvService> =
    mux.services.filter { it.havePmt && it.streams.any { s -> s.isVideo || s.isAudio } }
        .sortedWith(compareBy<TvService>({ if (it.lcn > 0) 0 else 1 }, { it.lcn }, { it.id }))

/**
 * The service to start with: the one chosen before ([rememberedId]) when it is in the multiplex, else the first clear TV service whose
 * picture this phone can decode, else the first clear TV service, else the first clear radio service, else the first one.
 */
fun pickService(services: List<TvService>, rememberedId: Int?, cat: DecoderCatalog): TvService? {
    if (services.isEmpty()) return null
    rememberedId?.let { id -> services.firstOrNull { it.id == id }?.let { return it } }
    val clear = services.filter { !it.scrambled }
    return clear.firstOrNull { s -> s.video?.let { videoPlayability(it, cat) is Playability.Plays } == true }
        ?: clear.firstOrNull { it.video != null }
        ?: clear.firstOrNull()
        ?: services.first()
}

/**
 * The sound track to play: one the phone can decode first, then the user's languages in order ([languages], ISO 639-2 codes such as
 * "eng", "deu", "ara"), then the order in the PMT. Scrambled tracks last. Null when the service has no sound.
 */
fun pickAudio(service: TvService, cat: DecoderCatalog, languages: List<String> = emptyList(), wantedPid: Int? = null): TvStream? {
    val a = service.audio
    if (a.isEmpty()) return null
    wantedPid?.let { p -> a.firstOrNull { it.pid == p }?.let { return it } }
    fun score(s: TvStream): Int {
        var v = 0
        if (!s.scrambled) v += 1000
        if (audioPlayability(s, cat) is Playability.Plays) v += 100
        val li = languages.indexOfFirst { it.equals(s.lang, ignoreCase = true) }
        if (li >= 0) v += 50 - minOf(li, 40)
        return v
    }
    return a.maxWithOrNull(compareBy<TvStream> { score(it) }.thenByDescending { a.indexOf(it) })
}

/** The PIDs the native ring must keep for a service: its picture, the chosen sound track and the clock (PCR) PID. */
fun pidsFor(service: TvService, audio: TvStream?): List<Int> =
    listOfNotNull(service.video?.pid, audio?.pid, service.pcrPid.takeIf { it in 1..8190 }).distinct()
