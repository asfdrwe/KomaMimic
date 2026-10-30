#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Cross-host Windows packaging checks; fixtures are synthetic AMD64 PEs."""
import contextlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import package_windows as package


def synthetic_pe(imports=(), delay_imports=()):
    data = bytearray(4096)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 128)
    data[128:134] = b'PE\0\0\x64\x86'
    struct.pack_into('<H', data, 134, 1)
    struct.pack_into('<H', data, 148, 240)
    optional = 152
    struct.pack_into('<H', data, optional, 0x20b)
    struct.pack_into('<I', data, optional + 108, 16)
    struct.pack_into('<IIII', data, optional + 240 + 8, 3584, 0x1000, 3584, 512)
    text = 2048
    for names, index, start, size, name_index in ((imports, 1, 512, 20, 3), (delay_imports, 13, 1024, 32, 1)):
        if not names:
            continue
        struct.pack_into('<II', data, optional + 112 + index * 8, start + 0xe00, size * (len(names) + 1))
        for i, name in enumerate(names):
            if index == 13:
                struct.pack_into('<I', data, start + i * size, 1)
            struct.pack_into('<I', data, start + i * size + name_index * 4, text + 0xe00)
            encoded = name.encode('ascii') + b'\0'
            data[text:text + len(encoded)] = encoded
            text += len(encoded)
    return bytes(data)


class WindowsPackageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='koma-windows-package-')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.app = self.directory / 'app'
        for name in package.REQUIRED:
            path = self.app / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(synthetic_pe(('kernel32.dll', 'MSVCP140.dll'))
                             if path.suffix in ('.exe', '.dll') else b'resource')

    def test_normal_and_delay_imports_and_architecture(self):
        self.assertEqual(package.pe_imports(synthetic_pe(('Qt6Core.dll',), ('optional.dll',))),
                         dict(imports=['qt6core.dll'], delay_imports=['optional.dll']))
        for data in (b'MZ', b'not PE', synthetic_pe()[:100], synthetic_pe().replace(b'\x64\x86', b'\x64\xaa', 1)):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                package.pe_imports(data)
        data = bytearray(synthetic_pe(('normal.dll',)))
        struct.pack_into('<I', data, 152 + 112 + 8, 0x9000)
        with self.assertRaisesRegex(ValueError, 'RVA'):
            package.pe_imports(data)

    def test_audit_rejects_missing_delay_dependency_and_wrong_search_directory(self):
        worker = self.app / 'komamimic-script-worker.exe'
        worker.write_bytes(synthetic_pe(('Qt6Core.dll',), ('missing.dll',)))
        with self.assertRaisesRegex(ValueError, 'missing.dll'):
            package.audit(self.app)
        # A sibling plugin directory does not satisfy the worker's DLL search.
        (self.app / 'platforms/missing.dll').write_bytes(synthetic_pe())
        with self.assertRaisesRegex(ValueError, 'missing.dll'):
            package.audit(self.app)
        (self.app / 'missing.dll').write_bytes(synthetic_pe())
        report = package.audit(self.app)
        self.assertEqual(report['msvc_prerequisites'], ['msvcp140.dll'])
        (self.app / 'Qt6Sql.dll').unlink()
        with self.assertRaisesRegex(ValueError, 'Qt6Sql.dll'):
            package.audit(self.app)

    def test_trim_excludes_unavailable_external_client_plugins_only(self):
        for name in ('position/qtposition_nmea.dll', 'qmltooling/debug.dll', 'sqldrivers/qsqloci.dll'):
            path = self.app / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(synthetic_pe(('not-bundled.dll',)))
        with self.assertRaisesRegex(ValueError, 'not-bundled'):
            package.audit(self.app)
        package.trim_plugins(self.app)
        self.assertFalse((self.app / 'position').exists())
        self.assertFalse((self.app / 'qmltooling').exists())
        self.assertEqual([p.name for p in (self.app / 'sqldrivers').iterdir()], ['qsqlite.dll'])
        package.audit(self.app)

    def test_notices_preserve_originals_and_report_qt_absence(self):
        prefix, sdk, destination = (self.directory / p for p in ('prefix', 'sdk', 'notices'))
        original = b'Original Copyright\r\nLicense text\r\n'
        for name in ('share/libarchive/copyright', 'share/libarchive/vcpkg.spdx.json'):
            path = prefix / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(original)
        sdk.mkdir()
        self.assertFalse(package.collect_notices(prefix, sdk, destination)['qt_notices_found'])
        legal = sdk / 'LICENSES/LGPL-3.0-only.txt'
        legal.parent.mkdir()
        legal.write_bytes(original)
        self.assertTrue(package.collect_notices(prefix, sdk, destination)['qt_notices_found'])
        self.assertEqual((destination / 'qt/LICENSES/LGPL-3.0-only.txt').read_bytes(), original)
        with self.assertRaisesRegex(ValueError, 'copyright'):
            package.collect_notices(self.directory / 'empty', sdk, destination)

    def manifest(self):
        package.save_json(self.app / 'BUILD_MANIFEST.json', dict(files=package.inventory(self.app),
                                                              pe_dependencies=package.audit(self.app)))

    def test_zip_roundtrip_hashes_reproducibility_and_tampering(self):
        self.manifest()
        first, second = self.directory / 'first.zip', self.directory / 'second.zip'
        package.write_zip(self.app, first)
        package.write_zip(self.app, second)
        self.assertEqual(package.digest(first), package.digest(second))
        extracted = package.extract_verified(first, self.directory / 'extract')
        self.assertEqual(package.inventory(self.app), package.inventory(extracted))
        (self.app / 'README.md').write_bytes(b'changed')
        changed = self.directory / 'changed.zip'
        package.write_zip(self.app, changed)
        with self.assertRaisesRegex(ValueError, 'payload differs'):
            package.extract_verified(changed, self.directory / 'changed-extract')

    def test_extraction_rejects_unsafe_duplicate_and_symlink_entries(self):
        for index, names in enumerate((['../escape'], ['C:/escape'], ['app/a', 'app/A'], ['app/link'])):
            archive = self.directory / f'unsafe-{index}.zip'
            with zipfile.ZipFile(archive, 'w') as stream:
                for name in names:
                    entry = zipfile.ZipInfo(name)
                    if name == 'app/link':
                        entry.external_attr = 0o120777 << 16
                    stream.writestr(entry, b'x')
            with self.assertRaises(ValueError):
                package.extract_verified(archive, self.directory / f'extract-{index}')

    def test_pack_source_identity_symbols_and_failure_gate(self):
        build, stage, sdk = (self.directory / p for p in ('build', 'stage', 'sdk'))
        build.mkdir()
        stage.mkdir()
        self.app.rename(stage / 'app')
        self.app = stage / 'app'
        (self.app / 'komamimic.pdb').write_bytes(b'symbols')
        (build / 'CMakeCache.txt').write_text(
            f'CMAKE_HOME_DIRECTORY:INTERNAL={ROOT.as_posix()}\nKOMAMIMIC_WINDOWS_FAST:BOOL=ON\n'
            f'KOMAMIMIC_QT_ROOT:PATH={sdk.as_posix()}\nVCPKG_TARGET_TRIPLET:STRING=x64-windows-release\n', encoding='utf-8')
        copyright_file = build / 'vcpkg_installed/x64-windows-release/share/test/copyright'
        copyright_file.parent.mkdir(parents=True)
        copyright_file.write_bytes(b'Original notice')
        sdk.mkdir()
        package.save_json(stage / 'QT_SDK.json', {})
        (stage / 'vcpkg-status.txt').write_text('installed test package', encoding='utf-8')
        source = [dict(path=p.relative_to(ROOT).as_posix(), sha256=package.digest(p)) for p in package.public_files(ROOT)]
        package.save_json(stage / 'SOURCE_MANIFEST.json', dict(files=source))
        info = dict(system='Windows', status='STAGED_NOT_MANUALLY_VERIFIED',
                    source_manifest_sha256=package.digest(stage / 'SOURCE_MANIFEST.json'), files=package.inventory(stage))
        package.save_json(stage / 'STAGE_MANIFEST.json', info)
        def verify(archive, build, output):
            package.extract_verified(archive, output / 'extracted')
        def git(command, **options):
            return '1' * 40 + '\n' if command[-1] == 'HEAD' else ''
        with mock.patch.object(package.platform, 'system', return_value='Windows'), mock.patch.object(package, 'native_verify', side_effect=verify), mock.patch.object(package.subprocess, 'check_output', side_effect=git), contextlib.redirect_stdout(io.StringIO()):
            output = self.directory / 'dist'
            package.pack(build, stage, output)
            manifest = json.loads((output / 'BUILD_MANIFEST.json').read_text(encoding='utf-8'))
            self.assertEqual(manifest['source_files'], source)
            self.assertEqual(manifest['notices']['final_license_and_corresponding_source_review'], 'PENDING')
            self.assertEqual(manifest['manual_verification'], 'PENDING')
            runtime = next(p for p in output.glob('*.zip') if not p.name.endswith('-symbols.zip'))
            with zipfile.ZipFile(runtime) as stream:
                self.assertFalse(any(n.endswith(('.pdb', '.log')) for n in stream.namelist()))
                self.assertIn('vc_redist.x64.exe', stream.read(runtime.stem + '/配布情報.txt').decode('utf-8-sig'))
            with zipfile.ZipFile(next((output / 'symbols').glob('*-symbols.zip'))) as stream:
                self.assertTrue(any(n.endswith('/komamimic.pdb') for n in stream.namelist()))
            self.assertEqual(len(list((output / 'sources').glob('*.tar.gz'))), 1)
            for line in (output / 'SHA256SUMS').read_text().splitlines():
                digest, name = line.split('  ')
                self.assertEqual(package.digest(output / name), digest)
            with mock.patch.object(package, 'native_verify', side_effect=RuntimeError('native failure')):
                failed = self.directory / 'failed'
                with self.assertRaisesRegex(RuntimeError, 'native failure'):
                    package.pack(build, stage, failed)
                self.assertFalse((failed / 'SHA256SUMS').exists())
            (self.app / 'unexpected.txt').write_bytes(b'unrecorded')
            with self.assertRaisesRegex(ValueError, 'inventory changed'):
                package.pack(build, stage, self.directory / 'extra')

    def test_native_checks_use_extracted_payload_and_clean_environment(self):
        build = self.directory / 'build'
        (build / 'Release').mkdir(parents=True)
        (build / 'Release/validation_probe.exe').write_bytes(synthetic_pe())
        self.manifest()
        manifest = json.loads((self.app / 'BUILD_MANIFEST.json').read_text())
        manifest['version'] = '1.0.0'
        package.save_json(self.app / 'BUILD_MANIFEST.json', manifest)
        archive = self.directory / 'runtime.zip'
        package.write_zip(self.app, archive)
        seen = []
        def execute(command, log, **options):
            seen.append((command, options))
            log.write_text('command\nKomaMimic 1.0.0\n', encoding='utf-8')
            return dict(returncode=0)
        def probe(command, **options):
            seen.append((command, options))
            options['stdout'].write('{"all_passed":true}')
            options['stderr'].write('Qt diagnostic')
            return mock.Mock(returncode=0)
        with (mock.patch.dict(package.os.environ, dict(SystemRoot='C:/Windows', PATH='developer/sdk',
                             QT_PLUGIN_PATH='developer/plugins', QTWEBENGINE_DISABLE_SANDBOX='1', SDL_DYNAMIC_API='poison')),
                mock.patch.object(package, 'execute', side_effect=execute),
                mock.patch.object(package.subprocess, 'run', side_effect=probe)):
            package.native_verify(archive, build, self.directory / 'verify')
        self.assertEqual(len(seen), 4)
        for command, options in seen:
            self.assertNotIn('developer', options['env']['PATH'])
            self.assertNotIn('QT_PLUGIN_PATH', list(options['env']))
            self.assertNotIn('QTWEBENGINE_DISABLE_SANDBOX', list(options['env']))
            self.assertNotIn('SDL_DYNAMIC_API', list(options['env']))
            self.assertEqual(options['env']['QT_QPA_PLATFORM'], 'windows')
            self.assertTrue((self.directory / 'verify/extracted' / self.app.name / 'platforms/qwindows.dll').is_file())
            self.assertIn('extracted', ' '.join(map(str, command)))
        self.assertEqual((self.directory / 'verify/native-readers-stderr.log').read_text(), 'Qt diagnostic')

    def test_native_probe_timeout_keeps_partial_diagnostics(self):
        build = self.directory / 'build'
        (build / 'Release').mkdir(parents=True)
        (build / 'Release/validation_probe.exe').write_bytes(synthetic_pe())
        self.manifest()
        manifest = json.loads((self.app / 'BUILD_MANIFEST.json').read_text())
        manifest['version'] = '1.0.0'
        package.save_json(self.app / 'BUILD_MANIFEST.json', manifest)
        archive = self.directory / 'runtime.zip'
        package.write_zip(self.app, archive)
        def execute(command, log, **options):
            log.write_text('KomaMimic 1.0.0\n', encoding='utf-8')
            return dict(returncode=0)
        def timeout(command, **options):
            options['stderr'].write('BEGIN QGuiApplication\nQt diagnostic\n')
            options['stderr'].flush()
            raise subprocess.TimeoutExpired(command, options['timeout'])
        output = self.directory / 'verify'
        with (mock.patch.dict(package.os.environ, dict(SystemRoot='C:/Windows')),
              mock.patch.object(package, 'execute', side_effect=execute),
              mock.patch.object(package.subprocess, 'run', side_effect=timeout)):
            with self.assertRaisesRegex(RuntimeError, 'timed out'):
                package.native_verify(archive, build, output)
        self.assertIn('BEGIN QGuiApplication\nQt diagnostic\n',
                      (output / 'native-readers-stderr.log').read_text())
        self.assertIn('timed out after 120', (output / 'native-readers-stderr.log').read_text())
        self.assertTrue((output / 'native-readers.json').is_file())


if __name__ == '__main__':
    unittest.main(verbosity=2)
