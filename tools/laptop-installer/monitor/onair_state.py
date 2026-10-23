#!/usr/bin/env python3
# OnAir laptop worker: writes the state files that monitor.py shows. Called by the OnAir-ci workflows.
# A copy of this file lives in adswill/OnAir-ci as scripts/onair_state.py (that copy is the one the jobs run, because
# the OnAir ref being built may not contain it). Keep both the same. Python 3 stdlib only. ASCII only.
# It never fails the job: every error is swallowed and the exit code is always 0.
#
#   onair_state.py begin --workflow W --ref R [--targets T] [--tests X] [--url U] [--phases a,b,c]
#   onair_state.py set key=value [key=value ...]
#   onair_state.py phase NAME
#   onair_state.py end --result success|failure|cancelled
#   onair_state.py install SRC_DIR      copy monitor files from SRC_DIR to ~/onair-monitor unless older
import json, os, re, shutil, sys, time

HOME = os.path.expanduser('~')
MON = os.environ.get('ONAIR_MONITOR_DIR', os.path.join(HOME, 'onair-monitor'))
STATE = os.path.join(MON, 'state')
CUR = os.path.join(STATE, 'current.json')
HIST = os.path.join(STATE, 'history.json')
LOGS = ('build.log', 'ctest.log', 'configure.log')
HIST_KEEP = 100


def load(path, default):
    try:
        with open(path, 'r', encoding='utf-8') as f:
            return json.load(f)
    except Exception:
        return default


def save(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp%d' % os.getpid()
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(obj, f, indent=1)
    os.replace(tmp, path)


def opt(args, name, default=''):
    if name in args:
        i = args.index(name)
        if i + 1 < len(args):
            return args[i + 1]
    return default


def close_phase(cur, now):
    for p in cur.get('phases', []):
        if p.get('end') is None:
            p['end'] = now


def cmd_begin(args):
    now = int(time.time())
    os.makedirs(STATE, exist_ok=True)
    for n in LOGS:
        p = os.path.join(STATE, n)
        try:
            if os.path.exists(p):
                os.replace(p, p + '.prev')
        except Exception:
            pass
        open(p, 'w').close()
    phases = [x for x in opt(args, '--phases', 'checkout,configure,build,test,summary').split(',') if x]
    cur = {
        'workflow': opt(args, '--workflow', '?'),
        'ref': opt(args, '--ref', '?'),
        'sha': opt(args, '--sha', ''),
        'targets': opt(args, '--targets', ''),
        'tests': opt(args, '--tests', ''),
        'run_url': opt(args, '--url', ''),
        'started': now,
        'phase_list': phases,
        'phase': '',
        'phases': [],
    }
    save(CUR, cur)


def cmd_set(args):
    cur = load(CUR, None)
    if not isinstance(cur, dict):
        return
    for kv in args:
        if '=' in kv:
            k, v = kv.split('=', 1)
            if re.match(r'^[a-z_]{1,20}$', k):
                cur[k] = v
    save(CUR, cur)


def cmd_phase(args):
    cur = load(CUR, None)
    if not isinstance(cur, dict) or not args:
        return
    now = int(time.time())
    close_phase(cur, now)
    name = args[0]
    cur.setdefault('phases', []).append({'name': name, 'start': now, 'end': None})
    cur['phase'] = name
    if name not in cur.get('phase_list', []):
        cur.setdefault('phase_list', []).append(name)
    save(CUR, cur)


def cmd_end(args):
    cur = load(CUR, None)
    now = int(time.time())
    result = opt(args, '--result', 'unknown').lower()
    word = {'success': 'PASS', 'failure': 'FAIL', 'cancelled': 'CANCEL'}.get(result, result.upper()[:6] or '?')
    if isinstance(cur, dict):
        close_phase(cur, now)
        entry = {
            'workflow': cur.get('workflow', '?'), 'ref': cur.get('ref', '?'), 'sha': cur.get('sha', ''),
            'targets': cur.get('targets', ''), 'tests': cur.get('tests', ''), 'run_url': cur.get('run_url', ''),
            'started': cur.get('started', now), 'duration': now - int(cur.get('started', now) or now),
            'result': word,
            'phases': [{'name': p.get('name'), 'secs': int((p.get('end') or now) - (p.get('start') or now))}
                       for p in cur.get('phases', [])],
        }
        hist = load(HIST, [])
        if not isinstance(hist, list):
            hist = []
        hist.append(entry)
        save(HIST, hist[-HIST_KEEP:])
    try:
        os.remove(CUR)
    except Exception:
        pass


def version_of(path):
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.match(r'^VERSION\s*=\s*(\d+)', line)
                if m:
                    return int(m.group(1))
    except Exception:
        pass
    return -1


def cmd_install(args):
    if not args:
        return
    src = args[0]
    new = os.path.join(src, 'monitor.py')
    if not os.path.isfile(new):
        print('monitor: no monitor files in this ref, keeping the installed ones')
        return
    if version_of(new) < version_of(os.path.join(MON, 'monitor.py')):
        print('monitor: the installed monitor is newer, keeping it')
        return
    os.makedirs(STATE, exist_ok=True)
    for n in os.listdir(src):
        if n.endswith('.py') or n.startswith('README'):
            shutil.copy2(os.path.join(src, n), os.path.join(MON, n))
    print('monitor: installed version', version_of(new), 'to', MON)


def main(argv):
    if not argv:
        return
    fn = {'begin': cmd_begin, 'set': cmd_set, 'phase': cmd_phase, 'end': cmd_end, 'install': cmd_install}.get(argv[0])
    if fn:
        fn(argv[1:])


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except Exception as e:  # never break the job
        print('onair_state:', e)
    sys.exit(0)
