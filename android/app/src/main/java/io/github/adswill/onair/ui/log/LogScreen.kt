package io.github.adswill.onair.ui.log

import android.content.ActivityNotFoundException
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.Chip
import io.github.adswill.onair.Label
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.LocalReducedMotion
import io.github.adswill.onair.Mono
import io.github.adswill.onair.OnAirIconButton
import io.github.adswill.onair.OnAirIcons
import io.github.adswill.onair.R
import io.github.adswill.onair.TextBox
import io.github.adswill.onair.TypeScale
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.diag.CrashLog
import io.github.adswill.onair.ui.ReceiverViewModel

/** The log viewer: the structured AppLog (app, engine, core, service ...), level filter, text filter, follow, share and clear. */
@Composable
fun LogScreen(vm: ReceiverViewModel, modifier: Modifier = Modifier) {
    val ctx = LocalContext.current
    val entries by vm.log.collectAsStateWithLifecycle()
    val st by vm.state.collectAsStateWithLifecycle()
    val set by vm.settings.collectAsStateWithLifecycle()
    LogContent(
        entries = entries, error = st.error, crash = remember { CrashLog.hasUnseen(ctx) },
        onShare = {
            try { ctx.startActivity(CrashLog.shareIntent(ctx)) } catch (e: ActivityNotFoundException) { AppLog.w("app", "no app to share with", e) }
            catch (e: Exception) { AppLog.e("app", "share failed", e) }
        },
        onClear = vm::clearLog, onCrashSeen = { CrashLog.markSeen(ctx) }, modifier = modifier,
        verbose = set.verboseLog, onVerbose = vm::setVerbose,
    )
}

@Composable
fun LogContent(
    entries: List<AppLog.Entry>, error: String, crash: Boolean, onShare: () -> Unit, onClear: () -> Unit, onCrashSeen: () -> Unit,
    modifier: Modifier = Modifier, verbose: Boolean = false, onVerbose: ((Boolean) -> Unit)? = null,
) {
    val p = LocalPalette.current
    var minLevel by rememberSaveable { mutableStateOf(AppLog.Level.INFO) }
    var follow by rememberSaveable { mutableStateOf(true) }
    var filter by rememberSaveable { mutableStateOf("") }
    var crashShown by remember { mutableStateOf(crash) }
    val shown = remember(entries, minLevel, filter) {
        entries.filter { it.level >= minLevel && (filter.isBlank() || it.message.contains(filter, true) || it.tag.contains(filter, true)) }
    }
    val list = rememberLazyListState()
    val reduced = LocalReducedMotion.current
    LaunchedEffect(shown.size, follow) {
        if (follow && shown.isNotEmpty()) { if (reduced) list.scrollToItem(shown.size - 1) else list.animateScrollToItem(shown.size - 1) }
    }
    Column(modifier.fillMaxSize().background(p.window).padding(horizontal = 8.dp)) {
        Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            for (l in AppLog.Level.entries) Chip(l.name.lowercase(), color = levelColor(l), selected = l == minLevel) { minLevel = l }
            Chip(stringResource(R.string.log_follow), color = p.accent, selected = follow) { follow = !follow }
            if (onVerbose != null) Chip(stringResource(R.string.log_verbose), color = p.warn, selected = verbose) { onVerbose(!verbose) }
            OnAirIconButton(OnAirIcons.Share, stringResource(R.string.log_share), framed = false, onClick = onShare)
            OnAirIconButton(OnAirIcons.Close, stringResource(R.string.log_clear), framed = false, onClick = onClear)
        }
        TextBox(filter, { filter = it }, Modifier.fillMaxWidth(), placeholder = stringResource(R.string.log_filter))
        if (crashShown) Text(
            stringResource(R.string.log_crash_banner), color = p.bad, fontFamily = Mono, fontSize = TypeScale.SMALL.sp,
            modifier = Modifier.padding(vertical = 6.dp),
        )
        if (crashShown) Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Chip(stringResource(R.string.log_share), color = p.bad) { onShare(); onCrashSeen(); crashShown = false }
            Chip(stringResource(R.string.dismiss), color = p.muted) { onCrashSeen(); crashShown = false }
        }
        if (error.isNotEmpty()) Text(stringResource(R.string.log_error, error), Modifier.padding(top = 6.dp), color = p.bad, fontFamily = Mono, fontSize = TypeScale.SMALL.sp)
        if (shown.isEmpty() && error.isEmpty()) Label(stringResource(R.string.log_empty), Modifier.padding(top = 8.dp))
        SelectionContainer(Modifier.fillMaxSize().padding(top = 6.dp).background(p.field)) {
            LazyColumn(state = list, modifier = Modifier.fillMaxSize().padding(6.dp)) {
                itemsIndexed(shown, key = { i, e -> "${e.timeMs}-$i" }) { _, e ->
                    Text(e.format(), color = levelColor(e.level), fontFamily = Mono, fontSize = TypeScale.CAPTION.sp)
                }
            }
        }
    }
}

@Composable
private fun levelColor(l: AppLog.Level) = LocalPalette.current.let { p ->
    when (l) { AppLog.Level.ERROR -> p.bad; AppLog.Level.WARN -> p.warn; AppLog.Level.DEBUG -> p.muted; else -> p.text }
}
