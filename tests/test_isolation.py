# SPDX-License-Identifier: GPL-3.0-only
"""Automated probes cannot inherit a developer's writable OS directories."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from isolated_environment import isolated_environment


class IsolationTests(unittest.TestCase):
    def test_all_platform_directories_are_private_and_parent_is_unchanged(self):
        for system in ('linux', 'darwin', 'win32'):
            with self.subTest(system=system), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                outside = root / 'normal-profile'
                outside.mkdir()
                marker = outside / 'settings.ini'
                marker.write_bytes(b'normal user settings')
                incoming = dict.fromkeys(('HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME',
                    'XDG_DATA_HOME', 'XDG_STATE_HOME', 'XDG_RUNTIME_DIR', 'TMPDIR',
                    'TMP', 'TEMP', 'CFFIXED_USER_HOME', 'USERPROFILE', 'APPDATA',
                    'LOCALAPPDATA'), str(outside))
                incoming.update(PATH='host-tools', QT_PLUGIN_PATH='deployed-plugins')
                snapshot = dict(incoming)
                private = root / 'isolated'
                env = isolated_environment(private, incoming, system)
                names = ['HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_DATA_HOME',
                         'XDG_STATE_HOME', 'XDG_RUNTIME_DIR', 'TMPDIR', 'TMP', 'TEMP']
                names += ['CFFIXED_USER_HOME'] if system == 'darwin' else []
                names += ['USERPROFILE', 'APPDATA', 'LOCALAPPDATA'] if system == 'win32' else []
                for key in names:
                    self.assertTrue(Path(env[key]).is_relative_to(private), key)
                    self.assertTrue(Path(env[key]).is_dir(), key)
                self.assertEqual(env['PATH'], 'host-tools')
                self.assertEqual(env['QT_PLUGIN_PATH'], 'deployed-plugins')
                self.assertEqual(incoming, snapshot)
                self.assertEqual(marker.read_bytes(), b'normal user settings')
                self.assertEqual(list(outside.iterdir()), [marker])

    def test_each_probe_gets_a_different_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            a, b = (isolated_environment(root / name, {}) for name in ('a', 'b'))
            for key in ('HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'TMPDIR'):
                self.assertNotEqual(a[key], b[key])
