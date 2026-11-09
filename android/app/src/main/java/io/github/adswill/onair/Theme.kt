package io.github.adswill.onair

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.remember
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/*
 * The OnAir look on Android: the desktop's palette (app/ui/theme.h pal::, app/ui/ui2_theme.cpp "instrument" palette, the default
 * of the desktop's new interface), its type scale (app/ui/typography.h: 11 12 13 14 16 20 32 px), its 3/4 px corners and dense
 * spacing, as a Material3 theme. android/docs/STYLE.md maps every desktop name to the token used here.
 */

/** The instrument palette. Field names are kept from 0.2.x (ResultsTab, AircraftMap and the mode panels use them). */
@Immutable
class Palette(
    /** ui2 T.bg: the window behind everything. */
    val window: Color,
    /** ui2 T.plotBg / plt PlotBg: the dark plate under the spectrum and waterfall. */
    val plate: Color,
    /** ui2 T.frame: cards, tables, panels. */
    val panel: Color,
    /** ui2 T.edge / ImGuiCol_Border: 1 dp rules. */
    val border: Color,
    /** ImGuiCol_FrameBg: text fields, drop-downs, log. */
    val field: Color,
    /** ui2 T.button. */
    val button: Color,
    /** ui2 T.text. */
    val text: Color,
    /** ui2 T.dim / ImGuiCol_TextDisabled: labels, units, captions. */
    val muted: Color,
    /** pal::accent (the user accent default 0.45 0.72 0.82). */
    val accent: Color,
    /** pal::okGreen, pal::warnAmber, pal::badRed: only where they mean a status. */
    val ok: Color, val warn: Color, val bad: Color,
    /** The spectrum trace (near white, as on the desktop). */
    val trace: Color,
    /** plt Grid: faint grid lines. */
    val grid: Color,
    /** ui2 T.rail: the navigation rail and bottom bar. */
    val rail: Color = window,
    /** ui2 T.bright: the lit digits of the frequency readout. */
    val bright: Color = text,
    /** plt PlotBg border and label text. */
    val plotBorder: Color = border,
    val plotText: Color = muted,
    /** ImGuiCol_ButtonActive / HeaderActive: a pressed or selected control. */
    val selected: Color = accent.copy(alpha = 0.28f),
    /** ui2 T.series: data series in the panels (first = the accent). */
    val series: List<Color> = emptyList(),
    val isDark: Boolean = true,
)

val DarkPalette = Palette(
    window = Color(0xFF2A2B2C), plate = Color(0xFF040405), panel = Color(0xFF18191A), border = Color(0xFF3A3C3E), field = Color(0xFF0E0F10),
    button = Color(0xFF3C3D3F), text = Color(0xFFDBDEE0), muted = Color(0xFF999B9E), accent = Color(0xFF73B8D1),
    ok = Color(0xFF66C285), warn = Color(0xFFE6B347), bad = Color(0xFFE06157), trace = Color(0xFFF2F3F4), grid = Color(0x29999EA3),
    rail = Color(0xFF222324), bright = Color(0xFFFFFFFF), plotBorder = Color(0xFF3D4042), plotText = Color(0xFF9EA3A8),
    selected = Color(0xFF4D758C),
    series = listOf(Color(0xFF73B8D1), Color(0xFF8CCC9E), Color(0xFFE0AD6B), Color(0xFF9EB8EB), Color(0xFFCC94C7), Color(0xFF8C8F94), Color(0xFFE0D68F), Color(0xFF595C61)),
    isDark = true,
)

val LightPalette = Palette(
    window = Color(0xFFE4E6E8), plate = Color(0xFF14171A), panel = Color(0xFFF4F5F6), border = Color(0xFFB8BCC0), field = Color(0xFFFFFFFF),
    button = Color(0xFFD2D5D8), text = Color(0xFF1B1E20), muted = Color(0xFF5E6368), accent = Color(0xFF1F7A99),
    ok = Color(0xFF2E8B57), warn = Color(0xFFB07A00), bad = Color(0xFFC0392B), trace = Color(0xFFE3E5E6), grid = Color(0x29999EA3),
    rail = Color(0xFFD9DCDF), bright = Color(0xFF0B0D0E), plotBorder = Color(0xFF3D4042), plotText = Color(0xFFB4B8BC),
    selected = Color(0xFF9CC7D8),
    series = listOf(Color(0xFF1F7A99), Color(0xFF2E8B57), Color(0xFFB0661A), Color(0xFF3F63B8), Color(0xFF94508F), Color(0xFF5E6368), Color(0xFF8A7A12), Color(0xFF8C9095)),
    isDark = false,
)

val LocalPalette = staticCompositionLocalOf { DarkPalette }

/** The desktop's three font looks (View > Font look): Current (monospace everywhere, the default), Clean, Compact. */
enum class TypeLook { CURRENT, CLEAN, COMPACT }

/** JetBrains Mono (OFL 1.1, third_party/fonts): numbers, the frequency readout and, in the Current look, all text. */
val JetBrainsMono = FontFamily(
    Font(R.font.jetbrains_mono_regular, FontWeight.Normal),
    Font(R.font.jetbrains_mono_bold, FontWeight.Bold),
)

/** Inter (OFL 1.1, third_party/fonts): interface text in the Clean and Compact looks. */
val Inter = FontFamily(
    Font(R.font.inter_regular, FontWeight.Normal),
    Font(R.font.inter_medium, FontWeight.Medium),
    Font(R.font.inter_semibold, FontWeight.SemiBold),
)

