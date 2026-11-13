// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.about

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.Chip
import io.github.adswill.onair.Label
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.LocalSpacing
import io.github.adswill.onair.LocalTextFamily
import io.github.adswill.onair.Mono
import io.github.adswill.onair.OnAirIcons
import io.github.adswill.onair.PanelButton
import io.github.adswill.onair.R
import io.github.adswill.onair.TypeScale
import io.github.adswill.onair.ui.ReceiverViewModel
import io.github.adswill.onair.ui.radio.appVersion
import io.github.adswill.onair.ui.radio.openUrl
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private class CreditsData(val credits: List<Credit>, val contributors: List<Contribution>, val supporters: List<String>, val ofl: String)

/** About, credits, contributors, supporters and the font licence: the desktop's Help > About and Help > Credits on one screen. */
@Composable
fun AboutScreen(vm: ReceiverViewModel, modifier: Modifier = Modifier) {
    val ctx = LocalContext.current
    val st by vm.state.collectAsStateWithLifecycle()
    val data by produceState<CreditsData?>(null) {
        value = withContext(Dispatchers.IO) { CreditsData(Credits.credits(ctx), Credits.contributors(ctx), Credits.supporters(ctx), Credits.ofl(ctx)) }
    }
    AboutContent(appVersion(st.coreVersion), data?.credits.orEmpty(), data?.contributors.orEmpty(), data?.supporters.orEmpty(), data?.ofl.orEmpty(),
        onLink = { openUrl(ctx, it) }, modifier = modifier)
}

@Composable
fun AboutContent(
    version: String, credits: List<Credit>, contributors: List<Contribution>, supporters: List<String>, ofl: String,
    onLink: (String) -> Unit, modifier: Modifier = Modifier,
) {
    val p = LocalPalette.current
    var tab by rememberSaveable { mutableIntStateOf(0) }
    val tabs = listOf(R.string.about_title, R.string.credits_title, R.string.contributors_title, R.string.supporters_title, R.string.licences_title)
    Column(modifier.fillMaxSize().background(p.window)) {
        Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 8.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            tabs.forEachIndexed { i, t -> Chip(stringResource(t), color = if (i == tab) p.accent else p.muted, selected = i == tab) { tab = i } }
        }
        when (tab) {
            0 -> Column(Modifier.fillMaxWidth().padding(16.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                Icon(OnAirIcons.Glyph, contentDescription = null, tint = p.accent, modifier = Modifier.size(72.dp))
                Text("OnAir", color = p.text, fontFamily = LocalTextFamily.current, fontSize = TypeScale.HERO.sp, fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.semantics { heading() })
                Text(version, color = p.muted, fontFamily = Mono, fontSize = TypeScale.BODY.sp)
                Spacer(Modifier.height(12.dp))
                Text(stringResource(R.string.about_text, version), color = p.text, fontFamily = LocalTextFamily.current, fontSize = TypeScale.BODY_LARGE.sp)
                Spacer(Modifier.height(12.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    PanelButton(stringResource(R.string.about_source)) { onLink("https://github.com/adswill/OnAir") }
                    PanelButton(stringResource(R.string.about_support)) { onLink("https://ko-fi.com/adswill") }
                }
            }
            1 -> LazyColumn(Modifier.fillMaxSize().padding(horizontal = 12.dp)) {
                if (credits.isEmpty()) item { Label(stringResource(R.string.credits_missing), Modifier.padding(8.dp)) }
                items(credits) { c -> LinkLine(c.name, c.author + " · " + c.licence, c.url, onLink) }
            }
            2 -> LazyColumn(Modifier.fillMaxSize().padding(horizontal = 12.dp)) {
                item { Label(stringResource(R.string.contributors_intro), Modifier.padding(vertical = 8.dp)) }
                items(contributors) { c -> LinkLine(c.who, c.what + " (" + c.kind + ")", c.link, onLink) }
            }
            3 -> LazyColumn(Modifier.fillMaxSize().padding(horizontal = 12.dp)) {
                item { Label(stringResource(R.string.supporters_intro), Modifier.padding(vertical = 8.dp)) }
                items(supporters) { s -> Text(s, color = p.text, fontFamily = LocalTextFamily.current, fontSize = TypeScale.BODY_LARGE.sp, modifier = Modifier.padding(vertical = 6.dp)) }
            }
            else -> LazyColumn(Modifier.fillMaxSize().padding(horizontal = 12.dp)) {
                item { Label(stringResource(R.string.fonts_licence_intro), Modifier.padding(vertical = 8.dp)) }
                item { Text(ofl.ifBlank { stringResource(R.string.credits_missing) }, color = p.text, fontFamily = Mono, fontSize = TypeScale.CAPTION.sp) }
            }
        }
    }
}

@Composable
private fun LinkLine(title: String, detail: String, url: String, onLink: (String) -> Unit) {
    val p = LocalPalette.current
    Column(
        Modifier.fillMaxWidth().heightIn(min = LocalSpacing.current.touch)
            .then(if (url.startsWith("http")) Modifier.clickable(role = Role.Button) { onLink(url) } else Modifier).padding(vertical = 6.dp),
    ) {
        Text(title, color = if (url.startsWith("http")) p.accent else p.text, fontFamily = LocalTextFamily.current, fontSize = TypeScale.BODY_LARGE.sp)
        Text(detail, color = p.muted, fontFamily = LocalTextFamily.current, fontSize = TypeScale.SMALL.sp)
    }
}
