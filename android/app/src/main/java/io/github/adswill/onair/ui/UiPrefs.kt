// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.doublePreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.emptyPreferences
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.core.stringSetPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import io.github.adswill.onair.TypeLook
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.model.SourceKind
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.io.IOException

/** A network radio the user added (rtl_tcp or SpyServer), shown in the radio picker. */
data class NetworkRadio(val kind: SourceKind, val name: String, val host: String, val port: Int) {
    val id: String get() = "${kind.code}|$host|$port"
    fun encode(): String = listOf(kind.code.toString(), name.replace('|', '/').replace(';', ','), host, port.toString()).joinToString("|")

    companion object {
        fun decode(s: String): NetworkRadio? {
            val f = s.split('|')
            if (f.size != 4) return null
            val kind = SourceKind.of(f[0].toIntOrNull() ?: return null)
            if (!kind.isNetwork) return null
            return NetworkRadio(kind, f[1], f[2], f[3].toIntOrNull()?.takeIf { it in 1..65535 } ?: return null)
        }
        fun decodeAll(s: String): List<NetworkRadio> = s.split(";;").mapNotNull { if (it.isBlank()) null else decode(it) }
        fun encodeAll(l: List<NetworkRadio>): String = l.joinToString(";;") { it.encode() }
    }
}

/**
 * Preferences of the interface only (not of the receiver): favourites, hidden modes, tuning style, the first-run state, the look.
 * Kept apart from [io.github.adswill.onair.settings.Settings] so the UI layer can grow without touching the receiver's settings;
 * stored in its own DataStore file (datastore/ui.preferences_pb). Nothing leaves the phone.
 */
data class UiPrefs(
    /** Mode indices (Modes.all) marked with a star: shown first in the mode picker. */
    val favourites: Set<Int> = emptySet(),
    /** Mode indices the user does not use; hidden from the picker while [hideUnused] is on. */
    val hidden: Set<Int> = emptySet(),
    val hideUnused: Boolean = false,
    /** Only modes the chosen radio can run (sample rate high enough). */
    val onlyCapable: Boolean = false,
    /** The desktop's tuning toggle: true = a tap centres the radio on the touched frequency, false = normal (the view stays). */
    val centreTuning: Boolean = false,
    /** Step of the frequency buttons in Hz; 0 = from the mode's channel. */
    val stepHz: Double = 0.0,
    val onboardingDone: Boolean = false,
    /** Look for rtl_tcp / SpyServer servers on the local network (mDNS). Off until the user turns it on. */
    val discoveryOptIn: Boolean = false,
    val typeLook: TypeLook = TypeLook.CURRENT,
    /** Animations off, whatever the system says. The system's "remove animations" also turns them off. */
    val reduceMotion: Boolean = false,
    /** Picking a mode starts receiving (one tap). */
    val autoStart: Boolean = true,
    /** The one-time hint about Android's battery optimisation was shown. */
    val batteryHintShown: Boolean = false,
    /** Follow the system's dark/light setting instead of Settings.dark. */
    val followSystemTheme: Boolean = false,
    val networkRadios: List<NetworkRadio> = emptyList(),
    /** Lesson ids (data/lessons/<id>.json) the user finished. */
    val lessonsDone: Set<String> = emptySet(),
)

private val Context.uiStore: DataStore<Preferences> by preferencesDataStore(name = "ui")

/** Holds [UiPrefs] in memory (authoritative while the process runs) and writes them to DataStore in the background. */
class UiPrefsStore(ctx: Context, private val scope: CoroutineScope) {
    private val store = ctx.applicationContext.uiStore
    private val _prefs = MutableStateFlow(UiPrefs())
    val prefs: StateFlow<UiPrefs> = _prefs.asStateFlow()
    private val _loaded = MutableStateFlow(false)
    /** False until the stored preferences were read: the first-run screen waits for it, so it never flashes. */
    val loaded: StateFlow<Boolean> = _loaded.asStateFlow()
    private val dirty = Channel<Unit>(Channel.CONFLATED)

