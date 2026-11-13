// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.nav

import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.setValue

/**
 * The app's destinations. Top-level ones are in the bottom bar / rail; the others are pushed on top of the current one.
 * [PANELS] is a top-level destination on phones only: on a tablet the panels sit next to the spectrum.
 */
enum class Dest(val route: String, val topLevel: Boolean) {
    RECEIVER("receiver", true),
    PANELS("panels", true),
    MODES("modes", true),
    RADIO("radio", true),
    SETTINGS("settings", true),
    LOG("log", false),
    ABOUT("about", false),
    ONBOARDING("onboarding", false);

    companion object {
        fun of(route: String): Dest? = entries.firstOrNull { it.route == route }
    }
}

/**
 * Back-stack rules, as pure functions (unit tested): the receiver is always at the bottom; choosing a top-level destination replaces
 * everything above the receiver (Material's rule: back from any tab goes home, then out); other destinations are pushed, never twice
 * in a row.
 */
object BackStack {
    fun navigate(stack: List<Dest>, d: Dest): List<Dest> = when {
        d == Dest.RECEIVER -> listOf(Dest.RECEIVER)
        d.topLevel -> listOf(Dest.RECEIVER, d)
        stack.lastOrNull() == d -> stack
        else -> stack + d
    }

    /** The stack after Back, or null when Back should leave the app (only the receiver is left). */
    fun back(stack: List<Dest>): List<Dest>? = if (stack.size <= 1) null else stack.dropLast(1)

    /** The top-level destination that owns [stack] (what the bar highlights). */
    fun selectedTop(stack: List<Dest>): Dest = stack.lastOrNull { it.topLevel } ?: Dest.RECEIVER

    fun sanitize(stack: List<Dest>): List<Dest> {
        val s = stack.filter { it != Dest.ONBOARDING }.ifEmpty { listOf(Dest.RECEIVER) }
        return if (s.first() == Dest.RECEIVER) s else listOf(Dest.RECEIVER) + s
    }
}

/** The navigation state held by the shell: a back stack of [Dest] as snapshot state, saved across re-creation. */
@Stable
class Navigator(initial: List<Dest> = listOf(Dest.RECEIVER)) {
    var stack: List<Dest> by mutableStateOf(BackStack.sanitize(initial))
        private set
    val current: Dest get() = stack.last()
    val selectedTop: Dest get() = BackStack.selectedTop(stack)
    val canGoBack: Boolean get() = stack.size > 1

    fun navigate(d: Dest) { stack = BackStack.navigate(stack, d) }

    /** Returns false when there is nothing to go back to (the system then closes the activity). */
    fun back(): Boolean { val s = BackStack.back(stack) ?: return false; stack = s; return true }

    companion object {
        val Saver: Saver<Navigator, String> = Saver(
            save = { n -> n.stack.joinToString(",") { it.route } },
            restore = { s -> Navigator(s.split(',').mapNotNull { Dest.of(it) }) },
        )
    }
}
