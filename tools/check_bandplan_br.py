#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Check the Brazil band plan (data/bandplans/builtin.json, plan "br") against Anatel's channel plans.

Anatel publishes its Planos Básicos as open data (CC licence), one ZIP with a CSV per plan:
  https://www.anatel.gov.br/dadosabertos/paineis_de_dados/radiodifusao/planos_basicos.zip
  PBFM.csv (FM, eFM and RTR), PBOM.csv (medium wave), PBTVD.csv (digital TV), PBOT.csv, Contorno_Protegido.csv

For FM, MW and TV every row's channel number (and frequency, where the CSV has one) must land on a channel
of the plan; the script prints the rows that do not, the channel range found, and the ZIP's file dates.

  python3 -I tools/check_bandplan_br.py planos_basicos.zip [--json data/bandplans/builtin.json]

TV rows give the nominal channel centre; the plan's ISDB-Tb centres sit 1/7 MHz above it, so TV frequencies are
compared within 200 kHz. Exit status 1 when a row does not fit, a plan file is missing from the ZIP (found by
name, in any folder, any case) or its CSV lacks the channel and frequency columns. Two rows of the 2024-11-01 snapshot are typos in
Anatel's file and are only reported (KNOWN_TYPOS); any other odd row means the plan or the file changed.
Stdlib only."""
import argparse, csv, io, json, os, sys, zipfile

ap = argparse.ArgumentParser()
ap.add_argument('zip')
ap.add_argument('--json', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data', 'bandplans', 'builtin.json'))
ap.add_argument('--plan', default='br')
ap.add_argument('--show', type=int, default=10, help='odd rows to print per file')
a = ap.parse_args()

d = json.load(open(a.json, encoding='utf-8'))
plan = next(p for p in d['plans'] if p['id'] == a.plan)
bands = [b for t in plan['use'] for b in d['templates'][t]['bands']] + plan.get('bands', [])

def channels(service):
    """number (or None) -> centre Hz, for every raster channel of the plan's bands of this service"""
    out = []
    for b in bands:
        if b['service'] != service or not b.get('step_hz'):
            continue
        w = b.get('bandwidth_hz', 0)
        c0 = b.get('first_centre_hz') or b['start_hz'] + w / 2
        k = 0
        while c0 + k * b['step_hz'] + w / 2 <= b['end_hz'] + 5:
            n = b['first_channel'] + k if 'first_channel' in b else None
            out.append((n, c0 + k * b['step_hz'], b['name']))
            k += 1
    return out

def col(header, *starts):
    for s in starts:
        for i, h in enumerate(header):
            if h.lower().startswith(s.lower()):
                return i
    return None

def num(s):
    s = (s or '').strip().replace(',', '.')
    try:
        return float(s)
    except ValueError:
        return None

# (file, channel, MHz, Município-UF) of rows known to be wrong in Anatel's file
KNOWN_TYPOS = {('PBFM.csv', 225, 93.1, 'Castanhal - PA'),         # channel 225 is 92.9 MHz
               ('PBTVD.csv', 23, 533.0, 'Matriz de Camaragibe - AL')}  # channel 23 is 527 MHz
UNIT = {'hz': 1, 'khz': 1e3, 'mhz': 1e6, 'ghz': 1e9}
CHECKS = [('PBFM.csv', 'fm'), ('PBOM.csv', 'am'), ('PBTVD.csv', 'tv')]

z = zipfile.ZipFile(a.zip)
members = {os.path.basename(n).lower(): n for n in z.namelist() if not n.endswith('/')}
bad_total = 0
print(f'{a.zip}')
for info in z.infolist():
    print(f'  {info.filename:28s} {info.file_size:>10d} bytes  dated {"%04d-%02d-%02d %02d:%02d" % info.date_time[:5]}')
for name, service in CHECKS:
    member = members.get(name.lower())
    if member is None:
        print(f'\n{name}: not in the ZIP'); bad_total += 1; continue
    rows = list(csv.reader(io.TextIOWrapper(z.open(member), encoding='utf-8-sig'), delimiter=';'))
    h, rows = (rows[0], rows[1:]) if rows else ([], [])
    ic, iff, iu = col(h, 'Canal'), col(h, 'Frequência', 'Frequencia'), col(h, 'Medida')
    if (ic is None and iff is None) or not rows:
        print(f'\n{name}: no channel or frequency column, or no rows (columns: {";".join(h)})'); bad_total += 1; continue
    im = col(h, 'Município', 'Municipio')
    chans = channels(service)
    by_num = {n: (c, bn) for n, c, bn in chans if n is not None}
    print(f'\n{name}: {len(rows)} rows; plan "{plan["id"]}" has {len(chans)} {service} channels; columns channel={h[ic] if ic is not None else "-"} frequency={h[iff] if iff is not None else "-"}')
    seen, odd, known = {}, [], []
    for r in rows:
        n = num(r[ic]) if ic is not None else None
        f = num(r[iff]) if iff is not None else None
        if f is not None:
            f *= UNIT.get((r[iu] if iu is not None else 'MHz').strip().lower(), 1e6)
        where = r[im] if im is not None else ''
        if n is not None and by_num:
            n = int(n)
            seen[n] = seen.get(n, 0) + 1
            if n not in by_num:
                odd.append(f'channel {n} is not in the plan ({where})')
            elif f is not None and abs(by_num[n][0] - f) > (200e3 if service == 'tv' else 1):
                msg = f'channel {n} listed at {f/1e6:.3f} MHz, plan has {by_num[n][0]/1e6:.3f} MHz ({where})'
                if (name, n, round(f / 1e6, 3), where) in KNOWN_TYPOS:
                    known.append(msg)
                else:
                    odd.append(msg)
        elif f is not None:
            if not any(abs(c - f) < 1 for _, c, _ in chans):
                odd.append(f'{f/1e3:.0f} kHz is not on the plan raster ({where})')
    if seen:
        print(f'  channels in the file: {min(seen)}-{max(seen)}, {len(seen)} distinct')
        names = {}
        for n in seen:
            if n in by_num:
                names[by_num[n][1]] = names.get(by_num[n][1], 0) + seen[n]
        for bn, c in names.items():
            print(f'    {bn}: {c} rows')
    for k in known:
        print('  known typo in the file: ' + k)
    print(f'  rows that do not fit: {len(odd)}')
    for o in odd[:a.show]:
        print('    ' + o)
    bad_total += len(odd)
sys.exit(1 if bad_total else 0)
