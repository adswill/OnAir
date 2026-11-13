// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Older OnAir versions (0.2.6 and earlier) wrote "squawk":0123 (a number with a leading zero, which is not valid JSON), so the
// whole upload was refused. Quote that one field and parse again; a body that is still not JSON is refused as before.
export function parseIngestBody(text) {
  try {
    return JSON.parse(text);
  } catch {
    return JSON.parse(text.replace(/("squawk"\s*:\s*)(\d+)/g, '$1"$2"'));
  }
}
