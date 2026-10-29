#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Relocation, launch isolation and integrity checks for the AppImage builder."""
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_appimage as appimage


class AppImageTests(unittest.TestCase):
    def test_host_boundary_keeps_qt_codecs_and_cpp_but_excludes_glibc_and_drivers(self):
        for name in ('libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2',
                     'ld-linux-x86-64.so.2', 'libGLX.so.0', 'libEGL.so.1', 'libnss_files.so.2'):
            self.assertIsNotNone(appimage.HOST_LIBRARIES.fullmatch(name), name)
        for name in ('libQt6Core.so.6', 'libstdc++.so.6', 'libnss3.so', 'libtiff.so.5', 'libjpeg.so.62'):
            self.assertIsNone(appimage.HOST_LIBRARIES.fullmatch(name), name)

    def test_glibc_audit_compares_versions_numerically_and_rejects_private_abi(self):
        versions = appimage.glibc_versions('GLIBC_2.9 GLIBC_2.28 GLIBC_2.35 GLIBCXX_3.4.30')
        self.assertEqual(max(versions), (2, 35))
        for unsupported in ('GLIBC_PRIVATE', 'GLIBC_ABI_DT_RELR'):
            with self.assertRaises(ValueError):
                appimage.glibc_versions(unsupported)

    def test_launcher_preserves_arguments_cwd_and_does_not_disable_sandbox(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'App Dir 日本語'
            (root / 'usr/bin').mkdir(parents=True)
            program = root / 'usr/bin/komamimic'
            program.write_text('#!' + sys.executable + '\nimport json, os, sys\n'
                               'print(json.dumps(dict(args=sys.argv[1:], cwd=os.getcwd(), '
                               'plugins=os.environ["QT_PLUGIN_PATH"], '
                               'sandbox=os.environ.get("QTWEBENGINE_DISABLE_SANDBOX"))))\n')
            program.chmod(0o755)
            (root / 'AppRun').write_text(appimage.APP_RUN)
            (root / 'AppRun').chmod(0o755)
            env = dict(os.environ)
            env.pop('QTWEBENGINE_DISABLE_SANDBOX', None)
            result = subprocess.check_output([str(root / 'AppRun'), 'book & space.cbz', '--version'],
                                             cwd=temporary, env=env, text=True)
            report = json.loads(result)
            self.assertEqual(report['args'], ['book & space.cbz', '--version'])
            self.assertEqual(Path(report['cwd']).resolve(), Path(temporary).resolve())
            self.assertEqual(report['plugins'], str(root / 'usr/plugins'))
            self.assertIsNone(report['sandbox'])

    def test_download_rejects_modified_packaging_tool(self):
        with tempfile.TemporaryDirectory() as temporary:
            with patch('urllib.request.urlopen', return_value=io.BytesIO(b'wrong bytes')):
                with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    appimage.download(dict(url='https://example.invalid/tool', sha256='0' * 64),
                                      Path(temporary) / 'tool')

    def test_tampered_or_escaping_payload_stops_before_tool_download(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'stage/AppDir').mkdir(parents=True)
            (root / 'stage/AppDir/AppRun').write_bytes(b'altered')
            for index, relative in enumerate(('AppRun', '../../outside')):
                (root / 'stage/BUILD_MANIFEST.json').write_text(json.dumps(dict(files={relative: '0' * 64})))
                with patch.object(appimage, 'download') as download:
                    with self.assertRaises(ValueError):
                        appimage.pack(root / 'stage', root / f'output-{index}')
                    download.assert_not_called()

    def test_tiff_five_and_six_cannot_be_staged_together(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            paths = []
            for name in ('libtiff.so.5', 'libtiff.so.6'):
                source = root / name
                source.write_bytes(b'fixture')
                paths.append((name, source))
            with patch.object(appimage, 'dependencies', return_value=paths):
                with self.assertRaisesRegex(ValueError, 'Mixed TIFF'):
                    appimage.collect_libraries(root / 'AppDir', [(root / 'program', root / 'unused')])

    @unittest.skipUnless(shutil.which('cc') and shutil.which('patchelf'), 'requires native ELF tools')
    def test_real_elf_dependency_is_relocated_and_cannot_leak_build_prefix(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'build'
            app = root / 'AppDir'
            source.mkdir()
            (app / 'usr/bin').mkdir(parents=True)
            (source / 'library.c').write_text('int koma_appimage_check(void) { return 7; }\n')
            (source / 'main.c').write_text('int koma_appimage_check(void); int main(void) { return koma_appimage_check(); }\n')
            library = source / 'libkoma_appimage_check.so.1'
            subprocess.run(['cc', '-shared', '-fPIC', str(source / 'library.c'),
                            '-Wl,-soname,libkoma_appimage_check.so.1', '-o', str(library)], check=True)
            preferred = source / 'preferred/libkoma_appimage_check.so.1'
            preferred.parent.mkdir()
            (source / 'preferred.c').write_text('int koma_appimage_check(void) { return 0; }\n')
            subprocess.run(['cc', '-shared', '-fPIC', str(source / 'preferred.c'),
                            '-Wl,-soname,libkoma_appimage_check.so.1', '-o', str(preferred)], check=True)
            program = source / 'program'
            subprocess.run(['cc', str(source / 'main.c'), str(library),
                            '-Wl,-rpath,' + str(source), '-o', str(program)], check=True)
            target = app / 'usr/bin/program'
            appimage.copy(program, target)
            appimage.collect_libraries(app, [(program, target)], preferred=[preferred])
            appimage.relocate_and_audit(app, (2, 35))
            shutil.rmtree(source)
            subprocess.run([str(target)], env=dict(os.environ, LD_LIBRARY_PATH=''), check=True)


if __name__ == '__main__':
    unittest.main()
