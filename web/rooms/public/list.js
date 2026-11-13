// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Room list page. Text only through textContent.
const ul = document.getElementById("list"), empty = document.getElementById("empty");
async function load() {
  try {
    const r = await fetch("/api/rooms", { cache: "no-store" });
    const { rooms } = await r.json();
    ul.replaceChildren();
    for (const x of rooms) {
      const li = document.createElement("li"), a = document.createElement("a"), b = document.createElement("b"), s = document.createElement("span");
      a.href = "/r/" + encodeURIComponent(x.id);
      b.textContent = x.title;
      s.className = "sub";
      s.textContent = [x.mode, x.freq ? x.freq + " MHz" : "", x.viewers + " watching"].filter(Boolean).join("  |  ");
      a.append(b, s); li.append(a); ul.append(li);
    }
    empty.hidden = rooms.length > 0;
  } catch { empty.hidden = false; }
}
load(); setInterval(load, 15000);
