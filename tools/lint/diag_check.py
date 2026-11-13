#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
"""Checks the diagnostic codes of OnAir (docs/DIAGNOSTICS.md). Python standard library only.

  python3 tools/lint/diag_check.py              check the tree (and the git history for permanence)
  python3 tools/lint/diag_check.py --new 5      print 5 random valid codes that are not in use
  python3 tools/lint/diag_check.py --self-test  run the rules on tests/diag/fixtures and compare with the expected findings
  options: --no-history (skip the permanence check), --root DIR, --quiet, FILE... (check only these files, no history)

Call forms it reads:
  C++     DIAG / DIAG_ERROR / DIAG_DEBUG / DIAG_TEXT (code, "fmt", ...), DIAG_EVERY / DIAG_DEBUG_EVERY (code, seconds, "fmt", ...), DIAG_UI(code, tr("text"))
  Kotlin  Diag.e / Diag.w / Diag.i / Diag.text (code, "msg"), Diag.d(code) { "msg" }
  Rust    diag!(code, "fmt", ...), diag_debug!(...), diag_text!(...)
  Python  diag(code, "fmt", ...), diag_debug(...), diag_text(...)
  JS      diag(code, "msg"), diagDebug(...), diagText(...)

Rules (an error fails the check):
  code      the first argument is one six-digit decimal literal on the line of the call: 100000-999999, last digit not 0, not within 1
            of a power of two, more than 3 bits set, not all digits the same
  literal   the message is exactly one string literal (placeholders are fine; no +, no joined literals, no variable)
  sentence  the message starts with a capital letter, digit or placeholder, ends in . ? or :, and has at least 6 words
  generic   the message is not only a generic word such as "failed" or "error"
  dup-code  each code is at exactly one call site in the tree
  dup-text  each message (placeholders normalised) is used once in the tree
  include   dect2/common/diag.h is included from source files only, never from a header or a file listed in NO_DIAG_HEADER
  history   a code once committed stays at its file: it may not disappear or move to another file (a file renamed with it is fine)
            unless tools/lint/diag_retired.txt lists it
Warnings (printed, not failing): a DIAG / DIAG_DEBUG / DIAG_ERROR inside a for/while body in C++ (per-sample or per-frame paths want a
counter or DIAG_EVERY).
"""
import json, os, re, secrets, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
FIXTURES = 'tests/diag/fixtures'
RETIRED = 'tools/lint/diag_retired.txt'
# the files that define the call forms: not call sites
IMPL = {'core/include/dect2/common/diag.h', 'core/src/common/diag.cpp', 'tools/lint/diag_check.py', 'tools/diag.py',
        'rust/onair_common/src/diag.rs', 'android/app/src/main/java/io/github/adswill/onair/diag/Diag.kt'}
# third-party files with an identifier of their own named DIAG: the header must never reach them
NO_DIAG_HEADER = {'core/src/dvbs/dvbs_sf.cpp'}
DIAG_HEADER = 'dect2/common/diag.h'
SKIP_DIRS = ('third_party/', 'build', '.git/', 'node_modules/', FIXTURES + '/')

LANG_BY_EXT = {'.cpp': 'cpp', '.cc': 'cpp', '.c': 'cpp', '.h': 'cpp', '.hpp': 'cpp', '.mm': 'cpp', '.kt': 'kt', '.rs': 'rs', '.py': 'py',
               '.js': 'js', '.mjs': 'js', '.ts': 'js'}
# call name -> index of the message argument (None: the message is the Kotlin trailing lambda)
CALLS = {
    'cpp': {'DIAG': 1, 'DIAG_ERROR': 1, 'DIAG_DEBUG': 1, 'DIAG_TEXT': 1, 'DIAG_EVERY': 2, 'DIAG_DEBUG_EVERY': 2, 'DIAG_UI': 1},
    'kt': {'Diag.e': 1, 'Diag.w': 1, 'Diag.i': 1, 'Diag.text': 1, 'Diag.d': None},
    'rs': {'diag!': 1, 'diag_debug!': 1, 'diag_text!': 1},
    'py': {'diag': 1, 'diag_debug': 1, 'diag_text': 1},
    'js': {'diag': 1, 'diagDebug': 1, 'diagText': 1},
}
LOGGING_CPP = {'DIAG', 'DIAG_ERROR', 'DIAG_DEBUG'}
GENERIC = {'failed', 'error', 'invalid', 'cannot open', 'write error', 'read error', 'not supported', 'out of memory', 'cancelled', 'ok',
           'unknown error', 'timeout', 'no data'}
