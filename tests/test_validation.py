#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Portable kit integrity, fixture structure and acceptance-report regressions."""
import contextlib
import csv
import hashlib
import io
import json
import plistlib
from pathlib import Path
import struct
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import xml.etree.ElementTree as ET
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests/fixtures'))
from prepare_validation import prepare, verify
from generate_validation_samples import generate
from run_validation import collect_test_evidence, create_forms, save_json, summarize
import run_validation
from stage_validation import japanese_qt_translation
from validation_cases import cases_for


class ValidationTests(unittest.TestCase):
    probe = None

    def require_macos_symlinks(self, directory):
        """Probe the host capability before a simulated macOS filesystem test."""
        target, link = directory / 'symlink-capability-target', directory / 'symlink-capability-link'
        target.write_bytes(b'link capability')
        try:
            link.symlink_to(target.name)
        except OSError as error:
            if getattr(error, 'winerror', None) == 1314:
                self.skipTest('macOS filesystem fixture requires symlinks; Windows privilege unavailable (WinError 1314)')
            raise
        except NotImplementedError:
            self.skipTest('macOS filesystem fixture requires symlinks; host does not implement them')
        finally:
            link.unlink(missing_ok=True)
            target.unlink(missing_ok=True)

    def test_macos_symlink_capability_reports_only_expected_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            denied = OSError('symbolic link privilege unavailable')
            denied.winerror = 1314
            with mock.patch.object(Path, 'symlink_to', side_effect=denied):
                for name in ('test_macos_helper_relocation_and_signing_preserve_entitlements',
                             'test_macos_sdl3_runtime_is_copied_with_dlopen_name',
                             'test_macos_stage_explicit_sdl3_and_startup_probe'):
                    with self.subTest(name=name), self.assertRaisesRegex(unittest.SkipTest, 'WinError 1314'):
                        getattr(self, name)()
            with mock.patch.object(Path, 'symlink_to', side_effect=OSError('unexpected filesystem failure')):
                with self.assertRaisesRegex(OSError, 'unexpected filesystem failure'):
                    self.require_macos_symlinks(directory)
    @classmethod
    def setUpClass(cls):
        cls.base = tempfile.TemporaryDirectory(prefix='koma-validation-')
        cls.directory = Path(cls.base.name)
        cls.kit = cls.directory / 'kit'
        with contextlib.redirect_stdout(io.StringIO()):
            prepare(ROOT, cls.kit)
        cls.fixtures = cls.kit / 'fixtures'

    @classmethod
    def tearDownClass(cls):
        cls.base.cleanup()

    def test_kit_archive_manifest_and_source_are_self_contained(self):
        with contextlib.redirect_stdout(io.StringIO()):
            manifest = verify(self.kit)
        records = {r['path']: r['sha256'] for r in manifest['files']}
        with zipfile.ZipFile(self.directory / 'kit.zip') as archive:
            for path, digest in records.items():
                self.assertEqual(hashlib.sha256(archive.read('kit/' + path)).hexdigest(), digest)
            self.assertIn('kit/VALIDATION_MANIFEST.json', archive.namelist())
            self.assertFalse(any('/tmp/' in p or '/internal/' in p or '__pycache__' in p for p in archive.namelist()))
        self.assertIn('source/tools/run_validation.py', records)
        self.assertIn('source/docs/developer/platform-validation.md', records)

    def test_generated_inputs_through_native_readers(self):
        if not self.probe:
            self.skipTest('Run through CTest with --probe to verify native reader integration')
        result = subprocess.run([str(self.probe), str(self.fixtures)], capture_output=True,
                                text=True, encoding='utf-8', timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(result.stdout)
        self.assertTrue(report['all_passed'])
        self.assertGreaterEqual(len(report['results']), 17)

    def test_generation_rejects_existing_output(self):
        with self.assertRaises(ValueError):
            generate(self.fixtures)
        with self.assertRaises(ValueError):
            prepare(ROOT, self.kit)

    def test_verify_rejects_changed_input_and_unsafe_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / 'a').write_bytes(b'altered')
            manifest = dict(schema=1, kind='synthetic-validation-kit', files=[dict(path='a', sha256='0' * 64)])
            save_json(directory / 'VALIDATION_MANIFEST.json', manifest)
            with self.assertRaisesRegex(ValueError, 'changed'):
                verify(directory)
            manifest['files'][0]['path'] = '../outside'
            save_json(directory / 'VALIDATION_MANIFEST.json', manifest)
            with self.assertRaisesRegex(ValueError, 'path'):
                verify(directory)

    def test_numbered_png_scanlines_and_animation_control(self):
        data = (self.fixtures / 'images/01.png').read_bytes()
        self.assertEqual(struct.unpack('>II', data[16:24]), (480, 720))
        offset, raw = 8, b''
        while offset < len(data):
            length = struct.unpack_from('>I', data, offset)[0]
            if data[offset + 4:offset + 8] == b'IDAT':
                raw += data[offset + 8:offset + 8 + length]
            offset += length + 12
        decoded = zlib.decompress(raw)
        self.assertEqual(len(decoded), 720 * (1 + 480 * 3))
        self.assertEqual(decoded[:4], b'\0\xff\xff\xff')
        animation = (self.fixtures / 'animation/apng/two-frames.apng').read_bytes()
        self.assertEqual(animation.count(b'fcTL'), 2)
        self.assertIn(b'acTL' + struct.pack('>II', 2, 0), animation)
        gif = (self.fixtures / 'animation/gif/two-frames.gif').read_bytes()
        self.assertTrue(gif.startswith(b'GIF89a'))
        self.assertEqual(gif.count(b'\x21\xf9\x04'), 2)

    def test_tiff_ifds_and_pdf_xref(self):
        tiff = (self.fixtures / 'animation/tiff/two-pages.tiff').read_bytes()
        current = struct.unpack_from('<I', tiff, 4)[0]
        offsets = []
        while current:
            tags = struct.unpack_from('<H', tiff, current)[0]
            entries = [struct.unpack_from('<HHII', tiff, current + 2 + i * 12) for i in range(tags)]
            offset = next(value for tag, _, _, value in entries if tag == 273)
            offsets.append(offset)
            current = struct.unpack_from('<I', tiff, current + 2 + tags * 12)[0]
        self.assertEqual(len(offsets), 2)
        self.assertEqual(tiff[offsets[0]:offsets[0] + 4096], b'\0' * 4096)
        self.assertEqual(tiff[offsets[1]:offsets[1] + 4096], b'\xff' * 4096)
        pdf = (self.fixtures / 'documents/three-pages.pdf').read_bytes()
        start = int(pdf.split(b'startxref\n')[1].splitlines()[0])
        self.assertTrue(pdf[start:].startswith(b'xref\n'))
        self.assertEqual(pdf.count(b'/Type /Page '), 3)
        self.assertEqual(pdf.count(b'/Subtype /Image'), 3)

    def test_zip_order_raw_cp932_and_mixed_documents(self):
        with zipfile.ZipFile(self.fixtures / 'books/01-first.zip') as archive:
            self.assertEqual(archive.namelist(), [f'{i:02}.png' for i in range(1, 7)])
        with zipfile.ZipFile(self.fixtures / 'books/02-next.zip') as archive:
            self.assertEqual(archive.namelist(), [f'{i:02}.png' for i in range(1, 7)])
            self.assertEqual(archive.read('01.png'), (self.fixtures / 'images/06.png').read_bytes())
        raw = (self.fixtures / 'books/03-cp932.zip').read_bytes()
        length = struct.unpack_from('<H', raw, 26)[0]
        self.assertEqual(raw[30:30 + length].decode('cp932'), '表紙ソ.png')
        self.assertIn(b'\x83\x5c', raw[30:30 + length])
        with zipfile.ZipFile(self.fixtures / 'books/04-mixed-documents.zip') as archive:
            self.assertEqual(set(archive.namelist()), {'first.txt', 'second.md', 'cover.png'})

    def test_epub_package_references_and_text_encoding(self):
        for name in ('reflow.epub', 'fixed.epub'):
            with zipfile.ZipFile(self.fixtures / 'documents' / name) as archive:
                self.assertEqual(archive.namelist()[0], 'mimetype')
                self.assertEqual(archive.getinfo('mimetype').compress_type, zipfile.ZIP_STORED)
                package = ET.fromstring(archive.read('OPS/book.opf'))
                ns = dict(opf='http://www.idpf.org/2007/opf')
                self.assertEqual(len(package.findall('opf:spine/opf:itemref', ns)), 3)
                for item in package.findall('opf:manifest/opf:item', ns):
                    self.assertIn('OPS/' + item.get('href'), archive.namelist())
                for path in archive.namelist():
                    if path.endswith('.xhtml'):
                        ET.fromstring(archive.read(path))
        utf8 = (self.fixtures / 'documents/aozora-utf8.txt').read_text(encoding='utf-8')
        self.assertEqual(utf8, (self.fixtures / 'documents/aozora-cp932.txt').read_bytes().decode('cp932'))

    def test_report_starts_unrun_and_rejects_lost_cases(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            create_forms(directory, 'Windows')
            save_json(directory / 'automatic.json', dict(status='PASS', system='Windows'))
            with contextlib.redirect_stdout(io.StringIO()):
                summarize(directory)
            summary = json.loads((directory / 'summary.json').read_text(encoding='utf-8'))
            self.assertFalse(summary['all_manual_passed'])
            self.assertEqual(summary['manual_counts']['NOT_RUN'], len(cases_for('Windows')))
            with (directory / 'manual.csv').open(encoding='utf-8-sig', newline='') as stream:
                rows = list(csv.DictReader(stream))
            with (directory / 'manual.csv').open('w', encoding='utf-8-sig', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
                writer.writeheader()
                writer.writerows(rows[1:])
            with self.assertRaisesRegex(ValueError, 'removed'):
                summarize(directory)

    def test_report_requires_observation_and_retains_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            create_forms(directory, 'Darwin')
            save_json(directory / 'automatic.json', dict(status='FAIL', system='Darwin'))
            with (directory / 'manual.csv').open(encoding='utf-8-sig', newline='') as stream:
                rows = list(csv.DictReader(stream))
            rows[0]['status'] = 'NA'
            def write():
                with (directory / 'manual.csv').open('w', encoding='utf-8-sig', newline='') as stream:
                    writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
                    writer.writeheader()
                    writer.writerows(rows)
            write()
            with self.assertRaisesRegex(ValueError, 'actual'):
                summarize(directory)
            rows[0]['actual'] = '機器なし'
            write()
            with contextlib.redirect_stdout(io.StringIO()):
                summarize(directory)
            self.assertEqual(json.loads((directory / 'summary.json').read_text(encoding='utf-8'))['automatic_status'], 'FAIL')

    def test_evidence_separates_qt_skips_and_failed_suites(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            logs = directory / 'build/Testing/Temporary'
            logs.mkdir(parents=True)
            (logs / 'LastTest.log').write_text('SKIP : document_tests::render() IPC denied\nTotals: 4 passed, 0 failed, 1 skipped, 0 blacklisted\n', encoding='utf-8')
            output = directory / 'results'
            output.mkdir()
            (output / 'ctest.xml').write_text('<testsuite><testcase name="one"/><testcase name="two"><error/></testcase></testsuite>', encoding='utf-8')
            result = collect_test_evidence(directory / 'build', output)
            self.assertEqual(result['qt_totals'], dict(passed=4, failed=0, skipped=1))
            self.assertEqual(result['skip_lines'], 1)
            self.assertEqual([s['status'] for s in result['suites']], ['PASS', 'FAIL'])

    def test_failed_source_check_stops_build_and_ignores_old_test_logs(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            build = directory / 'build'
            logs = build / 'Testing/Temporary'
            logs.mkdir(parents=True)
            (logs / 'LastTest.log').write_text('Totals: 999 passed, 0 failed, 0 skipped\n', encoding='utf-8')
            args = SimpleNamespace(build_dir=build, output=directory / 'results', fixtures=self.fixtures,
                                   cmake_arg=[], jobs=2)
            def fake_execute(command, log, *a, **kw):
                return dict(command=[str(c) for c in command], returncode=7 if Path(log).name == 'source-check.log' else 0,
                            seconds=0, log=Path(log).name)
            with mock.patch.object(run_validation, 'execute', side_effect=fake_execute), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(run_validation.run(args), 1)
            data = json.loads((args.output / 'automatic.json').read_text(encoding='utf-8'))
            self.assertEqual(data['status'], 'FAIL')
            self.assertEqual([s['name'] for s in data['steps']], ['source-check'])
            self.assertEqual(data['tests']['qt_totals'], {})
            self.assertFalse((args.output / 'LastTest.log').exists())

    def test_windows_environment_uses_actual_vcpkg_installed_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            build, installed = directory / 'build', directory / 'custom-installed'
            build.mkdir()
            (build / 'CMakeCache.txt').write_text('VCPKG_INSTALLED_DIR:PATH=' + installed.as_posix() + '\n', encoding='utf-8')
            tools = installed / 'x64-windows/tools/Qt6/bin'
            tools.mkdir(parents=True)
            (tools / 'qtpaths.exe').write_bytes(b'fake')
            with mock.patch.object(run_validation.platform, 'system', return_value='Windows'), mock.patch.object(run_validation.subprocess, 'check_output', return_value='C:/plugins\n'):
                env = run_validation.runtime_environment(build)
            self.assertEqual(env['QT_PLUGIN_PATH'], 'C:/plugins')
            self.assertTrue(env['PATH'].startswith(str(installed / 'x64-windows/bin')))

    def test_prebuilt_sdk_rejects_incomplete_wrong_arch_and_wrong_version(self):
        import setup_windows
        with tempfile.TemporaryDirectory() as tmp:
            sdk = Path(tmp)
            with self.assertRaisesRegex(ValueError, 'Incomplete'):
                setup_windows.verify_sdk(sdk)
            for name in setup_windows.CONFIG['required_files']:
                path = sdk / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'placeholder')
            core = sdk / 'bin/Qt6Core.dll'
            with self.assertRaisesRegex(ValueError, 'PE header'):
                setup_windows.verify_sdk(sdk)
            header = bytearray(64)
            header[:2] = b'MZ'
            struct.pack_into('<I', header, 60, 64)
            core.write_bytes(header + b'PE\0\0\x64\xaa')
            with self.assertRaisesRegex(ValueError, 'x64'):
                setup_windows.verify_sdk(sdk)
            core.write_bytes(header + b'PE\0\0\x64\x86')
            spec = sdk / 'mkspecs/win32-msvc/qmake.conf'
            spec.parent.mkdir(parents=True)
            spec.write_text('MSVC', encoding='utf-8')
            with mock.patch.object(setup_windows.subprocess, 'check_output', return_value='6.5.0'):
                with self.assertRaisesRegex(ValueError, 'Expected Qt'):
                    setup_windows.verify_sdk(sdk)
            with mock.patch.object(setup_windows.subprocess, 'check_output', return_value=setup_windows.CONFIG['qt_version']):
                identity = setup_windows.verify_sdk(sdk)
                self.assertEqual(identity['architecture'], 'x64-msvc2022')
                self.assertEqual(identity['files'][0]['sha256'], hashlib.sha256(core.read_bytes()).hexdigest())

    def test_sdk_installer_pins_upstream_fix_and_checks_repository_before_download(self):
        import setup_windows
        commands = setup_windows.installer_commands('python', 'SDK')
        requirement = next(x for x in commands[0] if x.startswith('aqtinstall @ '))
        self.assertTrue(requirement.endswith('@' + setup_windows.CONFIG['aqt_commit']))
        self.assertIn('--upgrade', commands[0])
        # Exercise the actual preflight in a subprocess: 3.3.0's old bool API
        # resolves this architecture to an empty extension, whereas the fix
        # resolves msvc2022_64. No SDK download is needed for either case.
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / 'aqt'
            package.mkdir()
            (package / '__init__.py').write_text('', encoding='utf-8')
            metadata = package / 'metadata.py'
            for extension, expected in (('', 1), ('msvc2022_64', 0)):
                metadata.write_text('class Version:\n def __init__(self, value): pass\n'
                    'class QtRepoProperty:\n @staticmethod\n def extension_for_arch(arch, version): return ' + repr(extension) + '\n', encoding='utf-8')
                # Disable bytecode cache so equal-size fixture rewrites are not reused.
                result = subprocess.run([sys.executable, '-B', '-c', commands[1][2]], cwd=tmp,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, expected, result.stderr)

    def test_sdk_installer_reuses_tool_environment_after_metadata_failure(self):
        import setup_windows
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            python = output / '.aqt-venv/Scripts/python.exe'
            python.parent.mkdir(parents=True)
            python.write_bytes(b'existing-environment')
            sdk = output / setup_windows.CONFIG['qt_version'] / setup_windows.CONFIG['sdk_directory']
            calls = []
            def run(command, **kwargs):
                calls.append(command)
                if 'install-qt' in command:
                    sdk.mkdir(parents=True)
                return mock.Mock(returncode=0)
            with mock.patch.object(setup_windows.platform, 'system', return_value='Windows'), mock.patch.object(setup_windows.subprocess, 'run', side_effect=run), mock.patch.object(setup_windows, 'verify_sdk', return_value={'verified': True}), mock.patch.object(setup_windows.venv.EnvBuilder, 'create') as create, contextlib.redirect_stdout(io.StringIO()):
                setup_windows.setup(output)
            create.assert_not_called()
            self.assertEqual(len(calls), 3)
            self.assertTrue((sdk / 'KOMAMIMIC_QT_SDK.json').is_file())

    def test_prebuilt_runtime_uses_sdk_before_release_only_dependencies(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            sdk = build / 'sdk'
            (sdk / 'bin').mkdir(parents=True)
            (sdk / 'bin/qtpaths.exe').write_bytes(b'fake')
            (build / 'CMakeCache.txt').write_text(
                'KOMAMIMIC_WINDOWS_FAST:BOOL=ON\nKOMAMIMIC_QT_ROOT:PATH=' + sdk.as_posix() +
                '\nVCPKG_TARGET_TRIPLET:STRING=x64-windows-release\n', encoding='utf-8')
            with mock.patch.object(run_validation.platform, 'system', return_value='Windows'), mock.patch.object(run_validation.subprocess, 'check_output', return_value='SDK/plugins'):
                env = run_validation.runtime_environment(build)
            paths = env['PATH'].split(run_validation.os.pathsep)
            self.assertEqual(paths[:2], [str(sdk / 'bin'), str(build / 'vcpkg_installed/x64-windows-release/bin')])
            self.assertEqual(env['QT_PLUGIN_PATH'], 'SDK/plugins')

    def test_platform_report_does_not_claim_full_windows_acceptance(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            create_forms(directory, 'Windows', 'platform')
            save_json(directory / 'automatic.json', dict(status='PASS', system='Windows', test_scope='platform'))
            with contextlib.redirect_stdout(io.StringIO()):
                summarize(directory)
            summary = json.loads((directory / 'summary.json').read_text(encoding='utf-8'))
            self.assertEqual(summary['test_scope'], 'platform')
            self.assertEqual(summary['manual_counts']['NOT_RUN'], 18)
            self.assertFalse(summary['all_manual_passed'])
            self.assertEqual(len(cases_for('Linux', 'full')), 36)

    def test_homebrew_framework_only_qt_prefix_is_discovered(self):
        cmake = shutil.which('cmake')
        if not cmake:
            self.skipTest('CMake is unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp).resolve()
            (directory / 'qtbase/include').mkdir(parents=True)
            (directory / 'qtwebengine/lib/cmake').mkdir(parents=True)
            # No include/ for framework-only Qt; other formula prefixes do not exist.
            script = directory / 'check.cmake'
            script.write_text('''cmake_minimum_required(VERSION 3.22)
set(APPLE TRUE)
set(CMAKE_CROSSCOMPILING FALSE)
set(KOMAMIMIC_BREW_EXECUTABLE mocked-brew)
set(KOMAMIMIC_USE_HOMEBREW ON)
set(test_prefix "@PREFIX@")
set(CMAKE_PREFIX_PATH explicit-prefix)
set(QT_ADDITIONAL_PACKAGES_PREFIX_PATH explicit-qt-prefix)
function(execute_process)
    cmake_parse_arguments(brew "" "RESULT_VARIABLE;OUTPUT_VARIABLE" "COMMAND" ${ARGN})
    list(GET brew_COMMAND 2 formula)
    set(${brew_RESULT_VARIABLE} 0 PARENT_SCOPE)
    set(${brew_OUTPUT_VARIABLE} "${test_prefix}/${formula}" PARENT_SCOPE)
endfunction()
include("@HELPER@")
if(NOT CMAKE_PREFIX_PATH STREQUAL "explicit-prefix;${test_prefix}/qtbase;${test_prefix}/qtwebengine")
    message(FATAL_ERROR "Incorrect CMake prefixes: ${CMAKE_PREFIX_PATH}")
endif()
if(NOT QT_ADDITIONAL_PACKAGES_PREFIX_PATH STREQUAL "explicit-qt-prefix;${test_prefix}/qtbase;${test_prefix}/qtwebengine")
    message(FATAL_ERROR "Incorrect Qt prefixes: ${QT_ADDITIONAL_PACKAGES_PREFIX_PATH}")
endif()
'''.replace('@PREFIX@', directory.as_posix()).replace('@HELPER@',
            (ROOT / 'cmake/Homebrew.cmake').as_posix()), encoding='utf-8')
            result = subprocess.run([cmake, '-P', str(script)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_fast_dependencies_preserve_non_qt_features_and_baseline(self):
        root = json.loads((ROOT / 'vcpkg.json').read_text(encoding='utf-8'))
        fast = json.loads((ROOT / 'cmake/windows-fast/vcpkg.json').read_text(encoding='utf-8'))
        expected = [d for d in root['dependencies'] if not (d if isinstance(d, str) else d['name']).startswith('qt')]
        self.assertEqual(fast['dependencies'], expected)
        self.assertEqual(fast['builtin-baseline'], root['builtin-baseline'])
        self.assertEqual(fast['version-string'], root['version-string'])

    def test_windows_stage_packages_own_symbols_sdk_identity_and_runtime(self):
        import stage_validation
        import setup_windows
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            build, sdk, output = directory / 'build', directory / 'sdk', directory / 'stage'
            (build / 'Release').mkdir(parents=True)
            for name in ('komamimic', 'komamimic-script-worker'):
                (build / 'Release' / (name + '.exe')).write_bytes(b'own-exe')
                (build / 'Release' / (name + '.pdb')).write_bytes(b'own-symbols')
            (build / 'CMakeCache.txt').write_text(
                'CMAKE_HOME_DIRECTORY:INTERNAL=' + ROOT.as_posix() +
                '\nKOMAMIMIC_WINDOWS_FAST:BOOL=ON\nKOMAMIMIC_QT_ROOT:PATH=' + sdk.as_posix() +
                '\nVCPKG_TARGET_TRIPLET:STRING=x64-windows-release\n', encoding='utf-8')
            dependencies = build / 'vcpkg_installed/x64-windows-release/bin'
            dependencies.mkdir(parents=True)
            (dependencies / 'archive.dll').write_bytes(b'non-qt-runtime')
            for name in setup_windows.CONFIG['required_files']:
                path = sdk / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'qt-runtime')
            (sdk / 'KOMAMIMIC_QT_SDK.json').write_text('{}', encoding='utf-8')
            def deploy(command, log, env=None):
                app = output / 'app'
                for name in setup_windows.CONFIG['required_files']:
                    source = sdk / name
                    if name.startswith('bin/') and source.suffix in ('.dll', '.exe'):
                        shutil.copy2(source, app / source.name)
                    elif name.startswith(('plugins/', 'resources/', 'translations/')):
                        target = app / (name[len('plugins/'):] if name.startswith('plugins/') else name)
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(source, target)
                return dict(returncode=0, command=list(map(str, command)), log=Path(log).name)
            with mock.patch.object(stage_validation.platform, 'system', return_value='Windows'), mock.patch.object(stage_validation.subprocess, 'check_output', return_value=str(sdk / 'translations')), mock.patch.object(stage_validation, 'execute', side_effect=deploy), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(stage_validation.stage(build, output), 0)
            with zipfile.ZipFile(output / 'KomaMimic-windows-x64-validation.zip') as package:
                self.assertEqual(package.read('app/komamimic.pdb'), b'own-symbols')
                self.assertEqual(package.read('app/archive.dll'), b'non-qt-runtime')
                self.assertEqual(package.read('QT_SDK.json'), b'{}')
                manifest = json.loads(package.read('STAGE_MANIFEST.json'))
                self.assertEqual(manifest['status'], 'STAGED_NOT_MANUALLY_VERIFIED')
                self.assertIn('app/QtWebEngineProcess.exe', {r['path'] for r in manifest['files']})

    def test_qt_japanese_catalog_is_explicitly_deployed(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            translations = directory / 'qttranslations'
            translations.mkdir()
            (translations / 'qtbase_ja.qm').write_bytes(b'catalog-bytes')
            with mock.patch('stage_validation.subprocess.check_output', return_value=str(directory / 'missing')):
                japanese_qt_translation('qtpaths', directory / 'stage', [translations])
                self.assertEqual((directory / 'stage/qtbase_ja.qm').read_bytes(), b'catalog-bytes')
                with self.assertRaisesRegex(ValueError, 'qtbase_ja'):
                    japanese_qt_translation('qtpaths', directory / 'other')

    def test_macos_install_name_is_not_a_load_dependency(self):
        import stage_validation as stage
        parsed = stage.macho_commands('''Load command 0
          cmd LC_ID_DYLIB
         name /opt/homebrew/lib/libexample.dylib (offset 24)
Load command 1
          cmd LC_LOAD_DYLIB
         name /usr/lib/libSystem.B.dylib (offset 24)
Load command 2
          cmd LC_LOAD_WEAK_DYLIB
         name @rpath/QtCore.framework/Versions/A/QtCore (offset 24)
Load command 3
          cmd LC_RPATH
         path /opt/homebrew/lib (offset 12)
''')
        self.assertEqual(parsed['identifier'], '/opt/homebrew/lib/libexample.dylib')
        self.assertEqual(parsed['dependencies'], ['/usr/lib/libSystem.B.dylib',
                                                '@rpath/QtCore.framework/Versions/A/QtCore'])
        root = Path('/test/KomaMimic.app')
        self.assertEqual(stage.external_macos_dependencies(root, {
            root / 'Contents/Frameworks/libexample.dylib': parsed}), [])

    def test_macos_helper_relocation_and_signing_preserve_entitlements(self):
        import stage_validation as stage
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp).resolve()
            self.require_macos_symlinks(output)
            app = output / 'KomaMimic.app'
            frameworks = app / 'Contents/Frameworks'
            gui = frameworks / 'QtGui.framework/Versions/A/QtGui'
            helper = frameworks / 'QtWebEngineCore.framework/Versions/A/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess'
            poppler = frameworks / 'libpoppler.165.dylib'
            for path in (gui, helper, poppler):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'\xcf\xfa\xed\xfe' + b'fixture')
            alias = frameworks / 'QtGui.framework/QtGui'
            alias.symlink_to('Versions/A/QtGui')
            old = '/opt/homebrew/opt/qtbase/lib/QtGui.framework/Versions/A/QtGui'
            records = {
                gui: dict(identifier=old, dependencies=['/usr/lib/libSystem.B.dylib']),
                helper: dict(identifier='', dependencies=[old]),
                poppler: dict(identifier='/opt/homebrew/opt/poppler/lib/libpoppler.165.dylib', dependencies=[])}
            plan = stage.macos_relocation_plan(app, records)
            helper_change = next(r for r in plan if r['binary'].endswith('/QtWebEngineProcess'))
            self.assertEqual(helper_change['arguments'][:2], ['-change', old])
            new_name = helper_change['arguments'][2]
            target = (helper.parent / new_name[len('@loader_path/'):]).resolve()
            self.assertEqual(target, gui)
            self.assertEqual(stage.external_macos_dependencies(app, {
                helper: dict(identifier='', dependencies=[new_name])}),
                [dict(binary=helper.relative_to(app).as_posix(), dependency=new_name)])
            repaired = dict(records)
            repaired[helper] = dict(identifier='', dependencies=[new_name])
            self.assertEqual(stage.external_macos_dependencies(app, repaired), [])
            # A target whose install name was already changed is still matched
            # using its versioned framework suffix, never a random basename.
            records[gui]['identifier'] = '@rpath/QtGui.framework/Versions/A/QtGui'
            self.assertEqual(stage.macos_relocation_plan(app, records)[0]['binary'], helper_change['binary'])
            commands, captured = [], []
            entitlements = {'com.apple.security.cs.allow-jit': True}
            def signature(command, **kwargs):
                path = Path(command[-1])
                captured.append((path, len(commands)))
                data = entitlements if 'QtWebEngineProcess' in str(path) else {}
                return SimpleNamespace(stdout=plistlib.dumps(data) if data else b'')
            def call(name, command):
                commands.append(list(map(str, command)))
                if '--entitlements' in command:
                    path = command[command.index('--entitlements') + 1]
                    self.assertEqual(plistlib.loads(Path(path).read_bytes()), entitlements)
            with mock.patch.object(stage.subprocess, 'run', side_effect=signature):
                stage.relocate_macos(app, output, call, records)
            changes = [c for c in commands if c[0] == 'install_name_tool']
            signs = [c for c in commands if c[0] == 'codesign']
            self.assertTrue(changes)
            self.assertTrue(all(count == 0 for _, count in captured[:len(signs)]))
            signed = [Path(c[-1]) for c in signs]
            nested_app = helper.parents[2]
            web_framework = frameworks / 'QtWebEngineCore.framework'
            self.assertLess(signed.index(helper), signed.index(nested_app))
            self.assertLess(signed.index(nested_app), signed.index(web_framework))
            self.assertNotIn(app, signed)  # Outer seal follows the qt.conf update.
            helper_sign = next(c for c in signs if c[-1] == str(nested_app))
            self.assertIn('--entitlements', helper_sign)
            # Inspection deduplicates framework aliases and rejects escaping links.
            with mock.patch.object(stage.subprocess, 'check_output', return_value='cmd LC_ID_DYLIB\nname @rpath/example (offset 24)\n') as read:
                inspected = stage.inspect_macos(app, output / 'inspection.txt')
                self.assertEqual(set(inspected), {gui, helper, poppler})
                self.assertEqual(read.call_count, 3)
                (frameworks / 'outside').symlink_to(output / 'external')
                (output / 'external').write_bytes(b'\xcf\xfa\xed\xfe')
                with self.assertRaisesRegex(ValueError, 'escapes'):
                    stage.inspect_macos(app, output / 'unsafe-inspection.txt')

    def test_macos_missing_or_ambiguous_dependency_stops_before_edits(self):
        import stage_validation as stage
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp).resolve()
            app = output / 'KomaMimic.app'
            frameworks = app / 'Contents/Frameworks'
            paths = [frameworks / 'libone.dylib', frameworks / 'libtwo.dylib']
            records = {p: dict(identifier='/opt/homebrew/lib/same.dylib', dependencies=[]) for p in paths}
            helper = app / 'Contents/MacOS/komamimic'
            records[helper] = dict(identifier='', dependencies=['/opt/homebrew/lib/same.dylib'])
            with self.assertRaisesRegex(ValueError, 'unique bundled target'), mock.patch.object(stage.subprocess, 'run') as signature:
                call = mock.Mock()
                stage.relocate_macos(app, output, call, records)
            call.assert_not_called()
            signature.assert_not_called()
            records[helper]['dependencies'] = ['/opt/homebrew/lib/missing.dylib']
            with self.assertRaisesRegex(ValueError, 'unique bundled target'):
                stage.relocate_macos(app, output, call, records)
            call.assert_not_called()

    def test_macos_sdl3_runtime_is_copied_with_dlopen_name(self):
        import stage_validation as stage
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp).resolve()
            self.require_macos_symlinks(directory)
            prefix = directory / 'sdl3'
            (prefix / 'lib').mkdir(parents=True)
            real = prefix / 'lib/libSDL3.0.4.14.dylib'
            real.write_bytes(b'\xcf\xfa\xed\xfe' + b'SDL3 fixture')
            (prefix / 'lib/libSDL3.dylib').symlink_to(real.name)
            output = directory / 'stage'
            output.mkdir()
            app = output / 'KomaMimic.app'
            with mock.patch.object(stage.subprocess, 'check_output', return_value=str(prefix)) as brew:
                target = stage.bundle_macos_sdl3(app, output)
                brew.assert_called_once_with(['brew', '--prefix', 'sdl3'], text=True)
                self.assertEqual(target.read_bytes(), real.read_bytes())
                self.assertFalse(target.is_symlink())
                alias = app / 'Contents/Frameworks/libSDL3.dylib'
                self.assertTrue(alias.is_symlink())
                self.assertEqual(alias.resolve(), target)
                self.assertTrue(alias.resolve().is_relative_to(app))
                record = json.loads((output / 'SDL3_RUNTIME.json').read_text())
                self.assertEqual(record['sha256'], hashlib.sha256(real.read_bytes()).hexdigest())
                self.assertEqual(record['source'], str(real))
                self.assertEqual(real.read_bytes(), target.read_bytes())
                with self.assertRaisesRegex(ValueError, 'already exists'):
                    stage.bundle_macos_sdl3(app, output)
                real.unlink()
                with self.assertRaisesRegex(ValueError, 'runtime missing'):
                    stage.bundle_macos_sdl3(directory / 'other.app', output)

    def test_macos_stage_explicit_sdl3_and_startup_probe(self):
        import stage_validation as stage
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp).resolve()
            self.require_macos_symlinks(directory)
            build, qt, translations, images = (directory / name for name in ('build', 'qt', 'translations', 'images'))
            build.mkdir()
            for name in ('komamimic', 'komamimic-script-worker'):
                (build / name).write_bytes(b'\xcf\xfa\xed\xfe')
            (build / 'CMakeCache.txt').write_text('CMAKE_HOME_DIRECTORY:INTERNAL=' + ROOT.as_posix() + '\n')
            (qt / 'bin').mkdir(parents=True)
            for name in ('qtpaths', 'macdeployqt'):
                (qt / 'bin' / name).write_bytes(b'tool fixture')
            translations.mkdir()
            (translations / 'qtbase_ja.qm').write_bytes(b'catalog')
            sdl = directory / 'sdl3/lib/libSDL3.dylib'
            sdl.parent.mkdir(parents=True)
            sdl.write_bytes(b'\xcf\xfa\xed\xfe' + b'SDL3')
            def query(command, **kwargs):
                if command[:2] == ['brew', '--prefix']:
                    return str(dict(qtbase=qt, qttranslations=translations,
                                    qtimageformats=images, sdl3=sdl.parent.parent)[command[2]])
                return str(translations if command[-1] == 'QT_INSTALL_TRANSLATIONS' else directory / 'plugins')
            for failed_probe in (False, True):
                output = directory / ('stage-fail' if failed_probe else 'stage-pass')
                app = output / 'KomaMimic.app'
                seen = []
                def deploy(command, log, env=None, timeout=None):
                    seen.append((list(map(str, command)), env, timeout))
                    if Path(command[0]).name == 'macdeployqt':
                        for name in ('Contents/PlugIns/platforms/libqcocoa.dylib',
                                     'Contents/PlugIns/sqldrivers/libqsqlite.dylib',
                                     'Contents/Frameworks/QtWebEngineCore.framework/Versions/A/Helpers/QtWebEngineProcess',
                                     'Contents/Resources/qtwebengine_resources.pak',
                                     'Contents/Resources/qtwebengine_locales/en-US.pak'):
                            path = app / name
                            path.parent.mkdir(parents=True, exist_ok=True)
                            path.write_bytes(b'fixture')
                    code = 1 if failed_probe and command[-1] == '--version' else 0
                    return dict(command=list(map(str, command)), returncode=code, log=Path(log).name)
                with mock.patch.object(stage.platform, 'machine', return_value='arm64'), mock.patch.object(stage.platform, 'system', return_value='Darwin'), mock.patch.object(stage.subprocess, 'check_output', side_effect=query), mock.patch.object(stage, 'inspect_macos', return_value={}), mock.patch.object(stage, 'relocate_macos'), mock.patch.object(stage, 'execute', side_effect=deploy), mock.patch.dict(stage.os.environ, {'DYLD_LIBRARY_PATH': '/developer/lib', 'SDL_DYNAMIC_API': 'override', 'QT_PLUGIN_PATH': '/developer/plugins'}), contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(stage.stage(build, output), int(failed_probe))
                manifest = json.loads((output / 'STAGE_MANIFEST.json').read_text())
                self.assertEqual(manifest['status'], 'FAIL' if failed_probe else 'STAGED_NOT_MANUALLY_VERIFIED')
                if failed_probe:
                    self.assertIn('staged-version failed', manifest['error'])
                deploy_args = seen[0][0]
                self.assertIn('-executable=' + str(app / 'Contents/Frameworks/libSDL3.0.dylib'), deploy_args)
                command, env, timeout = seen[-1]
                self.assertEqual(command, [str(app / 'Contents/MacOS/komamimic'), '--version'])
                self.assertEqual(timeout, 30)
                self.assertNotIn('DYLD_LIBRARY_PATH', env)
                self.assertNotIn('SDL_DYNAMIC_API', env)
                self.assertNotIn('QT_PLUGIN_PATH', env)
                self.assertEqual(env['PATH'], '/usr/bin:/bin:/usr/sbin:/sbin')

    def test_legacy_profile_seeds_old_setting_without_touching_main_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            build = directory / 'build'
            build.mkdir()
            executable = build / 'komamimic'
            executable.write_bytes(b'test executable identity')
            results = directory / 'results'
            results.mkdir()
            save_json(results / 'automatic.json', dict(build=str(build)))
            args = SimpleNamespace(results=results, executable=executable, profile='legacy', input=None)
            with mock.patch.object(run_validation, 'execute', return_value=dict(returncode=0)):
                self.assertEqual(run_validation.launch(args), 0)
            config = results / 'profiles/legacy/settings.ini'
            self.assertIn('[text]\nrenderer=0\n', config.read_text(encoding='utf-8'))
            self.assertFalse((results / 'profiles/main/settings.ini').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
