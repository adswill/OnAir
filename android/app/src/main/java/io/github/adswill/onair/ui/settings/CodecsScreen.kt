// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.settings

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.Label
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.LocalTextFamily
import io.github.adswill.onair.Mono
import io.github.adswill.onair.PanelButton
import io.github.adswill.onair.R
import io.github.adswill.onair.data.CodecEntry
import io.github.adswill.onair.ui.ReceiverViewModel

/**
 * Settings > Codecs: every optional codec and plug-in OnAir can load, what needs it, whether a library is installed (file and ABI), and Import / Remove.
 * Import takes a .so or a .zip of them; each file is checked by the core (name on the list, arm64-v8a ELF), then shown with its SHA-256 and a warning
 * before anything is copied. Nothing is downloaded and no codec is bundled (docs/CODECS.md, "Codecs on Android").
 */
@Composable
fun CodecsScreen(vm: ReceiverViewModel, modifier: Modifier = Modifier) {
    val p = LocalPalette.current
    val list by vm.codecList.collectAsStateWithLifecycle()
    val imp by vm.codecImport.collectAsStateWithLifecycle()
    LaunchedEffect(Unit) { vm.refreshCodecList() }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri -> if (uri != null) vm.importCodec(uri) }

    LazyColumn(modifier.fillMaxSize().background(p.window).padding(horizontal = 12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        item {
            Spacer(Modifier.height(8.dp))
            Label(stringResource(R.string.codecs_intro), size = 13, color = p.text)
            Spacer(Modifier.height(6.dp))
            PanelButton(stringResource(R.string.codecs_import)) { pick.launch(arrayOf("*/*")) }
            if (imp.message.isNotEmpty()) Label(stringResource(R.string.codecs_done, imp.message), size = 12, color = p.ok)
            if (imp.pending.isEmpty()) for ((f, why) in imp.refused) Label(stringResource(R.string.codecs_refused, if (f.isEmpty()) why else "$f: $why"), size = 12, color = p.warn)
        }
        items(list, key = { it.id }) { c -> CodecRow(c, vm) }
        item { Spacer(Modifier.height(24.dp)) }
    }

    if (imp.pending.isNotEmpty()) {
        AlertDialog(
            onDismissRequest = { vm.cancelCodecImport() },
            title = { Text(stringResource(R.string.codecs_confirm_title)) },
            text = {
                SelectionContainer {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        for (f in imp.pending) {
                            Column {
                                Text(f.file, fontFamily = Mono, fontSize = 14.sp)
                                Text(stringResource(R.string.codecs_file_abi, "${f.name}, ${f.size} B", f.abi), fontSize = 12.sp)
                                Text(stringResource(R.string.codecs_sha, f.sha256), fontFamily = Mono, fontSize = 11.sp)
                            }
                        }
                        for ((f, why) in imp.refused) Text(stringResource(R.string.codecs_refused, "$f: $why"), fontSize = 12.sp)
                        Text(stringResource(R.string.codecs_warning), fontSize = 13.sp)
                    }
                }
            },
            confirmButton = { TextButton(onClick = { vm.confirmCodecImport() }) { Text(stringResource(R.string.codecs_confirm_yes)) } },
            dismissButton = { TextButton(onClick = { vm.cancelCodecImport() }) { Text(stringResource(R.string.codecs_cancel)) } },
        )
    }
}

@Composable
private fun CodecRow(c: CodecEntry, vm: ReceiverViewModel) {
    val p = LocalPalette.current
    Column(Modifier.fillMaxWidth().background(p.panel).padding(12.dp), verticalArrangement = Arrangement.spacedBy(3.dp)) {
        Text(c.name, color = p.text, fontFamily = LocalTextFamily.current, fontSize = 14.sp)
        Text(
            stringResource(if (c.installed) R.string.codecs_installed else R.string.codecs_not_installed),
            color = if (c.installed) p.ok else p.warn, fontFamily = Mono, fontSize = 12.sp,
        )
        Label(stringResource(R.string.codecs_needed_for, c.usedFor), size = 12)
        if (c.installed) Label(stringResource(R.string.codecs_file_abi, c.installedFile, c.abi), size = 11)
        Label(stringResource(R.string.codecs_names, c.files), size = 11)
        Label(stringResource(R.string.codecs_where, c.source), size = 11)
        if (c.installed) {
            Spacer(Modifier.height(2.dp))
            PanelButton(stringResource(R.string.codecs_remove)) { vm.removeCodecFile(c.installedFile) }
            Label(stringResource(R.string.codecs_removed_note), size = 11)
        }
    }
}
