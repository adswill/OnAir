// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/*
 * The instrument widgets: flat charcoal faces, 1 dp rules, 3 dp corners (the desktop's FrameRounding), one accent. Every tappable
 * widget is at least 48 dp high (Spacing.touch) and says what it is to TalkBack (role, state). Text sizes are in sp, so they follow
 * the phone's text scale; nothing here has a fixed height that would clip scaled text.
 */

private val Shape = RoundedCornerShape(3.dp)

@Composable
fun Label(text: String, modifier: Modifier = Modifier, color: Color = LocalPalette.current.muted, size: Int = 12) =
    Text(text, modifier, color = color, fontFamily = LocalTextFamily.current, fontSize = size.sp)

/** A small section title (the desktop's separatorText), marked as a heading for TalkBack. */
@Composable
fun SectionLabel(text: String, modifier: Modifier = Modifier) =
    Text(
        text, modifier.padding(top = 10.dp, bottom = 3.dp).semantics { heading() }, color = LocalPalette.current.muted,
        fontFamily = LocalTextFamily.current, fontSize = TypeScale.CAPTION.sp, fontWeight = FontWeight.Medium,
    )

/** A section title with the desktop's coloured bar in front (mode groups, settings groups). */
@Composable
fun GroupHeader(text: String, color: Color, modifier: Modifier = Modifier) {
    Row(modifier.fillMaxWidth().padding(top = 12.dp, bottom = 4.dp).semantics(mergeDescendants = true) { heading() }, verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.width(3.dp).height(14.dp).background(color))
        Text(text, Modifier.padding(start = 8.dp), color = color, fontFamily = LocalTextFamily.current, fontSize = TypeScale.SMALL.sp, fontWeight = FontWeight.Medium)
    }
}

/** A status lamp (the desktop's round LED in the Panel layout). */
@Composable
fun Lamp(color: Color, size: Int = 10) = Box(Modifier.size(size.dp).clip(CircleShape).background(color))

/** A flat instrument button: charcoal face, thin rule; [on] fills it with the accent. 48 dp high. */
@Composable
fun PanelButton(text: String, on: Boolean = false, modifier: Modifier = Modifier, enabled: Boolean = true, onClick: () -> Unit) {
    val p = LocalPalette.current
    Box(
        modifier.heightIn(min = LocalSpacing.current.touch).clip(Shape).background(if (on) p.accent else p.button)
            .border(BorderStroke(1.dp, p.border), Shape)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick).padding(horizontal = 12.dp, vertical = 6.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text, color = if (!enabled) p.muted else if (on) p.plate else p.text, fontFamily = LocalTextFamily.current,
            fontSize = TypeScale.BODY.sp, maxLines = 2, overflow = TextOverflow.Ellipsis,
        )
    }
}

/** A square icon button, 48 dp, with a content description for TalkBack. */
@Composable
fun OnAirIconButton(
    icon: ImageVector, description: String, modifier: Modifier = Modifier, tint: Color = LocalPalette.current.text,
    on: Boolean = false, enabled: Boolean = true, framed: Boolean = true, onClick: () -> Unit,
) {
    val p = LocalPalette.current
    val base = modifier.sizeIn(minWidth = LocalSpacing.current.touch, minHeight = LocalSpacing.current.touch).clip(Shape)
    Box(
        (if (framed) base.background(if (on) p.selected else p.button).border(1.dp, p.border, Shape) else base)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) { Icon(icon, contentDescription = description, tint = if (enabled) tint else p.muted, modifier = Modifier.size(22.dp)) }
}

/** A small rounded chip (mode chip, BW chip, filters). [color] tints the text and the rule. */
@Composable
fun Chip(
    text: String, modifier: Modifier = Modifier, color: Color = LocalPalette.current.text, selected: Boolean = false,
    minHeight: Boolean = true, onClick: (() -> Unit)? = null,
) {
    val p = LocalPalette.current
    var m = modifier
    if (minHeight && onClick != null) m = m.heightIn(min = LocalSpacing.current.touch)
    Box(m, contentAlignment = Alignment.Center) {
        Text(
            text,
            Modifier.clip(Shape).background(if (selected) color.copy(alpha = 0.22f) else p.panel.copy(alpha = 0.9f))
                .border(1.dp, color.copy(alpha = if (selected) 0.9f else 0.55f), Shape)
                .then(if (onClick != null) Modifier.clickable(role = Role.Button, onClick = onClick) else Modifier)
                .padding(horizontal = 8.dp, vertical = 4.dp),
            color = color, fontFamily = LocalTextFamily.current, fontSize = TypeScale.SMALL.sp, maxLines = 1,
        )
    }
}

