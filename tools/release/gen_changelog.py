#!/usr/bin/env python3
"""Regenerate docs/CHANGELOG-3.0.md, the README mode table/count and docs/I18N-NEW-STRINGS-3.0.md.

  python3 tools/release/gen_changelog.py [--base pub/integ-30] [--since v0.2.9] [--issues-json FILE] [--check]

Source text: tools/release/changelog-3.0.src.md (read its header for the rules).
 - A bullet ending in [[a,b]] is shown only when every named branch (pub/<name>) is an ancestor of --base;
   otherwise it moves to "Not in this build yet".
 - New modes, the mode count, contributors and the maintainer appendix are computed from git and app/ui/mode_table.cpp.
 - Issue reporters come from `gh issue list` (read only) or --issues-json.
 - Run from the repository root. Nothing is posted anywhere.
"""
import argparse, json, os, re, subprocess, sys, datetime

def git(*a):
    return subprocess.run(['git', *a], capture_output=True, text=True).stdout

def merged(base, br):
    r = subprocess.run(['git', 'merge-base', '--is-ancestor', 'pub/' + br, base], capture_output=True)
    if r.returncode == 128:   # branch missing: treat as not merged
        return False
    return r.returncode == 0

# ---------------------------------------------------------------- mode registry
def parse_modes(text):
    gm = re.search(r'kGroupNames\[kNumGroups\]\s*=\s*\{(.*?)\};', text, re.S)
    groups = [re.sub(r'^N_\("(.*)"\)$', r'\1', g.strip()).strip('"') for g in
              re.findall(r'N_\("[^"]*"\)|"[^"]*"', gm.group(1))] if gm else []
    i = text.index('kBuiltinModes[] = {')
    body = text[i:text.index('\n};', i)]
    rows = []
    for m in re.finditer(r'^\s*\{(\d+),\s*"((?:[^"\\]|\\.)*)",\s*(\d+),', body, re.M):
        rows.append((int(m.group(1)), m.group(2), int(m.group(3))))
    return rows, groups

def mode_info(ref):
    if ref == 'WORKTREE':
        text = open('app/ui/mode_table.cpp').read()
    else:
        text = git('show', ref + ':app/ui/mode_table.cpp')
    return parse_modes(text)

SKIP_FAMILIES = (999, 19999)

def count_modes(rows):
    return len([r for r in rows if r[0] not in SKIP_FAMILIES])

README_GROUPS = [   # README row -> mode_table group names
    ('TV', ['TV']), ('Broadcast radio', ['BROADCAST RADIO']), ('Aviation', ['AVIATION']),
    ('Maritime', ['MARITIME']), ('Satellite and navigation', ['SATELLITE & NAVIGATION']),
    ('Two-way radio', ['TWO-WAY RADIO']), ('Cellular and rail', ['CELLULAR & RAIL']), ('Pagers', ['PAGERS']),
    ('IoT and sensors', ['IoT & SENSORS']), ('Weather and science', ['WEATHER & SCIENCE']),
    ('Amateur', ['AMATEUR - DIGITAL', 'AMATEUR - VOICE & PACKET']), ('HF utility', ['HF UTILITY']),
    ('Tools', ['GENERAL & TOOLS']), ('KA family (own modes)', ['Kris custom modes'])]

def readme_rows(rows, groups):
    out = {}
    for fam, name, g in rows:
        if fam in SKIP_FAMILIES or g >= len(groups):
            continue
        out.setdefault(groups[g], []).append(name)
    return out

# ---------------------------------------------------------------- changelog
def load_issue_authors(path):
    data = None
    if path and os.path.exists(path):
        data = json.load(open(path))
    else:
        try:
            r = subprocess.run(['gh', 'issue', 'list', '--repo', 'adswill/OnAir', '--state', 'all', '--limit', '500',
                                '--json', 'number,author'], capture_output=True, text=True, timeout=60)
            data = json.loads(r.stdout) if r.returncode == 0 else None
        except Exception:
            data = None
    return {i['number']: i['author']['login'] for i in data} if data else {}

def contributors(base, since):
    names = {}
    for line in git('log', since + '..' + base, '--format=%an|%ae').splitlines():
        n, e = line.split('|', 1)
        m = re.match(r'\d+\+([^@]+)@users\.noreply\.github\.com', e)
        if m and m.group(1) != 'adswill':
            names.setdefault(m.group(1), [n, 0])[1] += 1
    prs = git('log', since + '..' + base, '--grep=Merge pull request', '--format=%s').splitlines()
    return names, prs

