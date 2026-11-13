// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// sheet.js - phone layout glue: the track card is a bottom sheet with a 'peek' and a 'full' state. While it is open the map's
// bottom padding equals the sheet height, so the selected marker is centred in the free area above it (not hidden under it).
import { peekHeight, fullMaxHeight, settleState } from "./viewport.js";

export function createSheet({ host, map, mq, reduced, getLngLat }) {
  let state = "peek", open = false, dragging = false, lastH = 0, timer = 0;
  const vh = () => (window.visualViewport && window.visualViewport.height) || window.innerHeight;
  const active = () => mq.matches && open && !host.hidden;
  const dur = () => (reduced ? 0 : 250);

  function applySize() {
    host.classList.toggle("peek", active() && state === "peek");
    host.classList.toggle("full", active() && state === "full");
    host.style.setProperty("--full-max", fullMaxHeight(vh()) + "px");
    host.style.height = active() && state === "peek" ? peekHeight(vh()) + "px" : "";
  }

  function recentre(animate = true, extra = null) {
    if (!map) return;
    const bottom = active() ? Math.round(host.getBoundingClientRect().height) : 0;
    lastH = bottom;
    const o = { padding: { top: 0, left: 0, right: 0, bottom }, duration: animate ? dur() : 0, ...(extra || {}) };
    const ll = open && getLngLat ? getLngLat() : null;
    o.center = ll || map.getCenter();   // closing: keep what was at the middle of the free area
    map.easeTo(o);
  }

  function setState(s) { state = s; applySize(); recentre(); }

  function onOpen() {
    if (!open) state = "peek";
    open = true;
    applySize();
    recentre();
  }
  function onClose() {
    const was = active();
    open = false;
    applySize();
    if (was) recentre();
  }

  // drag the handle / header: the sheet follows the finger, then settles to peek or full
  let startY = 0, startH = 0, moved = 0;
  host.addEventListener("pointerdown", (e) => {
    if (!active() || !e.target.closest(".grab, .c-head") || e.target.closest(".x")) return;
    dragging = true; startY = e.clientY; startH = host.getBoundingClientRect().height; moved = 0;
    host.classList.add("dragging");
    try { host.setPointerCapture(e.pointerId); } catch { /* ignore */ }
  });
  host.addEventListener("pointermove", (e) => {
    if (!dragging) return;
    moved = e.clientY - startY;
    const h = Math.max(peekHeight(vh()), Math.min(fullMaxHeight(vh()), startH - moved));
    host.style.height = Math.round(h) + "px";
  });
  const end = (e) => {
    if (!dragging) return;
    dragging = false;
    host.classList.remove("dragging");
    try { host.releasePointerCapture(e.pointerId); } catch { /* ignore */ }
    const next = Math.abs(moved) < 6 ? (state === "peek" ? "full" : "peek") : settleState(state, moved);
    setState(next);
  };
  host.addEventListener("pointerup", end);
  host.addEventListener("pointercancel", end);

  // content grew or shrank (rows appeared, photo loaded): keep the marker clear if the sheet height really changed
  if (window.ResizeObserver) {
    new ResizeObserver(() => {
      if (!active() || dragging) return;
      clearTimeout(timer);
      timer = setTimeout(() => { if (Math.abs(host.getBoundingClientRect().height - lastH) > 8) recentre(); }, 120);
    }).observe(host);
  }

  // rotation, resize, iOS toolbar collapse
  let rt = 0;
  const onView = () => {
    document.documentElement.style.setProperty("--vh", vh() + "px");
    clearTimeout(rt);
    rt = setTimeout(() => {
      map && map.resize();
      applySize();
      if (!dragging) recentre(false);
    }, 80);
  };
  window.addEventListener("resize", onView);
  window.addEventListener("orientationchange", onView);
  if (window.visualViewport) {
    window.visualViewport.addEventListener("resize", onView);
    window.visualViewport.addEventListener("scroll", onView);
  }
  mq.addEventListener && mq.addEventListener("change", () => { applySize(); recentre(false); });
  document.documentElement.style.setProperty("--vh", vh() + "px");
  // belt and braces: if anything (focus, scrollIntoView) scrolls the pinned page, put it back so no background strip shows
  const reset = () => { if (document.body.scrollTop || document.body.scrollLeft) { document.body.scrollTop = 0; document.body.scrollLeft = 0; } if (window.scrollY || window.scrollX) window.scrollTo(0, 0); };
  document.body.addEventListener("scroll", reset, { passive: true });
  window.addEventListener("scroll", reset, { passive: true });

  return { onOpen, onClose, setState, recentre };
}
