package io.github.adswill.onair

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.addPathNodes
import androidx.compose.ui.unit.dp

/**
 * OnAir's own line icons: 24-unit grid, 1.8-unit strokes with round caps, the thin-rule look of the desktop's icon set
 * (app/ui/icons.h). Drawn black and tinted by Icon(), so they follow the palette. No icon font, nothing downloaded.
 */
object OnAirIcons {
    private fun icon(name: String, vararg strokes: String, fills: List<String> = emptyList()): ImageVector {
        val b = ImageVector.Builder(name, 24.dp, 24.dp, 24f, 24f)
        for (d in strokes) b.addPath(
            addPathNodes(d), stroke = SolidColor(Color.Black), strokeLineWidth = 1.8f, strokeLineCap = StrokeCap.Round,
            strokeLineJoin = StrokeJoin.Round,
        )
        for (d in fills) b.addPath(addPathNodes(d), fill = SolidColor(Color.Black))
        return b.build()
    }

    /** The OnAir glyph (the launcher icon): a dot with two pairs of arcs. */
    val Glyph = icon(
        "onair",
        "M7.2 7.6A6.6 6.6 0 0 0 7.2 16.4", "M16.8 7.6A6.6 6.6 0 0 1 16.8 16.4",
        "M4.4 4.8A10.6 10.6 0 0 0 4.4 19.2", "M19.6 4.8A10.6 10.6 0 0 1 19.6 19.2",
        fills = listOf("M12 12m-2.2 0a2.2 2.2 0 1 1 4.4 0a2.2 2.2 0 1 1 -4.4 0"),
    )
    val Play = icon("play", fills = listOf("M8 5.5L19 12L8 18.5Z"))
    val Stop = icon("stop", fills = listOf("M6.5 6.5H17.5V17.5H6.5Z"))
    val Record = icon("record", fills = listOf("M12 12m-6 0a6 6 0 1 1 12 0a6 6 0 1 1 -12 0"))
    val Plus = icon("plus", "M12 5V19", "M5 12H19")
    val Minus = icon("minus", "M5 12H19")
    val ChevronDown = icon("chevron_down", "M6 9L12 15L18 9")
    val ChevronUp = icon("chevron_up", "M6 15L12 9L18 15")
    val ChevronRight = icon("chevron_right", "M9 6L15 12L9 18")
    val Back = icon("back", "M15 5L8 12L15 19")
    val Close = icon("close", "M6 6L18 18", "M18 6L6 18")
    val Check = icon("check", "M5 12.5L10 17.5L19 7")
    val Search = icon("search", "M10.5 10.5m-6 0a6 6 0 1 0 12 0a6 6 0 1 0 -12 0", "M15 15L20 20")
    val Star = icon("star", fills = listOf("M12 3.5L14.6 9L20.5 9.6L16 13.6L17.3 19.5L12 16.5L6.7 19.5L8 13.6L3.5 9.6L9.4 9Z"))
    val StarOutline = icon("star_outline", "M12 3.5L14.6 9L20.5 9.6L16 13.6L17.3 19.5L12 16.5L6.7 19.5L8 13.6L3.5 9.6L9.4 9Z")
    val Speaker = icon("speaker", "M4 9.5H8L13 5.5V18.5L8 14.5H4Z", "M16 9A4 4 0 0 1 16 15", "M18.5 6.5A7.5 7.5 0 0 1 18.5 17.5")
    val SpeakerOff = icon("speaker_off", "M4 9.5H8L13 5.5V18.5L8 14.5H4Z", "M16 9.5L21 14.5", "M21 9.5L16 14.5")
    val Squelch = icon("squelch", "M3 15H7L9 9L12 18L15 6L17 15H21", "M3 20H21")
    val Target = icon("target", "M12 12m-7 0a7 7 0 1 0 14 0a7 7 0 1 0 -14 0", "M12 2V6", "M12 18V22", "M2 12H6", "M18 12H22")
    /** Spectrum: the receiver screen. */
    val Spectrum = icon("spectrum", "M3 19H21", "M3 16L6 14L8 15L10 6L12 13L14 11L16 15L18 13L21 14")
    /** Modes: a small tree of rows. */
    val Modes = icon("modes", "M4 6H8", "M11 6H20", "M4 12H8", "M11 12H20", "M4 18H8", "M11 18H20")
    /** Radio: an antenna on a box. */
    val Radio = icon("radio", "M4 11H20V19H4Z", "M8 11L16 4", "M8 15H8.01", "M12 15H17")
    /** More: three dots (overflow menu). */
    val More = icon("more", "M12 5H12.01", "M12 12H12.01", "M12 19H12.01")
    val Settings = icon(
        "settings", "M12 12m-3 0a3 3 0 1 0 6 0a3 3 0 1 0 -6 0",
        "M12 3V5.5", "M12 18.5V21", "M3 12H5.5", "M18.5 12H21", "M5.6 5.6L7.4 7.4", "M16.6 16.6L18.4 18.4", "M5.6 18.4L7.4 16.6", "M16.6 7.4L18.4 5.6",
    )
    val Panels = icon("panels", "M4 5H20V19H4Z", "M12 5V19", "M14.5 9H17.5", "M14.5 12.5H17.5")
    val Log = icon("log", "M5 4H19V20H5Z", "M8 8H16", "M8 12H16", "M8 16H13")
    val Info = icon("info", "M12 12m-9 0a9 9 0 1 0 18 0a9 9 0 1 0 -18 0", "M12 11V16.5", "M12 7.5V7.6")
    val Share = icon("share", "M12 15V4", "M8 8L12 4L16 8", "M5 13V20H19V13")
    val Network = icon("network", "M3.5 9A12 12 0 0 1 20.5 9", "M6.5 12.5A7.5 7.5 0 0 1 17.5 12.5", "M9.5 16A3 3 0 0 1 14.5 16", "M12 19.5V19.6")
    val Usb = icon("usb", "M12 3V18", "M9 6L12 3L15 6", "M12 13L7.5 10.5V8.5", "M12 15.5L16.5 13V11", "M12 20m-1.6 0a1.6 1.6 0 1 0 3.2 0a1.6 1.6 0 1 0 -3.2 0")
    val File = icon("file", "M6 3H14L18 7V21H6Z", "M14 3V7H18")
    val Wave = icon("wave", "M2 12C4 6 6 6 8 12S12 18 14 12S18 6 22 12")
    val Edit = icon("edit", "M4 20H8L19 9L15 5L4 16Z", "M13 7L17 11")
    val Heart = icon("heart", "M12 20S4 14.5 4 9A4 4 0 0 1 12 7A4 4 0 0 1 20 9C20 14.5 12 20 12 20Z")
    val Hide = icon("hide", "M3 12S6.5 6 12 6S21 12 21 12S17.5 18 12 18S3 12 3 12Z", "M4 20L20 4")
}
