#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Tests for tools/check_bandplan_br.py on small made-up ZIPs shaped like Anatel's planos_basicos.zip.

  python3 -m unittest tools/test_check_bandplan_br.py"""
import os, subprocess, sys, tempfile, unittest, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHECKER = os.path.join(HERE, 'check_bandplan_br.py')

FM = 'Canal;Frequência;Medida;Município\n201;88,1;MHz;Florianópolis - SC\n300;107,9;MHz;Porto Alegre - RS\n'
AM = 'Frequência;Medida;Município\n1000;kHz;São Paulo - SP\n'
TV = 'Canal;Frequência;Medida;Município\n14;473;MHz;Curitiba - PR\n'


class CheckerTest(unittest.TestCase):
    def run_on(self, files):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'planos_basicos.zip')
            with zipfile.ZipFile(path, 'w') as z:
                for name, text in files.items():
                    z.writestr(name, text.encode('utf-8'))
            r = subprocess.run([sys.executable, '-I', CHECKER, path], capture_output=True, text=True)
            return r.returncode, r.stdout + r.stderr

    def test_rows_on_the_plan_pass(self):
        rc, out = self.run_on({'PBFM.csv': FM, 'PBOM.csv': AM, 'PBTVD.csv': TV})
        self.assertEqual(rc, 0, out)

    def test_files_in_a_folder_and_lower_case_are_found(self):
        rc, out = self.run_on({'dados/pbfm.csv': FM, 'dados/PBOM.CSV': AM, 'PBTVD.csv': TV})
        self.assertEqual(rc, 0, out)
        self.assertNotIn('not in the ZIP', out)

    def test_a_missing_file_fails(self):
        rc, out = self.run_on({'PBFM.csv': FM, 'PBOM.csv': AM})
        self.assertEqual(rc, 1, out)
        self.assertIn('PBTVD.csv: not in the ZIP', out)

    def test_a_changed_layout_fails(self):
        rc, out = self.run_on({'PBFM.csv': 'Numero;Freq\n201;88,1\n', 'PBOM.csv': AM, 'PBTVD.csv': TV})
        self.assertEqual(rc, 1, out)
        self.assertIn('no channel or frequency column', out)

    def test_an_empty_file_fails(self):
        rc, out = self.run_on({'PBFM.csv': '', 'PBOM.csv': AM, 'PBTVD.csv': TV})
        self.assertEqual(rc, 1, out)

    def test_a_row_off_the_plan_fails(self):
        rc, out = self.run_on({'PBFM.csv': FM + '201;95,0;MHz;Joinville - SC\n', 'PBOM.csv': AM, 'PBTVD.csv': TV})
        self.assertEqual(rc, 1, out)
        self.assertIn('channel 201 listed at 95.000 MHz', out)


if __name__ == '__main__':
    unittest.main()