    private object K {
        val fav = stringSetPreferencesKey("fav")
        val hidden = stringSetPreferencesKey("hidden")
        val hideUnused = booleanPreferencesKey("hideUnused")
        val onlyCapable = booleanPreferencesKey("onlyCapable")
        val centre = booleanPreferencesKey("centreTuning")
        val step = doublePreferencesKey("stepHz")
        val onboarding = booleanPreferencesKey("onboardingDone")
        val discovery = booleanPreferencesKey("discovery")
        val look = intPreferencesKey("typeLook")
        val motion = booleanPreferencesKey("reduceMotion")
        val autoStart = booleanPreferencesKey("autoStart")
        val batteryHint = booleanPreferencesKey("batteryHintShown")
        val system = booleanPreferencesKey("followSystemTheme")
        val radios = stringPreferencesKey("netRadios")
        val lessons = stringSetPreferencesKey("lessonsDone")
    }

    init {
        scope.launch {
            val p = try { store.data.catch { e -> if (e is IOException) emit(emptyPreferences()) else throw e }.first() } catch (e: Exception) {
                AppLog.e("settings", "ui preferences unreadable, using defaults", e); emptyPreferences()
            }
            val stored = read(p)
            // a change made before the read finished wins over the stored value of that field only if it was the default before
            _prefs.update { now -> if (now == UiPrefs()) stored else now }
            _loaded.value = true
            for (u in dirty) write(_prefs.value)
        }
    }

    private fun read(p: Preferences) = UiPrefs(
        favourites = p[K.fav].orEmpty().mapNotNull { it.toIntOrNull() }.toSet(),
        hidden = p[K.hidden].orEmpty().mapNotNull { it.toIntOrNull() }.toSet(),
        hideUnused = p[K.hideUnused] ?: false,
        onlyCapable = p[K.onlyCapable] ?: false,
        centreTuning = p[K.centre] ?: false,
        stepHz = p[K.step] ?: 0.0,
        onboardingDone = p[K.onboarding] ?: false,
        discoveryOptIn = p[K.discovery] ?: false,
        typeLook = TypeLook.entries.getOrElse(p[K.look] ?: 0) { TypeLook.CURRENT },
        reduceMotion = p[K.motion] ?: false,
        autoStart = p[K.autoStart] ?: true,
        batteryHintShown = p[K.batteryHint] ?: false,
        followSystemTheme = p[K.system] ?: false,
        networkRadios = NetworkRadio.decodeAll(p[K.radios].orEmpty()),
        lessonsDone = p[K.lessons].orEmpty(),
    )

    /** Changes the preferences now (the UI sees it in the same frame) and on disk soon. */
    fun update(block: (UiPrefs) -> UiPrefs) {
        _prefs.update(block)
        dirty.trySend(Unit)
    }

    private suspend fun write(v: UiPrefs) {
        run {
            try {
                store.edit { e ->
                    e[K.fav] = v.favourites.map { it.toString() }.toSet()
                    e[K.hidden] = v.hidden.map { it.toString() }.toSet()
                    e[K.hideUnused] = v.hideUnused
                    e[K.onlyCapable] = v.onlyCapable
                    e[K.centre] = v.centreTuning
                    e[K.step] = v.stepHz
                    e[K.onboarding] = v.onboardingDone
                    e[K.discovery] = v.discoveryOptIn
                    e[K.look] = v.typeLook.ordinal
                    e[K.motion] = v.reduceMotion
                    e[K.autoStart] = v.autoStart
                    e[K.batteryHint] = v.batteryHintShown
                    e[K.system] = v.followSystemTheme
                    e[K.radios] = NetworkRadio.encodeAll(v.networkRadios)
                    e[K.lessons] = v.lessonsDone
                }
            } catch (e: Exception) { AppLog.e("settings", "ui preferences not saved", e) }
        }
    }
}