HISTORY_RE = r'DIAG(_[A-Z]+)?\s*\(|Diag\.[a-z]+\s*\(|diag(_debug|_text)?!?\s*\(|diag(Debug|Text)\s*\('
CODE_IN_LINE = re.compile(r'(?:\bDIAG(?:_[A-Z]+)?|\bDiag\.[a-z]+|\bdiag(?:_debug|_text)?!?|\bdiag(?:Debug|Text))\s*\(\s*([1-9]\d{5})\b')


def valid_code(c):
    if c < 100000 or c > 999999 or c % 10 == 0:
        return False
    for k in range(16, 21):
        if abs(c - (1 << k)) <= 1:
            return False
    if bin(c).count('1') <= 3:
        return False
    return len(set(str(c))) > 1


# ---------------------------------------------------------------- lexer
class Tok:
    __slots__ = ('kind', 'text', 'value', 'line', 'pos')

    def __init__(self, kind, text, value, line, pos):
        self.kind, self.text, self.value, self.line, self.pos = kind, text, value, line, pos

    def __repr__(self):
        return '%s:%r@%d' % (self.kind, self.text, self.line)


IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
NUMBER = re.compile(r"[0-9][0-9a-zA-Z_.']*")


def lex(src, lang):
    """Tokens with comments dropped and string literals resolved: kinds ident, num, str, punct."""
    toks, i, n, line = [], 0, len(src), 1
    hash_comment = lang == 'py'
    while i < n:
        c = src[i]
        if c == '\n':
            line += 1; i += 1; continue
        if c in ' \t\r\f\v':
            i += 1; continue
        if c == '\\' and lang == 'cpp' and src.startswith('\\\n', i):
            line += 1; i += 2; continue
        if hash_comment and c == '#':
            j = src.find('\n', i)
            i = n if j < 0 else j
            continue
        if not hash_comment and src.startswith('//', i):
            j = src.find('\n', i)
            i = n if j < 0 else j
            continue
        if not hash_comment and src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            line += src.count('\n', i, j); i = j
            continue
        start_line = line
        # string literals, with their prefixes
        m = IDENT.match(src, i)
        if m:
            word = m.group(0)
            j = m.end()
            if j < n and src[j] in '"\'':
                low = word.lower()
                if lang == 'cpp' and src[j] == '"' and word in ('R', 'u8R', 'uR', 'UR', 'LR'):
                    k = src.find('(', j)
                    delim = src[j + 1:k]
                    end = src.find(')' + delim + '"', k)
                    end = n if end < 0 else end
                    toks.append(Tok('str', src[i:end + len(delim) + 2], src[k + 1:end], start_line, i))
                    line += src.count('\n', i, end); i = end + len(delim) + 2
                    continue
                if (lang == 'cpp' and word in ('u8', 'u', 'U', 'L')) or (lang == 'py' and set(low) <= set('rbuf') and len(low) <= 2) \
                        or (lang == 'rs' and word in ('b',)):
                    t, i, line = lex_quoted(src, j, lang, line, raw=('r' in low and lang == 'py'), prefix=word, start=i)
                    toks.append(t); continue
            if lang == 'rs' and word in ('r', 'br') and j < n and src[j] == '#':
                k = j
                while k < n and src[k] == '#':
                    k += 1
                if k < n and src[k] == '"':
                    hashes = src[j:k]
                    end = src.find('"' + hashes, k + 1)
                    end = n if end < 0 else end
                    toks.append(Tok('str', src[i:end + 1 + len(hashes)], src[k + 1:end], start_line, i))
                    line += src.count('\n', i, end); i = end + 1 + len(hashes)
                    continue
            toks.append(Tok('ident', word, word, line, i)); i = j
            continue
        if c.isdigit():
            m = NUMBER.match(src, i)
            toks.append(Tok('num', m.group(0), m.group(0), line, i)); i = m.end()
            continue
        if c == '"' or c == '`' or (c == "'" and lang in ('py', 'js')):
            t, i, line = lex_quoted(src, i, lang, line)
            toks.append(t); continue
        if c == "'":   # C++/Kotlin char literal, Rust char literal or lifetime
            if lang == 'rs':
                m = re.match(r"'(\\.[^']*|[^'\\])'", src[i:i + 12])
                if m:
                    toks.append(Tok('char', m.group(0), m.group(0), line, i)); i += m.end(); continue
                toks.append(Tok('punct', "'", "'", line, i)); i += 1; continue
            j = i + 1
            while j < n and src[j] != "'" and src[j] != '\n':
                j += 2 if src[j] == '\\' else 1
            toks.append(Tok('char', src[i:j + 1], src[i:j + 1], line, i)); i = j + 1
            continue
        toks.append(Tok('punct', c, c, line, i)); i += 1
    return toks


