package io.github.adswill.onair.tv

/*
 * The analogue TV modes on the phone (docs/ANDROID-PLAYER-4.1.md): Analog TV (PAL / SECAM / NTSC), MAC and FPV video carry no transport
 * stream, so the Watch panel shows the pictures the core's receivers already make for the desktop (native/src/jni_tv.cpp nativeAnalogFrame),
 * not MediaCodec output. Pure Kotlin (AnalogTvTest); AnalogTvController draws them.
 */

/** Which receiver of the core makes the pictures (the `which` of nativeAnalogFrame). */
enum class AnalogSource(val code: Int, val label: String) { ANALOG_TV(0, "Analog TV"), MAC(1, "MAC"), FPV(2, "FPV video") }

/** The desktop family numbers (app/ui/mode_table.cpp) of the analogue TV modes. */
object AnalogFamilies {
    const val ANALOG_TV = 8
    const val MAC = 47
    const val FPV = 101
}

/** The picture source of a mode family, or null for a mode without analogue pictures (the digital TV modes use [TvPlayer]). */
fun analogSourceOf(family: Int): AnalogSource? = when (family) {
    AnalogFamilies.ANALOG_TV -> AnalogSource.ANALOG_TV
    AnalogFamilies.MAC -> AnalogSource.MAC
    AnalogFamilies.FPV -> AnalogSource.FPV
    else -> null
}

/** One picture's facts, from nativeAnalogFrame's meta array {seq, width, height, colour, untuned, field rate}. */
data class AnalogFrameInfo(val seq: Long, val width: Int, val height: Int, val colour: Boolean, val untuned: Boolean, val fieldRate: Double) {
    val bytes: Int get() = width * height * 4
    val lines: Int get() = when { height >= 560 -> 625; height >= 460 -> 525; else -> 0 }

    companion object {
        /** Null when the meta holds no picture (width or height 0, too short, not finite). */
        fun from(meta: DoubleArray): AnalogFrameInfo? {
            if (meta.size < 6 || meta.any { !it.isFinite() }) return null
            val w = meta[1].toInt(); val h = meta[2].toInt()
            if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return null
            return AnalogFrameInfo(meta[0].toLong(), w, h, meta[3] != 0.0, meta[4] != 0.0, meta[5])
        }
    }
}

/**
 * The shape to draw a picture in. The receivers make square pixels: 768 x 576 and 640 x 480 are 4:3, MAC's 1024 x 576 is 16:9. Clamped to
 * 1 .. 2.4 so a wrong size never makes a sliver.
 */
fun analogAspect(width: Int, height: Int): Float = if (width <= 0 || height <= 0) 4f / 3f else (width.toFloat() / height).coerceIn(1f, 2.4f)

/** The line under the picture: "PAL 625 lines, colour, 50 fields/s" style, from what the picture says. */
fun analogLine(src: AnalogSource, i: AnalogFrameInfo?): String {
    if (i == null) return "${src.label}: waiting for a picture"
    val parts = ArrayList<String>()
    parts += "${src.label} ${i.width}x${i.height}"
    if (i.lines > 0) parts += "${i.lines} lines"
    parts += if (i.colour) "colour" else "black and white"
    if (i.fieldRate > 0) parts += "%.2f fields/s".format(java.util.Locale.ROOT, i.fieldRate).replace(".00 ", " ")
    if (i.untuned) parts += "no signal (snow)"
    return parts.joinToString(", ")
}

/**
 * How often to ask for a picture: a little faster than the signal's frame rate (half the field rate) so none is missed, at most 30 per
 * second (a phone gains nothing above that), at least 5 while nothing is known.
 */
fun analogPollMs(fieldRate: Double): Long {
    if (!(fieldRate > 0)) return 200
    val fps = (fieldRate / 2 * 1.2).coerceIn(5.0, 30.0)
    return (1000.0 / fps).toLong().coerceAtLeast(33)
}
