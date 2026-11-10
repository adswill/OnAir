package io.github.adswill.onair.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalInspectionMode
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.AppGraph
import io.github.adswill.onair.Dropdown
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.LocalTextFamily
import io.github.adswill.onair.PanelButton
import io.github.adswill.onair.R
import io.github.adswill.onair.SwitchRow
import io.github.adswill.onair.TextBox
import io.github.adswill.onair.ToolRow
import io.github.adswill.onair.TypeScale
import io.github.adswill.onair.settings.Settings
import io.github.adswill.onair.update.UpdateLogic
import io.github.adswill.onair.update.UpdatePhase
import kotlinx.coroutines.launch

/**
 * Settings > Updates (issue #183): the switch for the start-up check, the source (GitHub, GitHub only, mirror only) and the mirror, then
 * the state of the update itself: check, download with progress, install (Android asks the user to confirm).
 */
@Composable
fun UpdateSettings(s: Settings) {
    if (LocalInspectionMode.current) return   // previews have no app graph
    val mgr = AppGraph.updates
    val st by mgr.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val ctx = LocalContext.current
    val p = LocalPalette.current
    fun save(f: (Settings) -> Settings) { scope.launch { AppGraph.settings.update(f) } }
    val sources = listOf(stringResource(R.string.update_src_auto), stringResource(R.string.update_src_github), stringResource(R.string.update_src_mirror))
    val rel = st.release
    Column {
        SwitchRow(stringResource(R.string.update_check), s.updateCheck, { on -> save { x -> x.copy(updateCheck = on) } }, help = stringResource(R.string.update_check_help))
        Dropdown(sources, s.updateSource.coerceIn(0, 2), { i -> save { x -> x.copy(updateSource = i) } }, label = stringResource(R.string.update_source))
        if (s.updateSource == 0) {
            SwitchRow(stringResource(R.string.update_mirror_fallback), s.updateMirrorOn, { on -> save { x -> x.copy(updateMirrorOn = on) } }, help = stringResource(R.string.update_mirror_fallback_help))
        }
        if (s.updateSource == 2 || (s.updateSource == 0 && s.updateMirrorOn)) {
            var mirror by remember { mutableStateOf(s.updateMirror) }   // kept here while typing so the cursor does not jump
            TextBox(mirror, { v -> mirror = v; save { x -> x.copy(updateMirror = v) } }, label = stringResource(R.string.update_mirror_address), placeholder = UpdateLogic.DEFAULT_MIRROR)
        }
        val line = when (st.phase) {
            UpdatePhase.IDLE -> ""
            UpdatePhase.CHECKING -> stringResource(R.string.update_checking)
            UpdatePhase.UP_TO_DATE -> stringResource(R.string.update_up_to_date)
            UpdatePhase.AVAILABLE -> stringResource(R.string.update_available, rel?.version ?: "")
            UpdatePhase.DOWNLOADING -> stringResource(R.string.update_downloading, UpdateLogic.mb(st.done), if (st.total > 0) UpdateLogic.mb(st.total) else "?")
            UpdatePhase.READY -> stringResource(R.string.update_ready, rel?.version ?: "")
            UpdatePhase.INSTALLING -> stringResource(R.string.update_installing)
            UpdatePhase.FAILED -> stringResource(R.string.update_failed, st.error)
        }
        if (line.isNotEmpty()) Text(line + if (st.viaMirror) " (" + stringResource(R.string.update_via_mirror) + ")" else "", Modifier.padding(vertical = 6.dp), color = p.text, fontFamily = LocalTextFamily.current, fontSize = TypeScale.BODY_LARGE.sp)
        if (st.needConfirm) Text(stringResource(R.string.update_mirror_only_note), Modifier.padding(bottom = 6.dp), color = p.muted, fontFamily = LocalTextFamily.current, fontSize = TypeScale.SMALL.sp)
        if (st.needInstallPermission && st.phase == UpdatePhase.READY) Text(stringResource(R.string.update_install_permission), Modifier.padding(bottom = 6.dp), color = p.muted, fontFamily = LocalTextFamily.current, fontSize = TypeScale.SMALL.sp)
        ToolRow {
            when (st.phase) {
                UpdatePhase.AVAILABLE, UpdatePhase.FAILED -> {
                    if (st.phase == UpdatePhase.AVAILABLE && rel != null) {
                        val label = if (st.needConfirm) stringResource(R.string.update_download_anyway) else stringResource(R.string.update_download, UpdateLogic.mb(rel.size))
                        PanelButton(label) { mgr.download(st.needConfirm) }
                    }
                    PanelButton(stringResource(R.string.update_check_now)) { mgr.checkNow() }
                }
                UpdatePhase.DOWNLOADING -> PanelButton(stringResource(R.string.update_cancel)) { mgr.cancel() }
                UpdatePhase.READY -> PanelButton(stringResource(R.string.update_install), on = true) { mgr.install(ctx) }
                UpdatePhase.CHECKING, UpdatePhase.INSTALLING -> {}
                else -> PanelButton(stringResource(R.string.update_check_now)) { mgr.checkNow() }
            }
        }
    }
}
