package io.github.adswill.onair.ui.onboarding

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.snap
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringArrayResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.Chip
import io.github.adswill.onair.Label
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.LocalReducedMotion
import io.github.adswill.onair.LocalTextFamily
import io.github.adswill.onair.Modes
import io.github.adswill.onair.OnAirIcons
import io.github.adswill.onair.PanelButton
import io.github.adswill.onair.R
import io.github.adswill.onair.SectionLabel
import io.github.adswill.onair.TypeScale
import io.github.adswill.onair.groupAccent
import io.github.adswill.onair.model.SourceKind
import io.github.adswill.onair.ui.AppPermission
import io.github.adswill.onair.ui.Permissions
import io.github.adswill.onair.ui.ReceiverViewModel
import io.github.adswill.onair.ui.modes.groupName
import io.github.adswill.onair.ui.nav.DeepLink
import io.github.adswill.onair.ui.radio.AddNetworkRadioDialog
import io.github.adswill.onair.ui.radio.SourceRow
import io.github.adswill.onair.ui.rememberPermissionRequest
import io.github.adswill.onair.ui.UiHooks
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private const val STEPS = 5

/**
 * The first-run guide, mirroring the desktop's setup wizard (app/ui/wizard.cpp): welcome, which radio (with the permissions it
 * needs and the add-network-radio dialog), what to receive (becomes favourites and the start mode), the lessons (data/lessons/NAME.json,
 * when the build has any) and "all set". Every step can be skipped; Settings > Run the setup again brings it back.
 */
@Composable
fun OnboardingScreen(vm: ReceiverViewModel, pickFile: () -> Unit, onDone: () -> Unit, modifier: Modifier = Modifier) {
    val p = LocalPalette.current
    val ctx = LocalContext.current
    val s by vm.settings.collectAsStateWithLifecycle()
    val ui by vm.ui.collectAsStateWithLifecycle()
    var step by rememberSaveable { mutableIntStateOf(0) }
    var interests by rememberSaveable { mutableStateOf(emptyList<Int>()) }
    var adding by rememberSaveable { mutableStateOf(false) }
    var openLesson by rememberSaveable { mutableStateOf<String?>(null) }
    val ask = rememberPermissionRequest(ctx)
    var notifOk by rememberSaveable { mutableStateOf(!Permissions.needsRequest(ctx, AppPermission.NOTIFICATIONS)) }
    val lessons by produceState(emptyList<Lesson>()) { value = withContext(Dispatchers.IO) { Lessons.loadAll(ctx) } }
    val names = stringArrayResource(R.array.mode_names).toList()
    val subIds = Modes.all.map { it.subId }
    val reduced = LocalReducedMotion.current

    fun finish() {
        if (interests.isNotEmpty()) {
            val firsts = interests.mapNotNull { g -> Modes.all.firstOrNull { it.group == g }?.index }
            firsts.forEach { if (it !in ui.favourites) vm.toggleFavourite(it) }
            firsts.firstOrNull()?.let { vm.selectMode(it) }
        }
        vm.finishOnboarding()
        onDone()
    }

    val lesson = lessons.firstOrNull { it.id == openLesson }
    if (lesson != null) {
        LessonViewer(lesson, onTry = { st ->
            val idx = st.mode?.let { DeepLink.resolveMode(it, names, subIds) }
            vm.tuneTo(idx, st.freq?.let { DeepLink.parseFreqMhz(it) })
        }, onClose = { vm.markLesson(lesson.id); openLesson = null }, modifier = modifier)
        return
    }

    Column(modifier.fillMaxSize().background(p.window).padding(16.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(OnAirIcons.Glyph, contentDescription = null, tint = p.accent, modifier = Modifier.size(28.dp))
            Text(
                stringResource(R.string.setup_title, step + 1, STEPS), Modifier.padding(start = 10.dp).weight(1f).semantics { heading() },
                color = p.accent, fontFamily = LocalTextFamily.current, fontSize = TypeScale.TITLE.sp, fontWeight = FontWeight.SemiBold,
            )
            Chip(stringResource(R.string.skip), color = p.muted) { finish() }
        }
        Spacer(Modifier.height(12.dp))
        AnimatedContent(
            targetState = step, modifier = Modifier.weight(1f).fillMaxWidth(), label = "setup",
            transitionSpec = { if (reduced) fadeIn(snap()) togetherWith fadeOut(snap()) else fadeIn() togetherWith fadeOut() },
        ) { k ->
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).widthIn(max = 640.dp)) {
                when (k) {
                    0 -> {
                        Body(stringResource(R.string.setup_welcome))
                        Spacer(Modifier.height(8.dp))
                        Body(stringResource(R.string.setup_privacy), muted = true)
                    }
                    1 -> {
                        Body(stringResource(R.string.setup_radio_question))
                        Spacer(Modifier.height(8.dp))
                        SourceRow(OnAirIcons.Wave, stringResource(R.string.source_synth), stringResource(R.string.source_synth_short), s.source == SourceKind.TEST_SIGNAL) { vm.chooseSource(SourceKind.TEST_SIGNAL) }
                        val usb = UiHooks.usb
                        SourceRow(OnAirIcons.Usb, stringResource(R.string.source_usb), stringResource(if (usb == null) R.string.usb_not_in_build else R.string.usb_setup_hint), false) { usb?.refresh() }
                        SourceRow(OnAirIcons.Network, stringResource(R.string.add_network_radio), stringResource(R.string.setup_network_hint), s.source.isNetwork) { adding = true }
                        SourceRow(OnAirIcons.File, stringResource(R.string.source_file), stringResource(R.string.file_hint), s.source == SourceKind.IQ_FILE) { vm.chooseSource(SourceKind.IQ_FILE); pickFile() }
                        SectionLabel(stringResource(R.string.setup_permissions))
                        if (notifOk) Label(stringResource(R.string.setup_notifications_ok))
                        else {
                            Label(stringResource(R.string.setup_notifications_why))
                            Spacer(Modifier.height(4.dp))
                            PanelButton(stringResource(R.string.setup_allow_notifications)) { ask(listOf(AppPermission.NOTIFICATIONS)) { r -> notifOk = r.values.all { it } } }
                        }
                        Spacer(Modifier.height(4.dp))
                        Label(stringResource(R.string.setup_usb_permission))
                    }
                    2 -> {
                        Body(stringResource(R.string.setup_interests))
                        Spacer(Modifier.height(8.dp))
                        FlowChips(interests) { g -> interests = if (g in interests) interests - g else interests + g }
                    }
                    3 -> {
                        Body(stringResource(if (lessons.isEmpty()) R.string.setup_no_lessons else R.string.setup_lessons))
                        Spacer(Modifier.height(8.dp))
                        for (l in lessons) SourceRow(
                            if (l.id in ui.lessonsDone) OnAirIcons.Check else OnAirIcons.Info, l.title,
                            l.summary.ifBlank { stringResource(R.string.lesson_pages, l.steps.size) }, false,
                        ) { openLesson = l.id }
                    }
                    else -> {
                        Body(stringResource(R.string.setup_done))
                        Spacer(Modifier.height(8.dp))
                        Body(stringResource(R.string.setup_done_hint), muted = true)
                    }
                }
            }
        }
        Row(Modifier.fillMaxWidth().padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            if (step > 0) PanelButton(stringResource(R.string.back)) { step-- }
            Spacer(Modifier.weight(1f))
            val last = step == STEPS - 1
            PanelButton(stringResource(if (last) R.string.finish else R.string.next), on = true) { if (last) finish() else step++ }
        }
    }
    if (adding) AddNetworkRadioDialog(ui.discoveryOptIn, vm::setDiscovery, onDismiss = { adding = false }) { r -> vm.addNetworkRadio(r); vm.useNetworkRadio(r); adding = false }
}

