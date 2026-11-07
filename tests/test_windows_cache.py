#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Keep dependency reuse independent of releases and downstream test outcomes."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from windows_cache import dependency_identity, inventory, save_decision


class WindowsCacheTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='koma-windows-cache-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / 'cmake/windows-fast').mkdir(parents=True)
        (self.root / 'cmake/triplets').mkdir()
        self.manifest = {'name': 'komamimic', 'version-string': '1.5.0',
                         'builtin-baseline': 'a' * 40, 'dependencies': ['zlib']}
        self.write_manifest()
        (self.root / 'cmake/WindowsFast.cmake').write_text('set(VCPKG_TARGET_TRIPLET x64-windows-release)\n', encoding='utf-8')
        (self.root / 'cmake/triplets/x64-windows-release.cmake').write_text('set(VCPKG_BUILD_TYPE release)\n', encoding='utf-8')
        self.cache = self.root / 'cache'
        self.cache.mkdir()
        self.family = 'windows-2022-release-vcpkg-v2-dependencies-'
        self.matched = self.family + 'image-123-1'

    def write_manifest(self):
        (self.root / 'cmake/windows-fast/vcpkg.json').write_text(json.dumps(self.manifest), encoding='utf-8')

    def key(self):
        return dependency_identity(self.root)['dependency-key']

    def package(self, name='a.zip', data=b'completed binary package'):
        path = self.cache / 'ab' / name
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(data)
        return path

    def decision(self, before, matched=None):
        return save_decision(before, inventory(self.cache), self.matched if matched is None else matched, self.family)

    def test_release_versions_and_root_metadata_do_not_expire_dependencies(self):
        key = self.key()
        for field in ('version', 'version-string', 'version-semver', 'version-date',
                      'port-version', 'name', 'description', 'license'):
            self.manifest[field] = 'changed metadata'
            self.write_manifest()
            self.assertEqual(self.key(), key, field)

    def test_dependency_changes_expire_family(self):
        key = self.key()
        for field, value in [('dependencies', [{'name': 'zlib', 'version>=': '2.0'}]),
                             ('builtin-baseline', 'b' * 40), ('default-features', ['pdf']),
                             ('features', {'pdf': {'dependencies': ['poppler']}}),
                             ('overrides', [{'name': 'zlib', 'version': '1.2.3'}]),
                             ('future-dependency-setting', True)]:
            previous = self.manifest.copy()
            self.manifest[field] = value
            self.write_manifest()
            self.assertNotEqual(self.key(), key, field)
            self.manifest = previous

    def test_json_format_and_checkout_line_endings_do_not_expire_family(self):
        key = self.key()
        path = self.root / 'cmake/windows-fast/vcpkg.json'
        path.write_bytes(json.dumps(self.manifest, indent=4, sort_keys=True).replace('\n', '\r\n').encode())
        for path in (self.root / 'cmake').rglob('*.cmake'):
            path.write_bytes(path.read_text(encoding='utf-8').replace('\n', '\r\n').encode('utf-8'))
        self.assertEqual(self.key(), key)

    def test_triplet_and_dependency_setup_are_inputs(self):
        for relative in ('cmake/triplets/x64-windows-release.cmake', 'cmake/WindowsFast.cmake'):
            path = self.root / relative
            original = path.read_bytes()
            key = self.key()
            path.write_bytes(original + b'set(NEW_DEPENDENCY_OPTION ON)\n')
            self.assertNotEqual(self.key(), key)
            path.write_bytes(original)

    def test_optional_registry_configuration_is_an_input(self):
        for relative in ('vcpkg-configuration.json', 'cmake/windows-fast/vcpkg-configuration.json'):
            key = self.key()
            path = self.root / relative
            path.write_text('{"registries": []}', encoding='utf-8')
            self.assertNotEqual(self.key(), key)

    def test_invalid_baseline_is_rejected(self):
        self.manifest['builtin-baseline'] = 'moving-branch'
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, 'baseline'):
            self.key()

    def test_empty_or_unfinished_cache_is_not_saved_after_failure(self):
        before = inventory(self.cache)
        self.package('unfinished.zip.tmp')
        self.package('lock')
        self.assertFalse(self.decision(before, '')['save'])
        self.assertEqual(self.decision(before, '')['packages'], 0)

    def test_new_completed_package_is_saved_without_requiring_test_success(self):
        before = inventory(self.cache)
        self.package()
        result = self.decision(before, '')
        self.assertTrue(result['save'])
        self.assertEqual(result['changed'], 1)
        self.assertEqual(result['packages'], 1)

    def test_unchanged_warm_cache_does_not_reupload(self):
        self.package()
        before = inventory(self.cache)
        result = self.decision(before)
        self.assertFalse(result['save'])
        self.assertFalse(result['migrate'])
        self.assertEqual(result['changed'], 0)

    def test_partial_generation_can_grow_on_next_attempt(self):
        self.package('first.zip')
        before = inventory(self.cache)
        self.package('second.zip')
        result = self.decision(before)
        self.assertTrue(result['save'])
        self.assertEqual(result['changed'], 1)
        self.assertEqual(result['packages'], 2)

    def test_repaired_archive_is_saved_even_if_size_is_unchanged(self):
        path = self.package()
        before = inventory(self.cache)
        stamp = path.stat().st_mtime_ns + 2_000_000_000
        os.utime(path, ns=(stamp, stamp))
        self.assertTrue(self.decision(before)['save'])

    def test_removed_bad_archive_is_not_restored_again(self):
        bad = self.package('bad.zip')
        self.package('good.zip')
        before = inventory(self.cache)
        bad.unlink()
        result = self.decision(before)
        self.assertTrue(result['save'])
        self.assertEqual(result['removed'], 1)
        self.assertEqual(result['packages'], 1)

    def test_existing_legacy_cache_migrates_without_rebuilding(self):
        self.package()
        before = inventory(self.cache)
        result = self.decision(before, 'windows-2022-release-vcpkg-old-manifest-image')
        self.assertTrue(result['save'])
        self.assertTrue(result['migrate'])
        self.assertEqual(result['changed'], 0)

    def test_compatible_fallback_gets_its_own_dependency_family(self):
        self.package()
        self.assertTrue(self.decision(inventory(self.cache), 'windows-2022-release-vcpkg-v2-other-dependencies-image-1')['migrate'])

    def test_snapshot_from_different_directory_is_rejected(self):
        before = inventory(self.cache)
        before['directory'] += '-different'
        with self.assertRaisesRegex(ValueError, 'snapshot'):
            self.decision(before)

    def test_identity_cli_emits_pinned_commit_and_version_independent_key(self):
        output = self.root / 'github-output'
        result = subprocess.run([sys.executable, str(ROOT / 'tools/windows_cache.py'), 'identity',
                                 '--root', str(self.root), '--github-output', str(output)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = dict(line.split('=', 1) for line in output.read_text().splitlines())
        self.assertEqual(lines['commit'], 'a' * 40)
        self.assertEqual(lines['dependency-key'], self.key())

    def test_assess_cli_records_restore_identity_and_save_output(self):
        snapshot, report, output = [self.root / name for name in ('before.json', 'report.json', 'outputs')]
        snapshot.write_text(json.dumps(inventory(self.cache)), encoding='utf-8')
        self.package()
        result = subprocess.run([sys.executable, str(ROOT / 'tools/windows_cache.py'), 'assess',
                                 '--directory', str(self.cache), '--before', str(snapshot),
                                 '--restored-key', self.matched, '--family-prefix', self.family,
                                 '--output', str(report), '--github-output', str(output)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_text().strip(), 'save=true')
        self.assertEqual(json.loads(report.read_text())['restored-key'], self.matched)

    def test_cold_snapshot_and_assess_cli_accept_empty_restore_key(self):
        snapshot, report, output = [self.root / name for name in ('before.json', 'report.json', 'outputs')]
        tool = str(ROOT / 'tools/windows_cache.py')
        captured = subprocess.run([sys.executable, tool, 'snapshot', '--directory', str(self.cache),
                                   '--output', str(snapshot)], capture_output=True, text=True)
        self.assertEqual(captured.returncode, 0, captured.stderr)
        self.assertEqual(json.loads(snapshot.read_text())['files'], {})
        self.package()
        assessed = subprocess.run([sys.executable, tool, 'assess', '--directory', str(self.cache),
                                   '--before', str(snapshot), '--restored-key=',
                                   '--family-prefix', self.family, '--output', str(report),
                                   '--github-output', str(output)], capture_output=True, text=True)
        self.assertEqual(assessed.returncode, 0, assessed.stderr)
        self.assertEqual(output.read_text().strip(), 'save=true')
        self.assertEqual(json.loads(report.read_text())['restored-key'], '')

    def test_workflow_preserves_failures_and_saves_changed_generations(self):
        source = (ROOT / '.github/workflows/build.yml').read_text(encoding='utf-8')
        windows = source[source.index('\n  windows:'):]
        sections = re.split(r'(?=^      - )', windows, flags=re.MULTILINE)
        validation = next(x for x in sections if 'id: validation\n' in x)
        save = next(x for x in sections if 'Save completed dependencies even after validation failure' in x)
        assess = next(x for x in sections if 'id: vcpkg-cache-state\n' in x)
        restore = next(x for x in sections if 'id: vcpkg-cache\n' in x)
        self.assertNotIn('continue-on-error', validation)
        self.assertIn('exit $LASTEXITCODE', validation)
        self.assertIn('!cancelled()', save)
        self.assertIn('!cancelled()', assess)
        self.assertIn('--restored-key="$env:RESTORED_DEPENDENCY_KEY"', assess)
        self.assertIn("steps.vcpkg-cache-state.outputs.save == 'true'", save)
        self.assertIn('cache-primary-key', save)
        self.assertIn('github.run_id', restore)
        self.assertIn('github.run_attempt', restore)
        self.assertLess(windows.index('id: vcpkg-before'), windows.index('id: validation'))
        self.assertLess(windows.index('id: validation'), windows.index('id: vcpkg-cache-state'))
        self.assertLess(windows.index('Save completed dependencies even after validation failure'), windows.index('Stage portable validation ZIP'))
        self.assertNotIn('save-always', windows)

    def test_verified_sdk_is_saved_before_later_failure(self):
        source = (ROOT / '.github/workflows/build.yml').read_text(encoding='utf-8')
        windows = source[source.index('\n  windows:'):]
        self.assertLess(windows.index('Install or verify prebuilt Qt'), windows.index('Save verified Qt SDK'))
        self.assertLess(windows.index('Save verified Qt SDK'), windows.index('id: validation'))


if __name__ == '__main__':
    unittest.main()
