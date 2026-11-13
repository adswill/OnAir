// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// viewport.js - pure helpers for the phone layout (no DOM): how tall the track sheet is in each state and
// where the map should keep the selected marker so the sheet does not cover it.

export const PEEK_FRACTION = 0.31;   // default sheet height as a share of the viewport (28-34%)
export const FULL_FRACTION = 0.72;   // expanded sheet cap
const PEEK_MIN = 150, PEEK_MAX = 300;

/** Height in px of the collapsed 'peek' sheet. */
export function peekHeight(vh) {
  const v = Number(vh) || 0;
  return Math.round(Math.min(PEEK_MAX, Math.max(PEEK_MIN, v * PEEK_FRACTION), v * 0.5));
}

/** Largest height the expanded sheet may take. */
export function fullMaxHeight(vh) {
  const v = Number(vh) || 0;
  return Math.round(v * (v < 500 ? 0.6 : FULL_FRACTION));   // landscape phones keep more map
}

/** Free map rectangle above the sheet: {top, bottom, height, centerY}. `topInset` is the top bar, `sheetH` the sheet height. */
export function freeArea(vh, topInset, sheetH) {
  const top = Math.max(0, topInset || 0);
  const bottom = Math.max(top, vh - Math.max(0, sheetH || 0));
  return { top, bottom, height: bottom - top, centerY: (top + bottom) / 2 };
}

/** Is a marker at screen y inside the free area with `margin` px to spare? */
export function isClear(y, area, margin = 24) {
  return y >= area.top + margin && y <= area.bottom - margin;
}

/** panBy offset [dx, dy] that moves a marker at screen (x, y) to the middle of the free area (horizontally to the middle of `vw`). */
export function centreOffset(x, y, vw, area) {
  return [Math.round(x - vw / 2) + 0, Math.round(y - area.centerY) + 0];
}

/** Next sheet state after releasing a drag: dy is pixels moved (negative = up); velocity ignored for simplicity. */
export function settleState(current, dy, threshold = 36) {
  if (dy <= -threshold) return "full";
  if (dy >= threshold) return "peek";
  return current;
}
