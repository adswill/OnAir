// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui

import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

/** Material's window width classes, from the window size in dp (no window-size-class library needed). */
enum class WidthClass { COMPACT, MEDIUM, EXPANDED }

/**
 * How the shell lays the app out:
 *  - phone portrait (COMPACT): bottom bar; spectrum on top, panels below with a draggable split;
 *  - phone landscape (short height): navigation rail, spectrum left, panels right, the header shrinks;
 *  - tablet, unfolded foldable, desktop window (MEDIUM / EXPANDED): rail, two panes (spectrum left, panels right).
 * A folded foldable is a phone; unfolding it re-measures and switches layout without losing state (all state is hoisted).
 */
@Immutable
data class AppLayout(val width: WidthClass, val twoPane: Boolean, val shortHeight: Boolean) {
    val useRail: Boolean get() = width != WidthClass.COMPACT

    companion object {
        val Phone = AppLayout(WidthClass.COMPACT, twoPane = false, shortHeight = false)
        val PhoneLandscape = AppLayout(WidthClass.MEDIUM, twoPane = true, shortHeight = true)
        val Tablet = AppLayout(WidthClass.EXPANDED, twoPane = true, shortHeight = false)

        fun of(width: Dp, height: Dp): AppLayout {
            val wc = when { width < 600.dp -> WidthClass.COMPACT; width < 840.dp -> WidthClass.MEDIUM; else -> WidthClass.EXPANDED }
            val short = height < 480.dp
            // two panes once there is room for a 360 dp spectrum next to a 300 dp panel column
            val two = wc != WidthClass.COMPACT && (width >= 840.dp || width > height)
            return AppLayout(wc, two, short)
        }
    }
}

val LocalAppLayout = staticCompositionLocalOf { AppLayout.Phone }
