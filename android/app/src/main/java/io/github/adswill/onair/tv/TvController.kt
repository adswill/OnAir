// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.tv

import android.view.Surface
import io.github.adswill.onair.diag.AppLog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.Locale

/** What the TV panel draws. */
data class TvUiState(
    val visible: Boolean = false,
    val libraryLoaded: Boolean = true,
    val mux: TvMux = TvMux.empty,
    val services: List<TvService> = emptyList(),
    val selectedId: Int? = null,
    val audioPid: Int? = null,
    val notes: List<String> = emptyList(),
    val playback: TvPlaybackStats = TvPlaybackStats(),
) {
    val selected: TvService? get() = services.firstOrNull { it.id == selectedId }
}

/**
 * The TV screen's state, one per process (AppGraph.tv): polls the core's service list while the TV panel is on screen, picks the
 * service and sound track ([pickService], [pickAudio]), and runs the [TvPlayer] on the panel's Surface. Leaving the panel stops the
 * picture and the sound (TV in the background is not offered: it would cost the battery a phone cannot spare).
 */
class TvController(private val catalog: DecoderCatalog = AndroidDecoderCatalog()) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default + kotlinx.coroutines.CoroutineName("onair-tv-ui"))
    private val _state = MutableStateFlow(TvUiState(libraryLoaded = TvNative.loaded))
    val state: StateFlow<TvUiState> = _state.asStateFlow()

    private val player = TvPlayer(catalog) { st -> _state.update { it.copy(playback = st) } }
    private var poll: Job? = null
    private var surface: Surface? = null
    private var userPickedId: Int? = null
    private var userAudioPid: Int? = null
    private var playing: TvPlayer.Selection? = null
    private var missingSince = 0L

    /** The languages for the sound track, from the phone's settings (ISO 639-2: "eng", "deu" ...). */
    private val languages: List<String> = try {
        listOfNotNull(Locale.getDefault().isO3Language.takeIf { it.isNotBlank() }, "eng")
    } catch (_: Exception) { listOf("eng") }

    fun setVisible(on: Boolean) {
        _state.update { it.copy(visible = on) }
        if (on) {
            if (poll?.isActive == true) return
            poll = scope.launch {
                while (isActive) {
                    refresh()
                    delay(1000)
                }
            }
        } else {
            poll?.cancel(); poll = null
            stopPlayback()
        }
    }

    fun selectService(id: Int) {
        userPickedId = id; userAudioPid = null
        scope.launch { refresh() }
    }

    fun selectAudio(pid: Int) {
        userAudioPid = pid
        scope.launch { refresh() }
    }

    /** From the panel's SurfaceHolder callbacks (main thread); a null surface returns once the player let go of the old one. */
    fun setSurface(s: Surface?) {
        surface = s
        player.setSurface(s)
    }

    /**
     * A SurfaceView going away. Only the one in use lets the player go: when the picture moves between the panel and full screen, the new
     * Surface may arrive before the old one is destroyed.
     */
    fun releaseSurface(s: Surface?) { if (s == null || s === surface) setSurface(null) }

    fun setVolume(v: Float, mute: Boolean) { player.volume = v.coerceIn(0f, 1f); player.muted = mute }

    private fun stopPlayback() {
        playing = null
        player.stop()
        _state.update { it.copy(playback = TvPlaybackStats()) }
    }

    private fun refresh() {
        if (!TvNative.loaded) { _state.update { it.copy(libraryLoaded = false) }; return }
        val mux = try { TvMux.parse(TvNative.nativeTvServices()) } catch (e: Throwable) { AppLog.e("tv", "service list failed", e); return }
        val list = listedServices(mux)
        val chosen = pickService(list, userPickedId ?: playing?.service?.id, catalog)
        val audio = chosen?.let { pickAudio(it, catalog, languages, userAudioPid) }
        val notes = chosen?.let { playabilityNotes(it, audio, catalog) } ?: emptyList()
        _state.update { it.copy(mux = mux, services = list, selectedId = chosen?.id, audioPid = audio?.pid, notes = notes) }
        if (!_state.value.visible) return
        if (chosen == null) {
            // the service went away (another channel tuned, the receiver stopped): stop after a few seconds, not at the first gap
            if (playing != null) {
                val now = System.currentTimeMillis()
                if (missingSince == 0L) missingSince = now else if (now - missingSince > 4000) { stopPlayback(); missingSince = 0 }
            }
            return
        }
        missingSince = 0
        val sel = TvPlayer.Selection(chosen, audio)
        val p = playing
        if (p == null || p.service.id != chosen.id || p.audio?.pid != audio?.pid || streamsChanged(p.service, chosen)) {
            playing = sel
            player.start(sel, surface)
        }
    }

    /** The PMT changed the PIDs or codecs of the picture: start again with the new ones. */
    private fun streamsChanged(a: TvService, b: TvService): Boolean =
        a.video?.pid != b.video?.pid || a.video?.streamType != b.video?.streamType || a.pcrPid != b.pcrPid
}
