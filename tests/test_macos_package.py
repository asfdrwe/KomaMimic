#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Cross-host tests of bundle integrity and the native DMG command pipeline."""
import contextlib
import io
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import package_macos as package
import stage_validation as stage_tool


class MacPackageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='koma-macos-package-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.stage = self.root / 'stage'
        self.app = self.stage / 'KomaMimic.app'
        for name in (*package.REQUIRED, 'Contents/Resources/qtwebengine_resources.pak',
                     'Contents/Resources/icudtl.dat', 'Contents/Resources/qtwebengine_locales/en-US.pak',
                     'Contents/Resources/qtwebengine_locales/ja.pak',
                     'Contents/Frameworks/QtWebEngineCore.framework/Versions/A/Helpers/QtWebEngineProcess'):
            path = self.app / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'resource fixture')
        for path in (self.app / 'Contents/MacOS').iterdir():
            path.chmod(0o755)
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
            CFBundleExecutable='komamimic', CFBundleShortVersionString='1.0.0')))
        self.build = self.root / 'build'
        self.build.mkdir()
        (self.build / 'CMakeCache.txt').write_text(
            'CMAKE_HOME_DIRECTORY:INTERNAL=' + ROOT.as_posix() + '\n'
            'CMAKE_OSX_ARCHITECTURES:STRING=arm64\nCMAKE_BUILD_TYPE:STRING=Release\n')
        (self.stage / 'validation_probe').write_bytes(b'deployed probe')

    def link(self, path, target):
        try:
            path.symlink_to(target)
        except OSError as error:
            if sys.platform == 'win32' and error.winerror == 1314:
                self.skipTest('Windows symlink privilege is unavailable')
            raise

    def manifest(self):
        package.save_json(self.stage / 'SOURCE_MANIFEST.json', dict(files=[
            dict(path=p.relative_to(ROOT).as_posix(), sha256=package.digest(p)) for p in package.public_files(ROOT)]))
        files = [dict(path=p.relative_to(self.stage).as_posix(), sha256=package.digest(p))
                 for p in sorted(self.stage.rglob('*')) if p.is_file() and not p.is_symlink()]
        package.save_json(self.stage / 'STAGE_MANIFEST.json', dict(system='Darwin',
            status='STAGED_NOT_MANUALLY_VERIFIED', files=files,
            source_manifest_sha256=package.digest(self.stage / 'SOURCE_MANIFEST.json'),
            symlinks={p.relative_to(self.stage).as_posix(): os.readlink(p)
                      for p in self.stage.rglob('*') if p.is_symlink()}))

    def test_inventory_preserves_executable_modes_and_rejects_escaping_links(self):
        binary = self.app / 'Contents/MacOS/komamimic'
        before = package.inventory(self.app)
        binary.chmod(0o444)
        self.assertNotEqual(package.inventory(self.app), before)
        binary.chmod(0o755)
        alias = self.app / 'Contents/MacOS/alias'
        self.link(alias, binary.name)
        self.assertIn(dict(path='Contents/MacOS/alias', link=binary.name), package.inventory(self.app))
        alias.unlink()
        self.link(alias, '../../../outside')
        with self.assertRaisesRegex(ValueError, 'Unsafe bundle symlink'):
            package.inventory(self.app)

    def test_stage_rejects_changed_source_inventory_and_probe(self):
        self.manifest()
        app, _ = package.verified_stage(self.build, self.stage)
        self.assertEqual(app, self.app)
        extra = self.app / 'unrecorded'
        extra.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'inventory changed'):
            package.verified_stage(self.build, self.stage)
        extra.unlink()
        (self.stage / 'validation_probe').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'Stage changed'):
            package.verified_stage(self.build, self.stage)
        (self.stage / 'validation_probe').write_bytes(b'deployed probe')
        package.save_json(self.stage / 'SOURCE_MANIFEST.json', dict(files=[]))
        info = json.loads((self.stage / 'STAGE_MANIFEST.json').read_text())
        checksum = package.digest(self.stage / 'SOURCE_MANIFEST.json')
        info['source_manifest_sha256'] = checksum
        for record in info['files']:
            if record['path'] == 'SOURCE_MANIFEST.json':
                record['sha256'] = checksum
        package.save_json(self.stage / 'STAGE_MANIFEST.json', info)
        with self.assertRaisesRegex(ValueError, 'different source contents'):
            package.verified_stage(self.build, self.stage)

    def test_rpath_and_executable_loads_resolve_only_to_bundled_or_system_code(self):
        binary = self.app / 'Contents/MacOS/komamimic'
        library = self.app / 'Contents/Frameworks/libexample.dylib'
        library.write_bytes(b'fixture')
        records = {
            binary: dict(dependencies=['@rpath/libexample.dylib', '/usr/lib/libSystem.B.dylib'],
                         rpaths=['@executable_path/../Frameworks']),
            library: dict(dependencies=[], rpaths=[])}
        package.audit_dependencies(self.app, records)
        records[binary]['rpaths'] = ['/opt/homebrew/lib']
        with self.assertRaisesRegex(ValueError, 'External or unsupported rpath'):
            package.audit_dependencies(self.app, records)
        records[binary]['rpaths'] = ['@loader_path/../Frameworks']
        library.unlink()
        records.pop(library)
        with self.assertRaisesRegex(ValueError, 'Unresolved bundled load'):
            package.audit_dependencies(self.app, records)

    def test_relocation_removes_sdk_rpaths_adds_internal_rpath_and_keeps_loads(self):
        binary = self.app / 'Contents/MacOS/komamimic'
        records = {binary: dict(identifier='', dependencies=['@rpath/libexample.dylib'],
                                rpaths=['/opt/homebrew/opt/example/lib', '/opt/homebrew/opt/example/lib'])}
        plan = stage_tool.macos_relocation_plan(self.app, records)
        self.assertEqual(plan[0]['arguments'], ['-delete_rpath', '/opt/homebrew/opt/example/lib',
                                               '-add_rpath', '@loader_path/../Frameworks'])
        text = 'cmd LC_RPATH\npath /opt/homebrew/lib (offset 12)\n'
        self.assertEqual(stage_tool.macho_commands(text + text)['rpaths'], ['/opt/homebrew/lib'])

    def test_audit_rejects_missing_arm64_and_missing_resources(self):
        binary = self.app / 'Contents/MacOS/komamimic'
        records = {binary: dict(identifier='', dependencies=['/usr/lib/libSystem.B.dylib'], rpaths=[])}
        output = self.root / 'audit'
        output.mkdir()
        with (mock.patch.object(package, 'inspect_macos', return_value=records),
              mock.patch.object(package.subprocess, 'check_output', return_value='x86_64')):
            with self.assertRaisesRegex(ValueError, 'arm64'):
                package.audit(self.app, output, mock.Mock())
        (self.app / 'Contents/Resources/qtwebengine_locales/ja.pak').unlink()
        with self.assertRaisesRegex(ValueError, 'Incomplete macOS bundle'):
            package.audit(self.app, output, mock.Mock())

    def test_notices_keep_originals_and_include_transitive_formulae(self):
        formulae = [dict(name='icu4c@78' if n == 'icu4c' else n,
                        aliases=['icu4c'] if n == 'icu4c' else [],
                        dependencies=['dependency'] if n == 'qtbase' else [])
                    for n in (*package.RUNTIME_FORMULAE, 'dependency')]
        prefixes = {}
        for formula in formulae:
            path = self.root / 'cellar' / formula['name']
            path.mkdir(parents=True)
            (path / 'LICENSE').write_bytes(b'Original license\r\n')
            prefixes[formula['name']] = path
        def query(command, **kwargs):
            if command[1] == 'info':
                return json.dumps(dict(formulae=formulae))
            return '\n'.join(str(prefixes[n]) for n in command[2:])
        with mock.patch.object(package.subprocess, 'check_output', side_effect=query):
            result = package.collect_notices(self.root / 'notices', self.root)
        self.assertIn('dependency/LICENSE', result['files'])
        self.assertEqual((self.root / 'notices/qtbase/LICENSE').read_bytes(), b'Original license\r\n')
        self.assertEqual(result['final_license_and_corresponding_source_review'], 'PENDING')

    def test_native_probe_uses_cocoa_removes_sdk_and_keeps_timeout_diagnostics(self):
        output = self.root / 'verify'
        output.mkdir()
        calls = []
        def call(name, command, **options):
            calls.append((name, options))
            (output / (name + '.log')).write_text('KomaMimic 1.0.0\n')
        def timeout(command, **options):
            self.assertEqual(options['env']['QT_QPA_PLATFORM'], 'cocoa')
            self.assertNotIn('DYLD_LIBRARY_PATH', options['env'])
            options['stderr'].write('BEGIN QGuiApplication\n')
            raise subprocess.TimeoutExpired(command, options['timeout'])
        with (mock.patch.dict(package.os.environ, dict(DYLD_LIBRARY_PATH='/developer/lib', QT_PLUGIN_PATH='/sdk/plugins')),
              mock.patch.object(package.subprocess, 'run', side_effect=timeout)):
            with self.assertRaises(subprocess.TimeoutExpired):
                package.native_verify(self.app, self.stage / 'validation_probe', output, call, '1.0.0')
        self.assertFalse((self.app / 'Contents/MacOS/validation_probe').exists())
        self.assertEqual((output / 'native-readers-stderr.log').read_text(), 'BEGIN QGuiApplication\n')
        self.assertNotIn('DYLD_PRINT_LIBRARIES', calls[-1][1]['env'])

    @unittest.skipIf(sys.platform == 'win32', 'DMG pipeline fixture requires POSIX application symlinks')
    def test_pack_roundtrip_and_failure_detach_gate(self):
        self.link(self.root / 'capability', 'target')
        self.manifest()
        for fail in (False, True):
            output = self.root / ('fail' if fail else 'pass')
            seen = []
            volume = output / 'work/dmg-root'
            app = volume / 'KomaMimic.app'
            def execute(command, log, **kwargs):
                command = list(map(str, command))
                seen.append(command)
                log.write_text('command log\n')
                if command[0] == 'ditto':
                    if command[1] == '-c':
                        Path(command[-1]).write_bytes(b'app archive fixture')
                    elif command[1] == '-x':
                        shutil.copytree(app, Path(command[-1]) / app.name, symlinks=True)
                    else:
                        shutil.copytree(command[1], command[2], symlinks=True)
                elif command[:2] == ['hdiutil', 'create']:
                    Path(command[-1]).write_bytes(b'dmg fixture')
                elif command[:2] == ['hdiutil', 'attach']:
                    mount = Path(command[command.index('-mountpoint') + 1])
                    shutil.copytree(volume, mount, dirs_exist_ok=True, symlinks=True)
                return dict(returncode=0)
            def notices(destination, output):
                destination.mkdir(parents=True)
                (destination / 'NOTICE').write_bytes(b'original license')
                package.save_json(output / 'HOMEBREW.json', {})
                return dict(final_license_and_corresponding_source_review='PENDING')
            def runtime(*args):
                if fail:
                    raise ValueError('runtime failure')
            def sources(root, output, epoch):
                output.mkdir()
                (output / 'source.tar.gz').write_bytes(b'source fixture')
            with (mock.patch.object(package.platform, 'system', return_value='Darwin'),
                  mock.patch.object(package.platform, 'machine', return_value='arm64'),
                  mock.patch.object(package, 'execute', side_effect=execute),
                  mock.patch.object(package, 'collect_notices', side_effect=notices),
                  mock.patch.object(package, 'audit', return_value={}),
                  mock.patch.object(package, 'signing_entitlements', return_value={'jit': True}),
                  mock.patch.object(package, 'native_verify', side_effect=runtime),
                  mock.patch.object(package, 'prepare', side_effect=sources),
                  mock.patch.object(package.subprocess, 'check_output', side_effect=lambda c, **k: '' if 'status' in c else 'a' * 40),
                  contextlib.redirect_stdout(io.StringIO())):
                if fail:
                    with self.assertRaisesRegex(ValueError, 'runtime failure'):
                        package.pack(self.build, self.stage, output)
                else:
                    package.pack(self.build, self.stage, output)
            self.assertTrue(any(c[:2] == ['hdiutil', 'detach'] for c in seen))
            self.assertEqual((output / 'SHA256SUMS').exists(), not fail)
            self.assertEqual((output / 'sources').exists(), not fail)
            self.assertFalse(any('--deep' in c and '--sign' in c for c in seen))
            if not fail:
                manifest = json.loads((output / 'BUILD_MANIFEST.json').read_text())
                self.assertEqual(manifest['notarization'], 'NOT_PERFORMED')
                self.assertFalse(manifest['source_tree_dirty'])
                self.assertNotIn('Contents/MacOS/validation_probe', [r['path'] for r in manifest['files']])

    def test_native_success_removes_probe_and_rejects_homebrew_loads(self):
        for leak in (False, True):
            output = self.root / ('leaked' if leak else 'isolated')
            output.mkdir()
            def call(name, command, **options):
                (output / (name + '.log')).write_text('KomaMimic 1.0.0\n')
            def run(command, **options):
                options['stdout'].write('{"all_passed":true}')
                options['stderr'].write('dyld[123]: <UUID> ' +
                    ('/opt/homebrew/lib/example.dylib' if leak else '/usr/lib/libSystem.B.dylib'))
                return mock.Mock(returncode=0)
            with mock.patch.object(package.subprocess, 'run', side_effect=run):
                if leak:
                    with self.assertRaisesRegex(ValueError, 'Homebrew library'):
                        package.native_verify(self.app, self.stage / 'validation_probe', output, call, '1.0.0')
                else:
                    package.native_verify(self.app, self.stage / 'validation_probe', output, call, '1.0.0')
            self.assertFalse((self.app / 'Contents/MacOS/validation_probe').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