def lex_quoted(src, i, lang, line, raw=False, prefix='', start=None):
    start = i if start is None else start
    n = len(src)
    q = src[i]
    triple = src.startswith(q * 3, i) and lang in ('py', 'kt')
    if triple:
        end = src.find(q * 3, i + 3)
        end = n if end < 0 else end
        tok = Tok('str', src[start:end + 3], src[i + 3:end], line, start)
        line += src.count('\n', i, end)
        return tok, end + 3, line
    j = i + 1
    depth = 0
    while j < n:
        ch = src[j]
        if ch == '\\' and not raw:
            j += 2; continue
        if q == '`' and src.startswith('${', j):
            depth += 1; j += 2; continue
        if q == '`' and depth and ch == '}':
            depth -= 1; j += 1; continue
        if ch == q and not depth:
            break
        if ch == '\n' and q != '`':
            break
        j += 1
    tok = Tok('str', src[start:j + 1], src[i + 1:j], line, start)
    tok.value = src[i + 1:j]
    if prefix.lower().startswith('f') or 'f' in prefix.lower():
        tok.kind = 'str'   # f-strings are one literal with placeholders
    line += src.count('\n', i, j)
    return tok, j + 1, line


# ---------------------------------------------------------------- call sites
class Site:
    __slots__ = ('path', 'line', 'name', 'code', 'msg', 'lang')

    def __init__(self, path, line, name, code, msg, lang):
        self.path, self.line, self.name, self.code, self.msg, self.lang = path, line, name, code, msg, lang


def split_args(toks, k):
    """toks[k] is '(': returns (list of argument token lists, index after ')')."""
    depth, args, cur = 0, [], []
    i = k
    while i < len(toks):
        t = toks[i]
        if t.kind == 'punct' and t.text in '([{':
            depth += 1
            if depth > 1:
                cur.append(t)
        elif t.kind == 'punct' and t.text in ')]}':
            depth -= 1
            if depth == 0:
                args.append(cur)
                return args, i + 1
            cur.append(t)
        elif t.kind == 'punct' and t.text == ',' and depth == 1:
            args.append(cur); cur = []
        else:
            cur.append(t)
        i += 1
    args.append(cur)
    return args, i


def norm_text(s):
    s = re.sub(r'%[-+ #0]*(\d+|\*)?(\.(\d+|\*))?(hh|h|ll|l|L|z|j|t|I64)?[diouxXeEfgGcsp]', '#', s)
    s = re.sub(r'\$\{[^}]*\}|\$[A-Za-z_][A-Za-z0-9_.]*|\{[^}]*\}', '#', s)
    s = s.replace('%%', '%')
    return s


def words(s):
    return re.findall(r"[A-Za-z0-9][A-Za-z0-9'+./-]*", norm_text(s).replace('#', ' '))