/** The monospace family. Kept as `Mono` because the 0.2.x screens and the mode panels use it. */
val Mono: FontFamily = JetBrainsMono

/** The desktop's size scale (typo::kSizes): Caption, Small, Body, BodyLarge, Title, Display, Hero. */
object TypeScale {
    const val CAPTION = 12
    const val SMALL = 13
    const val BODY = 14
    const val BODY_LARGE = 16
    const val TITLE = 18
    const val DISPLAY = 20
    const val HERO = 32
}

/** Spacing in dp, from the desktop style (ItemSpacing 6x3, WindowPadding 8x6, FramePadding 6x2), and the touch minimum. */
@Immutable
data class Spacing(
    val xxs: Dp = 2.dp, val xs: Dp = 3.dp, val s: Dp = 4.dp, val m: Dp = 6.dp, val l: Dp = 8.dp, val xl: Dp = 12.dp, val xxl: Dp = 16.dp,
    /** Material's minimum touch target; every tappable thing is at least this big. */
    val touch: Dp = 48.dp,
)

val LocalSpacing = staticCompositionLocalOf { Spacing() }

/** True when animations should be skipped: the system's "remove animations" or the app's own setting. */
val LocalReducedMotion = staticCompositionLocalOf { false }

/** The text family of the current look (Mono in Current, Inter otherwise). */
val LocalTextFamily = staticCompositionLocalOf<FontFamily> { JetBrainsMono }

/** Desktop corner radii: FrameRounding 3, ChildRounding/PopupRounding 4; sheets and dialogs a little rounder. */
val OnAirShapes = Shapes(
    extraSmall = RoundedCornerShape(3.dp),
    small = RoundedCornerShape(4.dp),
    medium = RoundedCornerShape(6.dp),
    large = RoundedCornerShape(8.dp),
    extraLarge = RoundedCornerShape(12.dp),
)

private fun scheme(p: Palette) =
    (if (p.isDark) darkColorScheme() else lightColorScheme()).copy(
        primary = p.accent, onPrimary = p.plate, primaryContainer = p.selected, onPrimaryContainer = p.text,
        secondary = p.accent, onSecondary = p.plate, secondaryContainer = p.button, onSecondaryContainer = p.text,
        tertiary = p.ok, onTertiary = p.plate,
        background = p.window, onBackground = p.text, surface = p.window, onSurface = p.text,
        surfaceVariant = p.panel, onSurfaceVariant = p.muted, surfaceContainerLowest = p.field, surfaceContainerLow = p.panel,
        surfaceContainer = p.rail, surfaceContainerHigh = p.button, surfaceContainerHighest = p.button,
        inverseSurface = p.text, inverseOnSurface = p.window,
        outline = p.border, outlineVariant = p.border, error = p.bad, onError = p.plate, scrim = Color(0x99000000),
    )

fun onairTypography(p: Palette, look: TypeLook): Typography {
    val text = if (look == TypeLook.CURRENT) JetBrainsMono else Inter
    val d = if (look == TypeLook.COMPACT) -1 else 0
    fun s(size: Int, family: FontFamily = text, weight: FontWeight = FontWeight.Normal, color: Color = p.text) =
        TextStyle(fontFamily = family, fontSize = (size + d).sp, fontWeight = weight, color = color, lineHeight = ((size + d) * 1.35f).sp)
    val semi = if (look == TypeLook.CURRENT) FontWeight.Bold else FontWeight.SemiBold
    val medium = if (look == TypeLook.CURRENT) FontWeight.Normal else FontWeight.Medium
    return Typography(
        displayLarge = s(TypeScale.HERO + 8, JetBrainsMono), displayMedium = s(TypeScale.HERO + 4, JetBrainsMono),
        displaySmall = s(TypeScale.HERO, JetBrainsMono),
        headlineLarge = s(24, weight = semi), headlineMedium = s(22, weight = semi), headlineSmall = s(TypeScale.DISPLAY, weight = semi),
        titleLarge = s(TypeScale.DISPLAY, weight = semi), titleMedium = s(TypeScale.TITLE, weight = semi), titleSmall = s(TypeScale.BODY_LARGE, weight = medium),
        bodyLarge = s(TypeScale.BODY_LARGE), bodyMedium = s(TypeScale.BODY), bodySmall = s(TypeScale.SMALL, color = p.muted),
        labelLarge = s(TypeScale.BODY, weight = medium), labelMedium = s(TypeScale.SMALL, weight = medium), labelSmall = s(TypeScale.CAPTION, color = p.muted),
    )
}

/**
 * The theme. [dark] picks the palette (dark is the default, as on the desktop); [look] the font look; [reducedMotion] turns
 * animations off everywhere that reads [LocalReducedMotion].
 */
@Composable
fun OnAirTheme(dark: Boolean = true, look: TypeLook = TypeLook.CURRENT, reducedMotion: Boolean = false, content: @Composable () -> Unit) {
    val p = if (dark) DarkPalette else LightPalette
    val type = remember(p, look) { onairTypography(p, look) }
    CompositionLocalProvider(
        LocalPalette provides p,
        LocalSpacing provides Spacing(),
        LocalReducedMotion provides reducedMotion,
        LocalTextFamily provides (if (look == TypeLook.CURRENT) JetBrainsMono else Inter),
    ) {
        MaterialTheme(colorScheme = scheme(p), typography = type, shapes = OnAirShapes, content = content)
    }
}

/** The accent of a mode group (Modes.groupColors), as the desktop's mode tree and the new interface's per-mode accent. */
fun groupAccent(group: Int): Color = Color(Modes.groupColors.getOrElse(group) { 0xFF73B8D1 })
