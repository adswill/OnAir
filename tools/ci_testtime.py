#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""ctest test launcher that records wall and CPU seconds per test (docs/CI-SPEED-4.7.md).

Configure with  -DCMAKE_TEST_LAUNCHER="python3;<src>/tools/ci_testtime.py"  (CMake 3.29+) and set ONAIR_TESTTIME_LOG to a file: every test
run appends one line  "<wall> <user> <sys> <exit> <program> <args...>"  (CPU of the test and the children it waited for). The test's exit
code, output and signals pass through unchanged; without ONAIR_TESTTIME_LOG it only runs the test.
tools/ci_times.py --cpu reads these lines: a test whose CPU is far below its wall time is waiting (real time, sleeps, start-up), one whose
CPU is near or above it is computing.
"""
import os, resource, subprocess, sys, time

def main():
    if len(sys.argv) < 2:
        sys.exit("ci_testtime.py: no command")
    cmd = sys.argv[1:]
    log = os.environ.get("ONAIR_TESTTIME_LOG")
    r0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.monotonic()
    try:
        p = subprocess.Popen(cmd)
    except OSError as e:
        print("ci_testtime.py: %s: %s" % (cmd[0], e), file=sys.stderr)
        sys.exit(127)
    try:
        rc = p.wait()
    except KeyboardInterrupt:
        p.terminate(); rc = p.wait()
    wall = time.monotonic() - t0
    r1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    if log:
        try:
            with open(log, "a") as f:
                # tests of one program differ by their arguments or their ONAIR_* / QA_* environment (ctest ENVIRONMENT): both are logged
                envs = sorted("%s=%s" % (k, v) for k, v in os.environ.items() if k.startswith(("ONAIR_", "QA_")) and k != "ONAIR_TESTTIME_LOG")
                f.write("%.2f %.2f %.2f %d %s\n" % (wall, r1.ru_utime - r0.ru_utime, r1.ru_stime - r0.ru_stime, rc,
                                                     " ".join([os.path.basename(cmd[0])] + cmd[1:] + envs)))
        except OSError:
            pass
    # a test killed by a signal: the same signal for ctest (it reports a crash, not exit code -11)
    if rc < 0:
        os.kill(os.getpid(), -rc)
    sys.exit(rc)

if __name__ == "__main__":
    main()
