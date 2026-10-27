#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('check_title_imports', pathlib.Path(__file__).with_name('check-title-imports.py'))
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class ImportAuditTests(unittest.TestCase):
    def check(self, module, symbol, exported, surface):
        def fake(*args):
            if args[:2] == ('readelf', '-dW'):
                if args[2] == 'title':
                    return f' (NEEDED) Shared library: [{module}]\n'
                return f' (SONAME) Library soname: [{module}]\n'
            if args[:3] == ('nm', '-D', '--defined-only'):
                return f'0000 T {symbol}\n' if exported else ''
            if args[:3] == ('readelf', '--dyn-syms', '-W'):
                return f'  1: 0 0 FUNC GLOBAL DEFAULT UND {symbol}\n'
            raise AssertionError(args)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'surface'
            path.write_text(surface)
            with patch.object(audit, 'command', fake), patch.object(sys, 'argv', ['audit', 'title', str(path), 'stub']):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    return audit.main()

    def test_system_libc_uses_its_own_stub_not_libc_prx_surface(self):
        self.assertEqual(self.check('libSceLibcInternal.sprx', '__stderrp', True, ''), 0)

    def test_missing_module_export_fails(self):
        self.assertEqual(self.check('libSceLibcInternal.sprx', 'unknown_symbol', False, ''), 1)

    def test_libc_prx_requires_surface_nid(self):
        self.assertEqual(self.check('libc.prx', 'missing_symbol', True, ''), 1)
        self.assertEqual(self.check('libc.prx', 'present_symbol', True, audit.nid('present_symbol') + '|1|2|0\n'), 0)


if __name__ == '__main__':
    unittest.main()