@Composable
private fun Body(text: String, muted: Boolean = false) {
    val p = LocalPalette.current
    Text(text, color = if (muted) p.muted else p.text, fontFamily = LocalTextFamily.current, fontSize = TypeScale.BODY_LARGE.sp)
}

/** The mode groups as toggle chips, wrapped by hand in rows of two (FlowRow is still experimental in this Compose version). */
@Composable
private fun FlowChips(selected: List<Int>, onToggle: (Int) -> Unit) {
    val groups = (0 until Modes.GROUPS).toList()
    for (row in groups.chunked(2)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            for (g in row) Box(Modifier.weight(1f)) {
                Chip(groupName(g), Modifier.fillMaxWidth(), color = groupAccent(g), selected = g in selected) { onToggle(g) }
            }
            if (row.size == 1) Spacer(Modifier.weight(1f))
        }
    }
}

/** A lesson, page by page, with "Try it" where the page names a mode or frequency. */
@Composable
fun LessonViewer(lesson: Lesson, onTry: (LessonStep) -> Unit, onClose: () -> Unit, modifier: Modifier = Modifier) {
    val p = LocalPalette.current
    var page by rememberSaveable(lesson.id) { mutableIntStateOf(0) }
    val st = lesson.steps.getOrNull(page)
    Column(modifier.fillMaxSize().background(p.window).padding(16.dp)) {
        Text(lesson.title, color = p.accent, fontFamily = LocalTextFamily.current, fontSize = TypeScale.TITLE.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.semantics { heading() })
        if (lesson.steps.isNotEmpty()) Label(stringResource(R.string.lesson_page_of, page + 1, lesson.steps.size))
        Spacer(Modifier.height(12.dp))
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
            if (st == null) Body(lesson.summary)
            else {
                if (st.title.isNotBlank()) Text(st.title, color = p.text, fontFamily = LocalTextFamily.current, fontSize = TypeScale.TITLE.sp)
                Spacer(Modifier.height(6.dp))
                Body(st.body)
                if (st.mode != null || st.freq != null) {
                    Spacer(Modifier.height(12.dp))
                    PanelButton(stringResource(R.string.lesson_try)) { onTry(st) }
                }
            }
        }
        Row(Modifier.fillMaxWidth().padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            PanelButton(stringResource(R.string.close)) { onClose() }
            Spacer(Modifier.weight(1f))
            if (page > 0) PanelButton(stringResource(R.string.back)) { page-- }
            if (page < lesson.steps.size - 1) PanelButton(stringResource(R.string.next), on = true) { page++ }
            else PanelButton(stringResource(R.string.finish), on = true) { onClose() }
        }
    }
}