TAG = re.compile(r'\s*\[\[([^\]]+)\]\]\s*$')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', default='pub/integ-30')
    ap.add_argument('--since', default='v0.2.9')
    ap.add_argument('--issues-json')
    ap.add_argument('--src', default='tools/release/changelog-3.0.src.md')
    ap.add_argument('--out', default='docs/CHANGELOG-3.0.md')
    ap.add_argument('--readme', default='README.md')
    ap.add_argument('--no-readme', action='store_true')
    ap.add_argument('--worktree-modes', action='store_true', help='read the mode table from the working tree instead of --base')
    a = ap.parse_args()

    rows_new, groups = mode_info('WORKTREE' if a.worktree_modes else a.base)
    rows_old, _ = mode_info(a.since)
    old_keys = {(f, n) for f, n, _ in rows_old}
    added = [(f, n, g) for f, n, g in rows_new if (f, n) not in old_keys and f not in SKIP_FAMILIES]
    total, was = count_modes(rows_new), count_modes(rows_old)
    mt = ['| Mode | Group | Family number |', '|---|---|---|']
    for f, n, g in added:
        mt.append('| %s | %s | %d |' % (n.replace('|', '/'), groups[g].title() if g < len(groups) else '', f))
    extra = [n for f, n, g in rows_new if f in SKIP_FAMILIES]
    if extra:
        mt.append('')
        mt.append('The mode list also has %s (not counted as modes).' % ' and '.join(extra))
    cache = {}
    authors = load_issue_authors(a.issues_json)

    src = open(a.src).read()
    src = re.sub(r'<!--.*?-->\s*', '', src, flags=re.S)
    # split into sections, apply tags
    pending = []
    out_lines = []
    section = ''
    for line in src.split('\n'):
        if line.startswith('## '):
            section = line[3:].strip()
        m = TAG.search(line) if line.strip() and not line.startswith('#') else None
        if m and not line.lstrip().startswith(('-', '|')):
            # a paragraph tag covers the whole block since the last ### heading (tables included)
            brs = [b.strip() for b in m.group(1).split(',')]
            for b in brs:
                if b not in cache:
                    cache[b] = merged(a.base, b)
            line = TAG.sub('', line).rstrip()
            if not all(cache[b] for b in brs):
                start = max([i for i, l in enumerate(out_lines) if l.startswith('### ')] or [len(out_lines)])
                blk = out_lines[start:] + [line]
                del out_lines[start:]
                pending.append((section, '\n'.join(blk).strip('\n'), [b for b in brs if not cache[b]]))
                continue
            out_lines.append(line)
            continue
        if m:
            brs = [b.strip() for b in m.group(1).split(',')]
            for b in brs:
                if b not in cache:
                    cache[b] = merged(a.base, b)
            line = TAG.sub('', line).rstrip()
            if not all(cache[b] for b in brs):
                pending.append((section, line, [b for b in brs if not cache[b]]))
                continue
        out_lines.append(line)
    text = '\n'.join(out_lines)

    # issues block
    issue_rows = []
    keep = []
    in_issues = False
    for line in text.split('\n'):
        if line.strip() == '{{ISSUES}}':
            in_issues = True
            keep.append(line)
            continue
        keep.append(line)
    text = '\n'.join(keep)
    issues_src = [l for l in open(a.src).read().split('\n') if re.match(r'- #\d+ \|', l)]
    rows = ['| Issue | Reporter | Status | What |', '|---|---|---|---|']
    npend = 0
    for l in issues_src:
        m = TAG.search(l)
        brs = [b.strip() for b in m.group(1).split(',')] if m else []
        l2 = TAG.sub('', l)
        num, status, what = [x.strip() for x in l2[2:].split('|', 2)]
        for b in brs:
            if b not in cache:
                cache[b] = merged(a.base, b)
        nums = [int(x) for x in re.findall(r'\d+', num)]
        rep = ', '.join(sorted({authors[x] for x in nums if x in authors}))
        if brs and not all(cache[b] for b in brs):
            pending.append(('Issues from GitHub', '- %s (%s): %s' % (num, status, what), [b for b in brs if not cache[b]]))
            continue
        rows.append('| %s | %s | %s | %s |' % (num, rep, status, what.replace('|', '/')))
    text = text.replace('{{ISSUES}}', '\n'.join(rows) if len(rows) > 2 else 'No issue is closed by this build yet.')
    # contributors
    names, prs = contributors(a.base, a.since)
    c = []
    for h, (n, k) in sorted(names.items()):
        c.append('- **@%s** (%s): %d commit%s since %s.' % (h, n, k, '' if k == 1 else 's', a.since))
    if prs:
        c.append('- Pull requests merged since %s: %s.' % (a.since, '; '.join(sorted({re.sub(r"^Merge pull request ", "", p) for p in prs}))))
    c.append('- Earlier credits stay in the README: **@ffishman** (mesh clock fix, PR #217, and an offered PR for one engine-wide clock, issue #218) and **@frank-1225** (Chinese railway decoders, PR #231).')
    c.append('- Everyone who sent an issue, a recording or a Discord message: the reporter of each fixed issue is named in the table above. Third-party code and credits are in THIRD_PARTY.md and in Help > About.')
    c.append('- Work in this release was written with Claude (Anthropic) as co-author; the commits say so.')
    text = text.replace('{{CONTRIBUTORS}}', '\n'.join(c))
    text = text.replace('{{MODE_COUNT}}', str(total)).replace('{{NEW_MODE_COUNT}}', str(total - was))
    text = text.replace('{{NEW_MODES}}', 'New in the mode list since %s (%d, from app/ui/mode_table.cpp):\n\n%s' % (a.since, len(added), '\n'.join(mt)))
    text = text.replace('{{GENERATED}}', 'Generated %s from %s by tools/release/gen_changelog.py.' % (datetime.date.today().isoformat(), a.base))

    if pending:
        text = text.rstrip('\n') + '\n\n## Not in this build yet\n\nThese are finished or half-finished on work branches that are not merged into %s. They are not part of 3.0 until they are.\n\n' % a.base
        for sec, line, brs in pending:
            text += '%s *(%s; waiting for %s)*\n\n' % (line.rstrip(), sec, ', '.join(brs))
    # maintainer appendix
    allb = [b.strip().replace('origin/', '').replace('pub/', '') for b in git('branch', '-r').splitlines() if b.strip().replace('pub/', '').endswith('-030')]
    um = sorted(b for b in set(allb) if not merged(a.base, b))
    text = text.rstrip('\n') + '\n\n## For maintainers\n\n%d work branches (`*-030`) exist on pub; %d are merged into %s. Not merged: %s.\n' % (len(set(allb)), len(set(allb)) - len(um), a.base, ', '.join(um) if um else 'none')
    # unlisted issue numbers
    listed = {int(x) for x in re.findall(r'#(\d+)', open(a.src).read())}
    seen = set(int(x) for x in re.findall(r'\(?#(\d+)\)?', '\n'.join(git('log', a.since + '..' + a.base, '--format=%s').splitlines())))
    miss = sorted(seen - listed)
    if miss:
        text += '\nIssue numbers in commit messages since %s that this text does not mention (check by hand, some are pull requests): %s.\n' % (a.since, ', '.join('#%d' % x for x in miss))
    open(a.out, 'w').write(text.rstrip('\n') + '\n')
    print('wrote', a.out, '| modes', total, '(+%d)' % (total - was), '| pending bullets', len(pending), file=sys.stderr)

    if not a.no_readme and os.path.exists(a.readme):
        r = open(a.readme).read()
        grp = readme_rows(rows_new, groups)
        trows = ['| Family | Modes | What you get |', '|---|:---:|---|']
        # the descriptions stay hand-written in the README; only counts are generated
        desc = dict(re.findall(r'^\| \*\*(.+?)\*\* \| \d+ \| (.*) \|$', r, re.M))
        for name, gn in README_GROUPS:
            cnt = sum(len(grp.get(g, [])) for g in gn)
            if cnt:
                trows.append('| **%s** | %d | %s |' % (name, cnt, desc.get(name, '')))
        r = re.sub(r'(<!-- modes-table:begin -->\n).*?(\n<!-- modes-table:end -->)', lambda m: m.group(1) + '\n'.join(trows) + m.group(2), r, flags=re.S)
        r = re.sub(r'<!-- modecount -->.*?<!-- /modecount -->', '<!-- modecount -->%d<!-- /modecount -->' % total, r)
        open(a.readme, 'w').write(r)
        print('README: modes', total, file=sys.stderr)

if __name__ == '__main__':
    main()