def find_sites(path, src, lang, errors, warnings):
    toks = lex(src, lang)
    names = CALLS[lang]
    sites = []
    loop_stack = []   # C++: for each open brace, whether it opens a for/while body
    pending_loop = False
    i = 0
    while i < len(toks):
        t = toks[i]
        if lang == 'cpp' and t.kind == 'ident' and t.text in ('for', 'while') and i + 1 < len(toks) and toks[i + 1].text == '(':
            _, j = split_args(toks, i + 1)
            if j < len(toks) and toks[j].text == '{':
                pending_loop = True
            i = j
            continue
        if lang == 'cpp' and t.kind == 'punct' and t.text == '{':
            loop_stack.append(pending_loop); pending_loop = False
        elif lang == 'cpp' and t.kind == 'punct' and t.text == '}':
            if loop_stack:
                loop_stack.pop()
        name, k = None, i
        if t.kind == 'ident':
            if lang == 'kt' and t.text == 'Diag' and i + 2 < len(toks) and toks[i + 1].text == '.' and toks[i + 2].kind == 'ident':
                cand = 'Diag.' + toks[i + 2].text
                if cand in names:
                    name, k = cand, i + 3
            elif lang == 'rs' and i + 1 < len(toks) and toks[i + 1].text == '!':
                if t.text + '!' in names:
                    name, k = t.text + '!', i + 2
            elif t.text in names:
                name, k = t.text, i + 1
            if name and i > 0:
                prev = toks[i - 1]
                if (prev.kind == 'ident' and prev.text in ('def', 'fn', 'fun', 'function', 'define', 'macro_rules')) or \
                        (prev.kind == 'punct' and prev.text == '.' and lang != 'kt'):
                    name = None
        if not name or k >= len(toks) or toks[k].text != '(':
            i += 1
            continue
        args, after = split_args(toks, k)
        site = Site(path, t.line, name, None, None, lang)
        where = '%s:%d' % (path, t.line)
        # the code
        a0 = args[0] if args else []
        if len(a0) == 1 and a0[0].kind == 'num' and re.fullmatch(r'[1-9]\d{5}', a0[0].text):
            site.code = int(a0[0].text)
            if not valid_code(site.code):
                errors.append((path, t.line, 'code', '%s: code %d breaks the number rules (docs/DIAGNOSTICS.md)' % (where, site.code)))
            if a0[0].line != t.line:
                errors.append((path, t.line, 'code', '%s: the code must sit on the line of the call so one search finds it' % where))
        else:
            errors.append((path, t.line, 'code', '%s: %s needs a six-digit decimal literal as its code, found "%s"'
                           % (where, name, ' '.join(x.text for x in a0)[:40])))
        # the message
        mi = names[name]
        msg_toks = None
        if mi is None:   # Kotlin Diag.d(code) { "..." }
            if after < len(toks) and toks[after].text == '{':
                body, _ = split_args(toks, after)
                msg_toks = body[0] if body else []
            else:
                msg_toks = args[1] if len(args) > 1 else []
        else:
            msg_toks = args[mi] if len(args) > mi else []
        if name == 'DIAG_UI' and len(msg_toks) >= 3 and msg_toks[0].kind == 'ident' and msg_toks[0].text == 'tr' and msg_toks[1].text == '(' \
                and msg_toks[-1].text == ')':
            msg_toks = msg_toks[2:-1]
        if len(msg_toks) == 1 and msg_toks[0].kind == 'str':
            site.msg = msg_toks[0].value
            check_message(site, errors)
        else:
            errors.append((path, t.line, 'literal', '%s: the message must be exactly one string literal' % where))
        if lang == 'cpp' and name in LOGGING_CPP and any(loop_stack):
            warnings.append('%s: %s inside a for/while body: on a per-sample or per-frame path use a counter or DIAG_EVERY' % (where, name))
        sites.append(site)
        i = k
    return sites


