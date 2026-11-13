// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.modes

import io.github.adswill.onair.ModeDef

/** A row of the mode picker. */
sealed interface PickerRow {
    val key: String
    /** A group header; [group] = -1 for the favourites. */
    data class Header(val group: Int, val count: Int, val collapsed: Boolean) : PickerRow { override val key get() = "h$group" }
    data class Mode(val def: ModeDef, val inFavourites: Boolean) : PickerRow { override val key get() = (if (inFavourites) "f" else "m") + def.index }
}

/** The options that decide what the picker shows. */
data class PickerFilter(
    val query: String = "",
    val favourites: Set<Int> = emptySet(),
    val hidden: Set<Int> = emptySet(),
    val hideUnused: Boolean = false,
    val onlyCapable: Boolean = false,
    val collapsed: Set<Int> = emptySet(),
)

/**
 * The picker's rows, as a pure function (unit tested): favourites first, then every group of the desktop tree in its order.
 * A search shows every match expanded; hidden modes stay out while [PickerFilter.hideUnused] is on (except the selected one);
 * [capable] says whether the chosen radio can run a mode.
 */
object ModeFilter {
    fun rows(
        modes: List<ModeDef>, groups: Int, names: List<String>, blurbs: List<String>, f: PickerFilter, selected: Int,
        capable: (ModeDef) -> Boolean,
    ): List<PickerRow> {
        val q = f.query.trim()
        fun matches(m: ModeDef): Boolean = q.isEmpty() || names.getOrElse(m.index) { "" }.contains(q, true) || blurbs.getOrElse(m.index) { "" }.contains(q, true)
        fun visible(m: ModeDef): Boolean {
            if (m.index == selected) return matches(m)
            if (f.hideUnused && m.index in f.hidden) return false
            if (f.onlyCapable && !capable(m)) return false
            return matches(m)
        }
        val out = ArrayList<PickerRow>()
        val favs = modes.filter { it.index in f.favourites && visible(it) }
        if (favs.isNotEmpty()) {
            val collapsed = q.isEmpty() && -1 in f.collapsed
            out.add(PickerRow.Header(-1, favs.size, collapsed))
            if (!collapsed) favs.forEach { out.add(PickerRow.Mode(it, true)) }
        }
        for (g in 0 until groups) {
            val ms = modes.filter { it.group == g && visible(it) }
            if (ms.isEmpty()) continue
            val collapsed = q.isEmpty() && g in f.collapsed
            out.add(PickerRow.Header(g, ms.size, collapsed))
            if (!collapsed) ms.forEach { out.add(PickerRow.Mode(it, false)) }
        }
        return out
    }
}
