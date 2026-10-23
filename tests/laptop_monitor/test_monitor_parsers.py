#!/usr/bin/env python3
# Plain-assert tests for tools/laptop-installer/monitor (monitor.py parsers, layout, onair_state.py).
# Run: python3 tests/laptop_monitor/test_monitor_parsers.py
import json, os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
MON = os.path.join(HERE, '..', '..', 'tools', 'laptop-installer', 'monitor')
sys.path.insert(0, MON)
import monitor as M  # noqa: E402


def test_ninja():
    log = '\n'.join([
        '[1/412 0.105s] Building CXX object core/CMakeFiles/a.dir/a.cpp.o',
        '[2/412 0.310s] Building CXX object core/CMakeFiles/a.dir/b.cpp.o',
        '[250/412 61.004s] Building CXX object core/CMakeFiles/a.dir/c.cpp.o',
    ])
    r = M.parse_ninja(log)
    assert (r['done'], r['total']) == (250, 412), r
    assert abs(r['elapsed'] - 61.004) < 1e-6
    assert r['state'] == 'running' and r['last'].endswith('c.cpp.o')
    assert M.build_rate(r, 0) is not None
    # default NINJA_STATUS (no elapsed), finish
    r = M.parse_ninja('[1/2] Building C object x.o\n[2/2] Linking CXX executable app\n')
    assert (r['done'], r['total'], r['elapsed'], r['state']) == (2, 2, None, 'done'), r
    r = M.parse_ninja('ninja: no work to do.\n')
    assert r['state'] == 'uptodate' and r['done'] is None
    r = M.parse_ninja('[3/9 1.0s] Building CXX object x.o\nFAILED: x.o\n'
                      '/src/core/x.cpp:12:5: error: no member named foo\nninja: build stopped: subcommand failed.\n')
    assert r['state'] == 'failed' and r['failed'] == 1 and 'no member named foo' in r['error'], r
    assert M.parse_ninja('')['state'] == 'none'
    assert M.parse_ninja('\x00\xff garbage [x/y] [1/]\r\n')['done'] is None


def test_ctest():
    log = '\n'.join([
        'Test project /home/onair/build',
        '      Start  1: dvbt2_ldpc',
        ' 1/5 Test  #1: dvbt2_ldpc .......................   Passed    0.42 sec',
        '      Start  2: fm_rds',
        ' 2/5 Test  #2: fm_rds ...........................***Failed    1.20 sec',
        '[ RUN      ] Rds.decode',
        'core/tests/fm_rds.cpp:41: error: expected 3, got 4',
        '      Start  3: slow_one',
        ' 3/5 Test  #3: slow_one .........................***Timeout  1500.01 sec',
        ' 4/5 Test  #4: crashy ...........................Subprocess aborted***Exception:   0.10 sec',
        ' 5/5 Test  #5: gpu_only .........................***Skipped   0.00 sec',
        'a_very_long_test_name_that_fills_the_column_completely_x.***Failed  0.01 sec',
        '',
        '40% tests passed, 3 tests failed out of 5',
    ])
    r = M.parse_ctest(log)
    assert (r['done'], r['total']) == (5, 5), r
    names = [x['name'] for x in r['results']]
    assert names == ['dvbt2_ldpc', 'fm_rds', 'slow_one', 'crashy', 'gpu_only'], names
    res = [x['result'] for x in r['results']]
    assert res == ['PASS', 'FAIL', 'TIMEOUT', 'FAIL', 'SKIP'], res
    assert r['results'][1]['error'] == 'core/tests/fm_rds.cpp:41: error: expected 3, got 4', r['results'][1]
    assert abs(r['results'][2]['secs'] - 1500.01) < 1e-6
    assert r['passed'] == 1 and r['failed'] == 3 and r['skipped'] == 1, r
    assert r['summary'].startswith('40% tests passed')
    # long name glued to the dots
    one = M.parse_ctest_line('12/90 Test #12: averyveryverylongname_dvbt2_ldpc_regression_test.***Failed    3.10 sec')
    assert one and one['name'] == 'averyveryverylongname_dvbt2_ldpc_regression_test' and one['result'] == 'FAIL', one
    # tier marker resets the counters
    r = M.parse_ctest(log + '\n=== ONAIR TIER slow ===\n')
    assert r['tier'] == 'slow' and r['done'] == 0 and r['total'] is None
    r = M.parse_ctest('No tests were found!!!\n')
    assert r['nofound'] and r['done'] is None


