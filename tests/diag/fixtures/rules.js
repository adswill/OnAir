// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
// Fixture for tools/lint/diag_check.py --self-test (JS call forms).
export function fixture(res, name) {
  diag(518263, `The badge ${name} was not found, so the page shows the default badge.`);
  const t = diagText(518269, 'Sign in with GitHub first: this badge action needs an account.');
  diag(518267, "Bad request.");   // expect: sentence
  diagText(518269, "This line reuses the code of the second call site and must be reported.");   // expect: dup-code
  return { error: t, code: 518269 };
}