/** A checkbox row in the instrument look (the desktop's Checkbox). The whole row is the touch target. */
@Composable
fun CheckRow(text: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    val p = LocalPalette.current
    Row(
        Modifier.fillMaxWidth().heightIn(min = LocalSpacing.current.touch).toggleable(checked, role = Role.Checkbox, onValueChange = onChange),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(18.dp).clip(RoundedCornerShape(2.dp)).background(if (checked) p.accent else p.field).border(1.dp, p.border, RoundedCornerShape(2.dp)))
        Spacer(Modifier.width(10.dp))
        Text(text, color = p.text, fontFamily = LocalTextFamily.current, fontSize = TypeScale.BODY.sp)
    }
}

/** An on/off setting with a title and an optional line of help: a Switch on the right, the whole row toggles. */
@Composable
fun SwitchRow(title: String, checked: Boolean, onChange: (Boolean) -> Unit, help: String? = null, enabled: Boolean = true) {
    val p = LocalPalette.current
    Row(
        Modifier.fillMaxWidth().heightIn(min = 56.dp).toggleable(checked, enabled = enabled, role = Role.Switch, onValueChange = onChange)
            .padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f).padding(end = 12.dp)) {
            Text(title, color = if (enabled) p.text else p.muted, fontFamily = LocalTextFamily.current, fontSize = TypeScale.BODY_LARGE.sp)
            if (help != null) Text(help, color = p.muted, fontFamily = LocalTextFamily.current, fontSize = TypeScale.SMALL.sp)
        }
        Switch(
            checked, onCheckedChange = null, enabled = enabled,
            colors = SwitchDefaults.colors(
                checkedThumbColor = p.plate, checkedTrackColor = p.accent, uncheckedThumbColor = p.muted, uncheckedTrackColor = p.field,
                uncheckedBorderColor = p.border,
            ),
        )
    }
}

@Composable
fun TextBox(
    value: String, onValue: (String) -> Unit, modifier: Modifier = Modifier, numeric: Boolean = false, label: String? = null,
    placeholder: String? = null, onDone: (() -> Unit)? = null,
) {
    val p = LocalPalette.current
    Column(modifier) {
        if (label != null) Label(label)
        Box(Modifier.fillMaxWidth().heightIn(min = LocalSpacing.current.touch).clip(Shape).background(p.field).border(1.dp, p.border, Shape).padding(horizontal = 10.dp), contentAlignment = Alignment.CenterStart) {
            if (value.isEmpty() && placeholder != null) Text(placeholder, color = p.muted, fontFamily = LocalTextFamily.current, fontSize = TypeScale.BODY_LARGE.sp)
            BasicTextField(
                value, onValue, singleLine = true,
                textStyle = TextStyle(color = p.text, fontFamily = LocalTextFamily.current, fontSize = TypeScale.BODY_LARGE.sp),
                cursorBrush = SolidColor(p.accent),
                keyboardOptions = KeyboardOptions(keyboardType = if (numeric) KeyboardType.Decimal else KeyboardType.Text, imeAction = if (onDone != null) ImeAction.Done else ImeAction.Default),
                keyboardActions = KeyboardActions(onDone = { onDone?.invoke() }),
                modifier = Modifier.fillMaxWidth().semantics { if (label != null) contentDescription = label },
            )
        }
    }
}