def test_nvidia():
    g = M.parse_nvidia('NVIDIA GeForce RTX 3080 Ti Laptop GPU, 7, 812, 16384, 52\n')
    assert g == [{'name': 'NVIDIA GeForce RTX 3080 Ti Laptop GPU', 'util': 7.0, 'mem_used': 812.0,
                  'mem_total': 16384.0, 'temp': 52.0}], g
    g = M.parse_nvidia('NVIDIA RTX, [N/A], 100, 8192, [N/A]\n')
    assert g[0]['util'] is None and g[0]['temp'] is None and g[0]['mem_total'] == 8192.0
    assert M.parse_nvidia('') == [] and M.parse_nvidia('garbage') == []


def test_ccache():
    txt = '\n'.join(['Cacheable calls:   251 / 260 (96.54%)', '  Hits:            196 / 251 (78.09%)',
                     '    Direct:        190 / 196', '  Misses:           55 / 251 (21.91%)',
                     'Local storage:', '  Cache size (GB):  4.1 / 20.0 (20.50%)', '  Hits:   1 / 2'])
    r = M.parse_ccache(txt)
    assert (r['hits'], r['misses'], r['size'], r['max']) == (196, 55, 4.1, 20.0), r
    assert M.parse_ccache('')['hits'] is None


def test_runner_log_and_cmdline():
    log = ('[2026-10-23 10:00:00Z INFO Runner] Listening for Jobs\n'
           '[2026-10-23 10:05:00Z INFO Runner] Running job: build-test\n')
    assert M.parse_runner_log(log)[:2] == ('busy', 'build-test')
    log += '[2026-10-23 10:20:00Z INFO Runner] Job build-test completed with result: Succeeded\n'
    assert M.parse_runner_log(log)[0] == 'idle'
    assert M.parse_runner_log('')[0] == '?'
    assert M.sources_in_cmdline(['/usr/bin/ccache', '/usr/bin/c++', '-O2', '-c', '/s/core/a.cpp', '-o', 'a.o']) == '/s/core/a.cpp'
    assert M.sources_in_cmdline(['/usr/lib/gcc/x86_64-linux-gnu/13/cc1plus', '-quiet', '../core/b.cc']) == '../core/b.cc'
    assert M.sources_in_cmdline(['python3', 'x.cpp']) is None
    assert M.sources_in_cmdline([]) is None


def test_proc_parsers():
    stat = 'cpu  10 0 10 80 0 0 0 0 0 0\ncpu0 5 0 5 40 0 0 0 0 0 0\ncpu1 5 0 5 40 0 0 0 0 0 0\nintr 1\n'
    tot, cores = M.parse_proc_stat(stat)
    assert tot == (100, 80) and len(cores) == 2
    mi = M.parse_meminfo('MemTotal:       16000 kB\nMemAvailable:    8000 kB\n')
    assert mi['MemTotal'] == 16000 * 1024


def test_fake_proc():
    d = tempfile.mkdtemp()
    proc = os.path.join(d, 'proc')
    os.makedirs(os.path.join(proc, '101'))
    os.makedirs(os.path.join(proc, '202'))

    def put(rel, data, mode='w'):
        with open(os.path.join(proc, rel), mode) as f:
            f.write(data)
    put('stat', 'cpu  0 0 0 0 0 0 0 0\n' + ''.join('cpu%d 0 0 0 0 0 0 0 0\n' % i for i in range(20)))
    put('meminfo', 'MemTotal: 22000000 kB\nMemAvailable: 11000000 kB\nSwapTotal: 8000000 kB\nSwapFree: 8000000 kB\n')
    put('loadavg', '12.50 9.00 4.00 3/900 1234\n')
    put('uptime', '5000.00 1000.00\n')
    put('101/cmdline', b'./bin/Runner.Listener\0run\0', 'wb')
    put('101/stat', '101 (Runner.Listener) S ' + ' '.join(['0'] * 18) + ' 100000 0 0\n')
    put('202/cmdline', b'/usr/bin/ccache\0/usr/bin/c++\0-c\0/s/core/src/ldpc.cpp\0-o\0x.o\0', 'wb')
    put('202/stat', '202 (c++) R ' + ' '.join(['0'] * 18) + ' 480000 0 0\n')
    s = M.Sys(proc=proc, sysd=os.path.join(d, 'sys'), runner=os.path.join(d, 'runner'))
    s.vitals()
    put('stat', 'cpu  50 0 0 50 0 0 0 0\n' + ''.join('cpu%d %d 0 0 %d 0 0 0 0\n' % (i, i * 5, 100 - i * 5)
                                                       for i in range(20)))
    v = s.vitals()
    assert len(v['cores']) == 20 and abs(v['cores'][10] - 0.5) < 1e-9 and abs(v['cpu_total'] - 0.5) < 1e-9, v
    assert v['mem_total'] == 22000000 * 1024 and v['swap_used'] == 0 and v['load'] == '12.50 9.00 4.00'
    r = s.runner_info()
    assert r['state'] == 'IDLE' and r['compiling'] and r['compiling'][0][1] == '/s/core/src/ldpc.cpp', r
    hz = os.sysconf('SC_CLK_TCK')
    assert abs(r['uptime'] - (5000.0 - 100000.0 / hz)) < 1e-6, r['uptime']


