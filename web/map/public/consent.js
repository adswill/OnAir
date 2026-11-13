// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// consent.js - the visitor's choice about optional third-party content (aircraft photos from Planespotters).
// Nothing optional is requested before a choice is made. "Accept" lasts for this page view, "Always yes" is kept in
// localStorage, "Decline" is this page view only. The footer link "Privacy choices" reopens the popup and can reset it.

export const KEY = "onair.map.consent";

/** Stored choice: "always" or null (undecided or storage unavailable). */
export function readChoice(storage) {
  try { return storage.getItem(KEY) === "always" ? "always" : null; } catch { return null; }
}
export function saveChoice(storage, choice) {
  try {
    if (choice === "always") storage.setItem(KEY, "always"); else storage.removeItem(KEY);
  } catch { /* storage blocked: the choice then lasts for this page view only */ }
}
/** Is optional third-party content allowed, given the stored choice and this page view's answer ("yes" | "no" | null)? */
export function allowed(stored, session) {
  if (session === "yes") return true;
  if (session === "no") return false;
  return stored === "always";
}
/** The popup is shown only while nothing is decided. */
export function needsPrompt(stored, session) { return stored !== "always" && session == null; }
