// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.panels

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.github.adswill.onair.Label
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.Mono
import io.github.adswill.onair.PanelButton
import io.github.adswill.onair.R
import io.github.adswill.onair.SectionLabel
import io.github.adswill.onair.engine.DrmSlide
import io.github.adswill.onair.engine.JlPage
import io.github.adswill.onair.ui.ReceiverViewModel
import kotlinx.coroutines.delay

/**
 * DRM data services (#645), as the desktop shows them: the MOT slide show picture (an image card that follows the newest picture) and
 * Journaline (a menu of pages: tap a link to open it, Back goes up). Pictures are shown, never run or opened; Journaline pages are text.
 */
object DrmPanel : ModePanel {
    override val id = "drm.data"
    override val order = -1
    @Composable override fun title() = stringResource(R.string.panel_drm_data)
    override fun appliesTo(ctx: PanelContext) = ctx.standard == Std.DRM

    @Composable
    override fun Content(vm: ReceiverViewModel, ctx: PanelContext, modifier: Modifier) {
        Column(modifier.verticalScroll(rememberScrollState()).padding(12.dp)) {
            SectionLabel(stringResource(R.string.drm_slideshow))
            SlideCard(vm)
            Spacer(Modifier.height(12.dp))
            SectionLabel(stringResource(R.string.drm_journaline))
            JournalineView(vm)
        }
    }
}

@Composable
private fun SlideCard(vm: ReceiverViewModel) {
    val p = LocalPalette.current
    var slide by remember { mutableStateOf<DrmSlide?>(null) }
    var bitmap by remember { mutableStateOf<Bitmap?>(null) }
    LaunchedEffect(vm) {
        while (true) {
            val s = runCatching { vm.drmSlide(slide?.seq ?: 0L) }.getOrNull()
            if (s != null) { slide = s; bitmap = Bitmap.createBitmap(s.argb, s.w, s.h, Bitmap.Config.ARGB_8888) }
            delay(1000)
        }
    }
    Column(Modifier.fillMaxWidth().background(p.panel).padding(10.dp)) {
        val b = bitmap
        val s = slide
        if (b == null || s == null) Label(stringResource(R.string.drm_no_slide))
        else {
            Image(b.asImageBitmap(), s.caption.ifEmpty { stringResource(R.string.drm_slideshow) }, Modifier.fillMaxWidth().heightIn(max = 360.dp), contentScale = ContentScale.Fit)
            if (s.caption.isNotEmpty()) Text(s.caption, Modifier.padding(top = 6.dp), color = p.text, fontFamily = Mono, fontSize = 13.sp)
            Label(s.type.ifEmpty { "image" } + "  " + s.w + "x" + s.h, size = 11)
        }
    }
}

@Composable
private fun JournalineView(vm: ReceiverViewModel) {
    val p = LocalPalette.current
    // the path of object ids from the root to the page shown (-1 = the root); Back pops it
    var path by remember { mutableStateOf(listOf(-1)) }
    var page by remember { mutableStateOf<JlPage?>(null) }
    val current = path.last()
    LaunchedEffect(vm, current) {
        page = null
        while (true) {
            // pages keep arriving and change: ask again, a page that is not there yet shows the waiting text
            val pg = runCatching { vm.drmJournaline(current) }.getOrNull()
            if (pg != null) page = pg
            delay(1500)
        }
    }
    Column(Modifier.fillMaxWidth().background(p.panel).padding(10.dp)) {
        val pg = page
        if (path.size > 1) PanelButton(stringResource(R.string.drm_back)) { path = path.dropLast(1) }
        if (pg == null) Label(stringResource(R.string.drm_no_journaline))
        else {
            if (pg.title.isNotEmpty()) Text(pg.title, Modifier.padding(vertical = 4.dp), color = p.accent, fontFamily = Mono, fontSize = 15.sp)
            for ((id, text) in pg.links) {
                Text(
                    text.ifEmpty { "#$id" }, Modifier.fillMaxWidth().clickable { path = path + id }.heightIn(min = 44.dp).padding(vertical = 10.dp),
                    color = p.text, fontFamily = Mono, fontSize = 14.sp,
                )
            }
            for (line in pg.lines) Text(line, Modifier.padding(vertical = 2.dp), color = p.text, fontFamily = Mono, fontSize = 14.sp)
        }
    }
}
