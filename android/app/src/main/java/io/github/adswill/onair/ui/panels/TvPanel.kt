package io.github.adswill.onair.ui.panels

import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.AppGraph
import io.github.adswill.onair.Chip
import io.github.adswill.onair.Label
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.Mono
import io.github.adswill.onair.R
import io.github.adswill.onair.SectionLabel
import io.github.adswill.onair.tv.TvController
import io.github.adswill.onair.ui.ReceiverViewModel

/**
 * TV picture and sound for the TV modes (DVB-T/T2, DVB-C, DVB-S/S2, ATSC, ATSC 3.0, ISDB-T, DTMB ...): the picture on a SurfaceView,
 * the service list from the core's PSI/SI, the sound tracks, and plain lines on what this phone's decoders cannot play
 * (tv/TvController, tv/TvPlayer). Modes of the TV group that carry no transport stream (analogue TV, MAC) simply never get services.
 */
object TvPanel : ModePanel {
    override val id = "tv.watch"
    /** Before Results: on a TV mode the picture is what people want first. */
    override val order = -1
    @Composable override fun title() = stringResource(R.string.panel_tv)
    override fun appliesTo(ctx: PanelContext) = ctx.mode.group == 0 && ctx.subId == null

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    override fun Content(vm: ReceiverViewModel, ctx: PanelContext, modifier: Modifier) {
        val p = LocalPalette.current
        val tv: TvController = AppGraph.tv
        val st by tv.state.collectAsStateWithLifecycle()
        val settings by vm.settings.collectAsStateWithLifecycle()
        DisposableEffect(Unit) {
            tv.setVisible(true)
            onDispose { tv.setVisible(false) }
        }
        LaunchedEffect(settings.volume, settings.muted) { tv.setVolume(settings.volume, settings.muted) }

        Column(modifier.verticalScroll(rememberScrollState()).padding(10.dp)) {
            val pb = st.playback
            val ratio = if (pb.width > 0 && pb.height > 0) pb.width.toFloat() / pb.height else 16f / 9f
            Box(Modifier.fillMaxWidth().aspectRatio(ratio.coerceIn(1f, 2.4f)).background(Color.Black), contentAlignment = Alignment.Center) {
                AndroidView(
                    factory = { c ->
                        SurfaceView(c).apply {
                            holder.addCallback(object : SurfaceHolder.Callback {
                                override fun surfaceCreated(h: SurfaceHolder) = tv.setSurface(h.surface)
                                override fun surfaceChanged(h: SurfaceHolder, format: Int, w: Int, hh: Int) {}
                                override fun surfaceDestroyed(h: SurfaceHolder) = tv.setSurface(null)
                            })
                        }
                    },
                    modifier = Modifier.fillMaxWidth().aspectRatio(ratio.coerceIn(1f, 2.4f)),
                )
                if (pb.shown == 0L) Text(pb.state.ifBlank { " " }, color = Color(0xFFBBBBBB), fontSize = 13.sp, modifier = Modifier.padding(12.dp))
            }
            Spacer(Modifier.height(6.dp))
            val sel = st.selected
            if (sel != null) {
                Text(sel.title + (if (sel.now.isNotBlank()) "  -  ${sel.now}" else ""), color = p.text, fontSize = 15.sp)
                val line = listOfNotNull(
                    pb.state.takeIf { it.isNotBlank() },
                    pb.videoCodec.takeIf { it.isNotBlank() }?.let { v -> if (pb.width > 0) "$v ${pb.width}x${pb.height}" else v },
                    pb.audioCodec.takeIf { it.isNotBlank() }?.let { a -> if (pb.audioRate > 0) "$a ${pb.audioRate / 1000.0} kHz ${pb.audioChannels} ch" else a },
                ).joinToString("  |  ")
                if (line.isNotBlank()) Text(line, color = p.muted, fontFamily = Mono, fontSize = 12.sp)
                if (pb.shown > 0 || pb.audioFrames > 0) Text(
                    "pictures ${pb.shown}, late/skipped ${pb.dropped}, sound frames ${pb.audioFrames}, sound buffer ${pb.audioBufferMs} ms" +
                        (if (pb.ringDropped > 0) ", lost before the player ${pb.ringDropped}" else "") + (if (pb.ccErrors > 0) ", stream gaps ${pb.ccErrors}" else ""),
                    color = p.muted, fontFamily = Mono, fontSize = 11.sp,
                )
            }
            for (n in (st.notes + pb.notes).distinct()) Text(n, color = p.warn, fontSize = 13.sp, modifier = Modifier.padding(top = 4.dp))

            Spacer(Modifier.height(10.dp))
            when {
                !st.libraryLoaded -> Label(stringResource(R.string.tv_no_library))
                st.mux.packets == 0L -> Label(stringResource(R.string.tv_waiting))
                st.services.isEmpty() -> Label(stringResource(R.string.tv_no_services))
                else -> {
                    SectionLabel(stringResource(R.string.tv_services) + (if (st.mux.network.isNotBlank()) " - ${st.mux.network}" else ""))
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        for (s in st.services) {
                            val tags = listOfNotNull(
                                s.lcn.takeIf { it > 0 }?.toString(),
                                stringResource(R.string.tv_radio).takeIf { s.radio },
                                stringResource(R.string.tv_scrambled).takeIf { s.scrambled },
                            )
                            val label = (if (tags.isEmpty()) "" else tags.joinToString(" ", postfix = "  ")) + s.title
                            Chip(label, color = if (s.scrambled) p.muted else p.accent, selected = s.id == st.selectedId) { tv.selectService(s.id) }
                        }
                    }
                    val audio = sel?.audio.orEmpty()
                    if (audio.size > 1) {
                        Spacer(Modifier.height(8.dp))
                        SectionLabel(stringResource(R.string.tv_sound_tracks))
                        FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            for (a in audio) {
                                val t = listOf(a.lang.ifBlank { "-" }, a.codec).joinToString(" ")
                                Chip(t, color = p.accent, selected = a.pid == st.audioPid) { tv.selectAudio(a.pid) }
                            }
                        }
                    }
                }
            }
            Spacer(Modifier.height(12.dp))
            Text(stringResource(R.string.tv_honesty), color = p.muted, fontSize = 12.sp)
        }
    }
}