def check_frame(lines, W, H):
    assert len(lines) == H, (len(lines), H)
    for i, ln in enumerate(lines):
        want = W - 1 if i == H - 1 else W
        assert M.llen(ln) == want, (i, M.llen(ln), want, M.to_plain([ln]))


def test_layout_and_corrupt_state():
    d = tempfile.mkdtemp()
    # corrupt / partial state files must not crash
    with open(os.path.join(d, 'current.json'), 'w') as f:
        f.write('{"ref": "main", "phases": [{"name": "build", "start": "x"}, 5], "started": "oops"')
    with open(os.path.join(d, 'history.json'), 'w') as f:
        f.write('[1, "x", {"ref": null, "duration": "?", "started": "bad"}]')
    with open(os.path.join(d, 'build.log'), 'wb') as f:
        f.write(b'[10/\xff\xfe\n[5/20 1.0s] ok\n')
    sysobj = M.Sys(proc=os.path.join(d, 'noproc'), sysd=os.path.join(d, 'nosys'), runner=os.path.join(d, 'nr'))
    model = M.Model(d, sysobj, None)
    for W, H in ((120, 40), (100, 30), (80, 24), (60, 20), (200, 60)):
        for view in ('main', 'log', 'history'):
            m = model.collect(time.time())
            check_frame(M.render(model, m, W, H, view), W, H)
    # a valid running job (partial current.json written by the begin step)
    with open(os.path.join(d, 'current.json'), 'w') as f:
        json.dump({'ref': 'main', 'started': int(time.time()) - 30, 'phase': 'build',
                   'phases': [{'name': 'build', 'start': int(time.time()) - 10, 'end': None}]}, f)
    m = model.collect(time.time())
    txt = M.to_plain(M.render(model, m, 120, 40))
    assert 'CURRENT JOB' in txt and 'main' in txt and '5/20' in txt, txt
    # demo frames at every phase in both sizes, plus ASCII output is pure ASCII
    dd = tempfile.mkdtemp()
    demo = M.Demo(dd, t0=0)
    dm = M.Model(dd, sysobj, demo)
    for at in (1, 6, 20, 40, 60, 80, 90, 95, 101):
        for W, H in ((120, 40), (100, 30)):
            m = dm.collect(float(at))
            m['demo'] = True
            lines = M.render(dm, m, W, H)
            check_frame(lines, W, H)
            M.to_plain(lines, True).encode('ascii')


def test_state_writer():
    d = tempfile.mkdtemp()
    env = dict(os.environ, ONAIR_MONITOR_DIR=d)
    w = os.path.join(MON, 'onair_state.py')

    def run(*a):
        p = subprocess.run([sys.executable, w] + list(a), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert p.returncode == 0, p.stdout
        return p.stdout.decode()
    run('begin', '--workflow', 'Build and test', '--ref', 'main', '--targets', 'a b', '--tests', '^x', '--url', 'u')
    run('set', 'sha=abc1234', 'BAD KEY=1')
    run('phase', 'checkout')
    run('phase', 'build')
    cur = json.load(open(os.path.join(d, 'state', 'current.json')))
    assert cur['sha'] == 'abc1234' and cur['phase'] == 'build' and cur['phases'][0]['end'] is not None
    assert 'BAD KEY' not in cur
    run('end', '--result', 'failure')
    assert not os.path.exists(os.path.join(d, 'state', 'current.json'))
    h = json.load(open(os.path.join(d, 'state', 'history.json')))
    assert h[-1]['result'] == 'FAIL' and h[-1]['ref'] == 'main' and len(h[-1]['phases']) == 2
    # end without begin and garbage history never fail
    with open(os.path.join(d, 'state', 'history.json'), 'w') as f:
        f.write('{broken')
    run('end', '--result', 'success')
    run('phase', 'x')
    run('nonsense')
    # install copies monitor.py and keeps a newer installed copy
    run('install', MON)
    assert os.path.exists(os.path.join(d, 'monitor.py'))
    with open(os.path.join(d, 'monitor.py'), 'w') as f:
        f.write('VERSION = 999\n')
    assert 'newer' in run('install', MON)


if __name__ == '__main__':
    n = 0
    for name, fn in sorted(globals().items()):
        if name.startswith('test_') and callable(fn):
            fn()
            n += 1
            print('ok', name)
    print('all %d monitor tests passed' % n)