def check_message(site, errors):
    m = site.msg
    where = '%s:%d' % (site.path, site.line)
    stripped = norm_text(m).strip()
    if stripped.lower().rstrip('.:?! ') in GENERIC:
        errors.append((site.path, site.line, 'generic', '%s: "%s" is generic: say what happened, with which value, and what OnAir does next' % (where, m)))
        return
    first = stripped[:1]
    if not (first.isupper() or first.isdigit() or first == '#'):
        errors.append((site.path, site.line, 'sentence', '%s: the message must start with a capital letter: "%s"' % (where, m[:60])))
    elif not stripped.endswith(('.', '?', ':')):
        errors.append((site.path, site.line, 'sentence', '%s: the message must end in . ? or : "%s"' % (where, m[-60:])))
    elif len(words(m)) < 6:
        errors.append((site.path, site.line, 'sentence', '%s: the message needs at least 6 words: "%s"' % (where, m)))


# ---------------------------------------------------------------- the tree
def git(*args, check=True):
    r = subprocess.run(['git', '-C', ROOT] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and r.returncode:
        raise RuntimeError('git %s: %s' % (' '.join(args), r.stderr.decode(errors='replace').strip()))
    return r.stdout.decode('utf-8', errors='replace')


def tree_files():
    out = git('ls-files', '-co', '--exclude-standard')
    files = []
    for p in out.splitlines():
        if p.startswith(SKIP_DIRS) or p in IMPL:
            continue
        if os.path.splitext(p)[1] in LANG_BY_EXT and os.path.isfile(os.path.join(ROOT, p)):
            files.append(p)
    return files


def read(path):
    with open(os.path.join(ROOT, path), encoding='utf-8', errors='replace') as f:
        return f.read()


CPP_PRESCAN = re.compile(r'\bDIAG(_[A-Z]+)?\s*\(|' + re.escape(DIAG_HEADER))
OTHER_PRESCAN = {'kt': re.compile(r'\bDiag\.'), 'rs': re.compile(r'\bdiag(_debug|_text)?!'),
                 'py': re.compile(r'\bdiag(_debug|_text)?\s*\('), 'js': re.compile(r'\bdiag(Debug|Text)?\s*\(')}


def scan(files, errors, warnings):
    sites = []
    for p in files:
        lang = LANG_BY_EXT[os.path.splitext(p)[1]]
        src = read(p)
        if lang == 'cpp':
            if not CPP_PRESCAN.search(src):
                continue
            includes = re.search(r'#\s*include\s*[<"]' + re.escape(DIAG_HEADER) + '[">]', src)
            if includes and (p in NO_DIAG_HEADER or os.path.splitext(p)[1] in ('.h', '.hpp')):
                errors.append((p, 1, 'include', '%s: %s may be included from source files only (and never from %s)'
                               % (p, DIAG_HEADER, ', '.join(sorted(NO_DIAG_HEADER)))))
            if not includes:
                # DIAG( in a file without the header is someone else's identifier, unless it carries a code
                if re.search(r'\bDIAG(_[A-Z]+)?\s*\(\s*[1-9]\d{5}\b', src):
                    errors.append((p, 1, 'include', '%s: uses DIAG with a code but does not include %s' % (p, DIAG_HEADER)))
                continue
        elif not OTHER_PRESCAN[lang].search(src):
            continue
        sites.extend(find_sites(p, src, lang, errors, warnings))
    return sites


def check_unique(sites, errors):
    by_code, by_text = {}, {}
    for s in sites:
        if s.code is not None:
            by_code.setdefault(s.code, []).append(s)
        if s.msg is not None:
            by_text.setdefault(' '.join(norm_text(s.msg).lower().split()), []).append(s)
    for code, ss in by_code.items():
        if len(ss) > 1:
            for s in ss[1:]:
                errors.append((s.path, s.line, 'dup-code', '%s:%d: code %d is already used at %s:%d' % (s.path, s.line, code, ss[0].path, ss[0].line)))
    for text, ss in by_text.items():
        if len(ss) > 1:
            for s in ss[1:]:
                errors.append((s.path, s.line, 'dup-text', '%s:%d: the same message is already used at %s:%d (codes %s and %s)'
                               % (s.path, s.line, ss[0].path, ss[0].line, ss[0].code, s.code)))
    return by_code


# ---------------------------------------------------------------- permanence
def read_retired():
    out = {}
    p = os.path.join(ROOT, RETIRED)
    if os.path.exists(p):
        with open(p, encoding='utf-8') as f:
            for ln in f:
                ln = ln.split('#', 1)[0].strip()
                m = re.match(r'(\d{6})\s+(retired|moved)\b', ln)
                if m:
                    out[int(m.group(1))] = m.group(2)
    return out


def history_cache_path():
    try:
        d = git('rev-parse', '--git-common-dir').strip()
    except RuntimeError:
        return None
    if not os.path.isabs(d):
        d = os.path.join(ROOT, d)
    return os.path.join(d, 'onair-diag-history.json')


def history_codes(quiet):
    """{code: [paths it was ever added in]} from the commits of HEAD, cached in the git folder and extended from the cached commit."""
    try:
        head = git('rev-parse', 'HEAD').strip()
    except RuntimeError:
        return {}, 'no git history'
    cache_file = history_cache_path()
    cache = {'head': None, 'codes': {}}
    if cache_file and os.path.exists(cache_file):
        try:
            with open(cache_file, encoding='utf-8') as f:
                cache = json.load(f)
        except (OSError, ValueError):
            pass
    rng = 'HEAD'
    if cache.get('head'):
        if cache['head'] == head:
            return {int(k): v for k, v in cache['codes'].items()}, 'cached'
        if subprocess.run(['git', '-C', ROOT, 'merge-base', '--is-ancestor', cache['head'], head]).returncode == 0:
            rng = cache['head'] + '..HEAD'
        else:
            cache = {'head': None, 'codes': {}}
    exts = sorted({'*' + e for e in LANG_BY_EXT})
    out = git('log', '--format=@@commit %H', '-p', '--unified=0', '--no-renames', '--no-color', '-G', HISTORY_RE, rng, '--',
              *exts, ':(exclude)' + FIXTURES, ':(exclude)third_party')
    codes = {int(k): v for k, v in cache.get('codes', {}).items()}
    path = None
    for ln in out.splitlines():
        if ln.startswith('+++ '):
            path = ln[6:] if ln.startswith('+++ b/') else None
        elif ln.startswith('+') and path and path not in IMPL:
            for m in CODE_IN_LINE.finditer(ln):
                lst = codes.setdefault(int(m.group(1)), [])
                if path not in lst:
                    lst.append(path)
    if cache_file:
        try:
            with open(cache_file, 'w', encoding='utf-8') as f:
                json.dump({'head': head, 'codes': {str(k): v for k, v in codes.items()}}, f)
        except OSError:
            pass
    shallow = git('rev-parse', '--is-shallow-repository', check=False).strip() == 'true'
    return codes, ('shallow clone: only the commits present were read' if shallow else 'read')


def check_history(by_code, errors, quiet):
    hist, how = history_codes(quiet)
    retired = read_retired()
    for code, how_retired in retired.items():
        if how_retired == 'retired' and code in by_code:
            s = by_code[code][0]
            errors.append((s.path, s.line, 'history', '%s:%d: code %d is retired (%s) and may not be used again' % (s.path, s.line, code, RETIRED)))
    for code, paths in hist.items():
        if code in retired:
            continue
        if code not in by_code:
            errors.append((paths[-1], 0, 'history', 'code %d (committed in %s) is gone: codes are permanent; list it in %s if it is retired'
                           % (code, paths[0], RETIRED)))
            continue
        now = by_code[code][0].path
        for old in paths:
            if old != now and os.path.exists(os.path.join(ROOT, old)):
                errors.append((now, by_code[code][0].line, 'history', '%s: code %d belongs to %s (committed there); a new site needs a new code'
                               % (now, code, old)))
                break
    return how, len(hist)


# ---------------------------------------------------------------- commands
def new_codes(n):
    used = set()
    errs, warns = [], []
    for s in scan(tree_files(), errs, warns):
        if s.code:
            used.add(s.code)
    try:
        used |= set(history_codes(True)[0])
    except RuntimeError:
        pass
    used |= set(read_retired())
    for p in list(IMPL) + ['docs/DIAGNOSTICS.md'] + [os.path.join(FIXTURES, f) for f in (os.listdir(os.path.join(ROOT, FIXTURES)) if os.path.isdir(os.path.join(ROOT, FIXTURES)) else [])]:
        if os.path.isfile(os.path.join(ROOT, p)):
            used |= {int(x) for x in re.findall(r'\b([1-9]\d{5})\b', read(p))}
    out = []
    while len(out) < n:
        c = 100000 + secrets.randbelow(900000)
        if valid_code(c) and c not in used:
            used.add(c); out.append(c)
    print('\n'.join(str(c) for c in out))
    return 0


def check_files(files, history, quiet):
    errors, warnings = [], []
    sites = scan(files, errors, warnings)
    by_code = check_unique(sites, errors)
    hist_note = 'not checked'
    if history:
        try:
            how, nh = check_history(by_code, errors, quiet)
            hist_note = '%d committed codes, %s' % (nh, how)
        except RuntimeError as e:
            hist_note = 'not checked (%s)' % e
    return sites, errors, warnings, hist_note


def self_test():
    """Each fixture line that should fail carries `expect: <rule>` in a comment; the findings must match exactly."""
    fdir = os.path.join(ROOT, FIXTURES)
    files = sorted(os.path.join(FIXTURES, f) for f in os.listdir(fdir) if os.path.splitext(f)[1] in LANG_BY_EXT)
    ok = True
    for p in files:
        src = read(p)
        expected = set()
        for no, ln in enumerate(src.splitlines(), 1):
            for m in re.finditer(r'expect:\s*([a-z-]+)', ln):
                expected.add((no, m.group(1)))
        _, errors, _, _ = check_files([p], False, True)
        got = {(e[1], e[2]) for e in errors if e[1]}
        got_file = {e[2] for e in errors if not e[1]}
        if got != expected:
            ok = False
            print('FAIL %s' % p)
            for x in sorted(expected - got):
                print('  expected but not found: line %d %s' % x)
            for x in sorted(got - expected):
                print('  found but not expected: line %d %s' % x)
                for e in errors:
                    if (e[1], e[2]) == x:
                        print('    ' + e[3])
        elif got_file:
            ok = False
            print('FAIL %s: file-level findings %s' % (p, sorted(got_file)))
        else:
            print('ok   %s (%d findings as expected)' % (p, len(expected)))
    # the number rules themselves
    cases = {482917: True, 613587: True, 131072: False, 131071: False, 131073: False, 262143: False, 524289: False, 100000: False,
             482910: False, 111111: False, 999999: False, 131137: False, 99999: False, 1000001: False, 100001: True}
    for c, want in cases.items():
        if valid_code(c) != want:
            ok = False
            print('FAIL number rule: %d should be %s' % (c, 'valid' if want else 'rejected'))
    print('diag_check self-test: %s' % ('ok' if ok else 'FAILED'))
    return 0 if ok else 1


def main(argv):
    global ROOT
    args, history, quiet, files = list(argv), True, False, []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--root':
            ROOT = os.path.abspath(args[i + 1]); i += 2; continue
        if a == '--new':
            n = int(args[i + 1]) if i + 1 < len(args) and args[i + 1].isdigit() else 1
            return new_codes(n)
        if a == '--self-test':
            return self_test()
        if a == '--no-history':
            history = False
        elif a == '--quiet':
            quiet = True
        elif a in ('-h', '--help'):
            print(__doc__); return 0
        elif a.startswith('-'):
            print('unknown option %s' % a); return 2
        else:
            files.append(os.path.relpath(os.path.abspath(a), ROOT))
        i += 1
    if files:
        history = False
    sites, errors, warnings, hist_note = check_files(files or tree_files(), history, quiet)
    for w in warnings:
        print('warning: ' + w)
    for e in errors:
        print('error: ' + e[3])
    print('diagnostic codes: %d call sites, %d errors, %d warnings; history: %s' % (len(sites), len(errors), len(warnings), hist_note))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
