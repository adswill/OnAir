// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.diag

import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Structured in-app log: every entry has a time, a level, a tag and a message. It goes to logcat too, and the last [CAPACITY]
 * entries are kept for the Log tab (a [StateFlow] of an immutable list). Thread-safe: any thread may log.
 *
 * Tags in use: "app" (UI, lifecycle), "engine" (receiver thread, JNI calls), "core" (lines from the C++ engine log), "service",
 * "settings", "usb" (later), "mode" (later).
 */
object AppLog {
    enum class Level(val letter: Char, val priority: Int) { DEBUG('D', Log.DEBUG), INFO('I', Log.INFO), WARN('W', Log.WARN), ERROR('E', Log.ERROR) }

    data class Entry(val timeMs: Long, val level: Level, val tag: String, val message: String) {
        fun format(): String = "${stamp.get()!!.format(Date(timeMs))} ${level.letter}/$tag: $message"
    }

    const val CAPACITY = 2000
    private val stamp = ThreadLocal.withInitial { SimpleDateFormat("HH:mm:ss.SSS", Locale.US) }
    private val buf = ArrayDeque<Entry>()
    private val _entries = MutableStateFlow<List<Entry>>(emptyList())
    val entries: StateFlow<List<Entry>> = _entries.asStateFlow()

    fun d(tag: String, msg: String) = add(Level.DEBUG, tag, msg)
    fun i(tag: String, msg: String) = add(Level.INFO, tag, msg)
    fun w(tag: String, msg: String, t: Throwable? = null) = add(Level.WARN, tag, if (t == null) msg else "$msg: ${t.message ?: t.javaClass.simpleName}")
    fun e(tag: String, msg: String, t: Throwable? = null) = add(Level.ERROR, tag, if (t == null) msg else "$msg: ${t.message ?: t.javaClass.simpleName}")

    fun add(level: Level, tag: String, msg: String) {
        Log.println(level.priority, "OnAir/$tag", msg)
        addAll(listOf(Entry(System.currentTimeMillis(), level, tag, msg)), toLogcat = false)
    }

    /** Several entries at once (one list update), e.g. a batch of lines from the C++ log. */
    fun addAll(items: List<Entry>, toLogcat: Boolean = true) {
        if (items.isEmpty()) return
        if (toLogcat) items.forEach { Log.println(it.level.priority, "OnAir/${it.tag}", it.message) }
        synchronized(buf) {
            buf.addAll(items)
            while (buf.size > CAPACITY) buf.removeFirst()
            _entries.value = buf.toList()
        }
    }

    fun clear() = synchronized(buf) { buf.clear(); _entries.value = emptyList() }

    /** The whole log as text, for sharing. */
    fun dump(): String = entries.value.joinToString("\n") { it.format() }
}
