// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.panels

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.LocalSpacing
import io.github.adswill.onair.Mono
import io.github.adswill.onair.R

/** The search field shared by the list panels: a 48 dp instrument-style field with a hint. */
@Composable
fun PanelSearch(value: String, onChange: (String) -> Unit, modifier: Modifier = Modifier) {
    val p = LocalPalette.current
    val label = stringResource(R.string.panel_search)
    Box(
        modifier.fillMaxWidth().heightIn(min = LocalSpacing.current.touch).background(p.field, RoundedCornerShape(3.dp))
            .border(1.dp, p.border, RoundedCornerShape(3.dp)).padding(horizontal = 10.dp).semantics { contentDescription = label },
        contentAlignment = Alignment.CenterStart,
    ) {
        BasicTextField(
            value, onChange, singleLine = true,
            textStyle = TextStyle(color = p.text, fontFamily = Mono, fontSize = 13.sp), cursorBrush = SolidColor(p.accent),
            modifier = Modifier.fillMaxWidth(),
        )
        if (value.isEmpty()) Text(label, color = p.muted, fontFamily = Mono, fontSize = 13.sp)
    }
}