/** A closed drop-down in the instrument look: shows the chosen entry, opens a list. */
@Composable
fun Dropdown(items: List<String>, selected: Int, onSelect: (Int) -> Unit, modifier: Modifier = Modifier, label: String? = null) {
    val p = LocalPalette.current
    var open by remember { mutableStateOf(false) }
    val current = items.getOrElse(selected) { "" }
    Column(modifier) {
        if (label != null) Label(label)
        Box {
            Row(
                Modifier.fillMaxWidth().heightIn(min = LocalSpacing.current.touch).clip(Shape).background(p.field).border(1.dp, p.border, Shape)
                    .clickable(role = Role.DropdownList) { open = true }
                    .semantics { if (label != null) contentDescription = label; stateDescription = current }
                    .padding(horizontal = 10.dp),
                verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                Text(current, color = p.text, fontFamily = LocalTextFamily.current, fontSize = TypeScale.BODY.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                Icon(OnAirIcons.ChevronDown, contentDescription = null, tint = p.muted, modifier = Modifier.size(18.dp))
            }
            DropdownMenu(open, { open = false }) {
                items.forEachIndexed { i, t ->
                    DropdownMenuItem(
                        text = { Text(t, fontFamily = LocalTextFamily.current, fontSize = TypeScale.BODY.sp, color = if (i == selected) p.accent else p.text) },
                        onClick = { open = false; onSelect(i) },
                    )
                }
            }
        }
    }
}

/** A labelled slider. [onChange] moves a local value while dragging; [onDone] commits it when the finger lifts. */
@Composable
fun SliderRow(label: String, value: Float, range: ClosedFloatingPointRange<Float>, valueText: String, onChange: (Float) -> Unit, onDone: () -> Unit = {}, enabled: Boolean = true) {
    val p = LocalPalette.current
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth().heightIn(min = LocalSpacing.current.touch)) {
        Label(label, Modifier.widthIn(min = 56.dp, max = 120.dp).padding(end = 6.dp))
        Slider(
            value, onChange, valueRange = range, onValueChangeFinished = onDone, enabled = enabled,
            modifier = Modifier.weight(1f).semantics { contentDescription = label; stateDescription = valueText },
            colors = SliderDefaults.colors(thumbColor = p.accent, activeTrackColor = p.accent, inactiveTrackColor = p.border),
        )
        Label(valueText, Modifier.widthIn(min = 56.dp).padding(start = 6.dp), color = p.text)
    }
}

@Composable
fun Card(title: String, value: String, modifier: Modifier = Modifier) {
    val p = LocalPalette.current
    Column(modifier.clip(Shape).background(p.panel).border(1.dp, p.border, Shape).padding(horizontal = 10.dp, vertical = 6.dp).semantics(mergeDescendants = true) {}) {
        Label(title, size = 11)
        Text(value, color = p.text, fontFamily = Mono, fontSize = 14.sp)
    }
}

/** A framed group of controls (the desktop's child window with a 1 dp border). */
@Composable
fun Section(modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    val p = LocalPalette.current
    Column(modifier.fillMaxWidth().clip(RoundedCornerShape(4.dp)).background(p.panel).border(1.dp, p.border, RoundedCornerShape(4.dp)).padding(horizontal = 12.dp, vertical = 6.dp)) { content() }
}

/**
 * A level meter bar (the desktop's ADC / signal meters): [fraction] 0..1 filled with [color]; [text] is read by TalkBack and shown at
 * the right.
 */
@Composable
fun MeterBar(label: String, fraction: Float, text: String, color: Color, modifier: Modifier = Modifier) {
    val p = LocalPalette.current
    Row(
        modifier.semantics(mergeDescendants = true) {
            contentDescription = "$label $text"
            progressBarRangeInfo = ProgressBarRangeInfo(fraction.coerceIn(0f, 1f), 0f..1f)
        },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Label(label, size = TypeScale.CAPTION)
        Box(Modifier.padding(horizontal = 6.dp).weight(1f).height(6.dp).clip(RoundedCornerShape(1.dp)).background(p.field).border(1.dp, p.border, RoundedCornerShape(1.dp))) {
            Box(Modifier.fillMaxHeight().fillMaxWidth(fraction.coerceIn(0f, 1f)).background(color))
        }
        Text(text, color = p.text, fontFamily = Mono, fontSize = TypeScale.CAPTION.sp, maxLines = 1)
    }
}

/** A row of [content] that wraps nothing: just horizontal spacing in the desktop's 6 dp. */
@Composable
fun ToolRow(modifier: Modifier = Modifier, content: @Composable RowScope.() -> Unit) =
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically, content = content)
