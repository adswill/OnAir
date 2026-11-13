// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// consent-ui.js - the privacy popup. Optional third-party content (aircraft photos from Planespotters) is only requested after the
// visitor says yes. Buttons: Accept (this visit), Always yes (remembered in localStorage), Decline (this visit).
// The "Privacy choices" links (footer and filter panel) reopen it and offer to forget the saved choice.
import { readChoice, saveChoice, allowed, needsPrompt } from "./consent.js";

let session = null;   // "yes" | "no" | null for this page view
const store = () => { try { return window.localStorage; } catch { return null; } };
const stored = () => { const s = store(); return s ? readChoice(s) : null; };

export function photoAllowed() { return allowed(stored(), session); }

export function initConsent() {
  const box = document.getElementById("consent");
  if (!box) return;
  const status = document.getElementById("consentState");
  const forget = document.getElementById("consentForget");
  const show = () => {
    const st = stored();
    status.textContent = st === "always" ? "Saved: always yes." : session === "yes" ? "Accepted for this visit." : session === "no" ? "Declined for this visit." : "";
    forget.hidden = st !== "always";
    box.hidden = false;
    document.getElementById("consentAccept").focus();
  };
  const hide = () => { box.hidden = true; };
  document.getElementById("consentAccept").onclick = () => { session = "yes"; hide(); };
  document.getElementById("consentAlways").onclick = () => { session = "yes"; const s = store(); if (s) saveChoice(s, "always"); hide(); };
  document.getElementById("consentDecline").onclick = () => { session = "no"; hide(); };
  forget.onclick = () => { const s = store(); if (s) saveChoice(s, null); session = null; hide(); };
  for (const a of document.querySelectorAll("[data-privacy]")) a.addEventListener("click", (e) => { e.preventDefault(); show(); });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape" && !box.hidden) { session = session || "no"; hide(); } });
  if (needsPrompt(stored(), session)) show();
}
