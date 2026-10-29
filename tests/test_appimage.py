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
import setup_linux_poppler as poppler_build


class AppImageTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('pkg-config'), 'requires pkg-config')
    def test_private_pc_dependency_closure_is_installed_and_requeried(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            def pc(name, requires=''):
                (root / (name + '.pc')).write_text(
                    f'Name: {name}\nDescription: fixture\nVersion: 1\n'
                    f'Requires.private: {requires}\nCflags: -I/fixture/{name}\nLibs: -l{name}\n')
            pc('fontconfig', 'uuid')
            pc('gio-2.0', 'mount libselinux')
            real_run = subprocess.run
            installed = set()
            def run(command, **kwargs):
                if command[0] != 'dnf':
                    return real_run(command, **kwargs)
                for capability in command[4:]:
                    name = capability.removeprefix('pkgconfig(').removesuffix(')')
                    installed.add(name)
                    pc(name, 'blkid' if name == 'mount' else '')
                return subprocess.CompletedProcess(command, 0)
            env = dict(os.environ, PKG_CONFIG_PATH=str(root), PKG_CONFIG_LIBDIR=str(root))
            with patch.object(poppler_build.subprocess, 'run', side_effect=run):
                flags = poppler_build.pkg_config_flags(['fontconfig', 'gio-2.0'], env, True)
            self.assertEqual(installed, {'uuid', 'mount', 'libselinux', 'blkid'})
            self.assertIn('-I/fixture/gio-2.0', flags)
            self.assertIn('-I/fixture/blkid', flags)
            # A failed install must stop rather than looping or dropping flags.
            (root / 'uuid.pc').unlink()
            with patch.object(poppler_build.subprocess, 'run', side_effect=lambda command, **kw:
                              subprocess.CompletedProcess(command, 0) if command[0] == 'dnf'
                              else real_run(command, **kw)):
                with self.assertRaisesRegex(ValueError, 'Cannot resolve'):
                    poppler_build.pkg_config_flags(['fontconfig'], env, True)
            # Missing dependencies must not mutate the host without the CI flag.
            with self.assertRaisesRegex(ValueError, 'Cannot resolve'):
                poppler_build.pkg_config_flags(['fontconfig'], env)

    def test_rx_enum_generator_runs_with_selected_python_and_missing_tool_fails_early(self):
        with tempfile.TemporaryDirectory() as temporary:
            tool = Path(temporary) / 'opt/rx/glib2/bin/glib-mkenums'
            tool.parent.mkdir(parents=True)
            tool.write_text('#!/missing/interpreter\nimport sys\nassert sys.argv[1:] == ["--help"]\n')
            with patch.object(poppler_build, 'package_files', return_value=[str(tool)]):
                self.assertEqual(poppler_build.glib_mkenums(), tool.resolve())
                tool.unlink()
                with self.assertRaisesRegex(ValueError, 'Expected one installed RX glib-mkenums'):
                    poppler_build.glib_mkenums()

    def test_freetype_uses_selected_prefix_and_versioned_runtime_despite_broken_devel_link(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            libdir = root / 'lib64'
            runtime = libdir / 'freetype/libfreetype.so.26.2.14'
            runtime.parent.mkdir(parents=True)
            runtime.write_bytes(b'runtime')
            (runtime.parent / 'libfreetype.so.26').symlink_to(runtime.name)
            (libdir / 'libfreetype.so').symlink_to('missing-library')
            include = root / 'include/freetype2'
            (include / 'freetype').mkdir(parents=True)
            (include / 'ft2build.h').write_text('')
            (include / 'freetype/freetype.h').write_text('')
            with patch.object(poppler_build.subprocess, 'check_output',
                              side_effect=[str(libdir), str(include)]):
                self.assertEqual(poppler_build.freetype_paths(), (include, runtime.resolve()))
            runtime.unlink()
            with patch.object(poppler_build.subprocess, 'check_output',
                              side_effect=[str(libdir), str(include)]):
                with self.assertRaisesRegex(ValueError, 'Expected one libfreetype'):
                    poppler_build.freetype_paths()

    def test_debian_source_manifest_rejects_escaping_filenames(self):
        checksum = 'a' * 64
        text = f'Checksums-Sha256:\n {checksum} 123 source.tar.xz\nFiles:\n'
        self.assertEqual(poppler_build.checksums(text), {'source.tar.xz': (checksum, 123)})
        with self.assertRaises(ValueError):
            poppler_build.checksums(text.replace('source.tar.xz', '../source.tar.xz'))

    @unittest.skipUnless(shutil.which('patch'), 'requires patch')
    def test_debian_patches_are_applied_in_series_order(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            patches = root / 'debian/patches'
            patches.mkdir(parents=True)
            (root / 'value').write_text('old\n')
            (patches / 'series').write_text('# Security fixes\nfirst.patch\nsecond.patch\n')
            for name, before, after in [('first', 'old', 'intermediate'), ('second', 'intermediate', 'fixed')]:
                (patches / (name + '.patch')).write_text(
                    f'--- a/value\n+++ b/value\n@@ -1 +1 @@\n-{before}\n+{after}\n')
            poppler_build.apply_patches(root)
            self.assertEqual((root / 'value').read_text(), 'fixed\n')

    def test_poppler_data_override_patch_keeps_notices_and_targets_constructor_body(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / 'poppler/GlobalParams.cc'
            path.parent.mkdir()
            path.write_text('// Upstream copyright and license\n'
                            'GlobalParams::GlobalParams(const char *customPopplerDataDir) : cache{0}\n{\n'
                            '    originalSetup(customPopplerDataDir);\n}\n')
            checksum = poppler_build.relocatable_data_patch(root)
            text = path.read_text()
            self.assertIn('// Upstream copyright and license', text)
            self.assertIn('cache{0}\n{\n    if (!customPopplerDataDir)', text)
            self.assertIn('std::getenv("POPPLER_DATADIR")', text)
            self.assertEqual(checksum, appimage.digest(root / 'komamimic-relocatable-data.patch'))

    def test_japanese_catalog_uses_rpm_location_and_rejects_filtered_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            alternate = root / 'rpm-translations/qtbase_ja.qm'
            alternate.parent.mkdir()
            alternate.write_bytes(b'Japanese catalog fixture')
            with patch.object(appimage, 'output', return_value=str(alternate)):
                self.assertEqual(appimage.japanese_catalog(root / 'qt-query'), alternate.resolve())
                alternate.unlink()
                with self.assertRaisesRegex(ValueError, '_install_langs'):
                    appimage.japanese_catalog(root / 'qt-query')

    def test_japanese_catalog_uses_existing_qt_query_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            catalog = root / 'qtbase_ja.qm'
            catalog.write_bytes(b'Japanese catalog fixture')
            with patch.object(appimage, 'output') as rpm:
                self.assertEqual(appimage.japanese_catalog(root), catalog)
                rpm.assert_not_called()

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
                               'poppler=os.environ["POPPLER_DATADIR"], '
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
            self.assertEqual(report['poppler'], str(root / 'usr/share/poppler'))
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
            report = json.loads((root / 'TIFF_DEPENDENCIES.json').read_text())
            self.assertEqual({entry['tiff'] for entry in report}, {'libtiff.so.5', 'libtiff.so.6'})
            self.assertTrue(all(entry['root'] == str(root / 'program') for entry in report))

    @unittest.skipUnless(shutil.which('cc') and shutil.which('patchelf'), 'requires native ELF tools')
    def test_dependency_report_traces_real_elf_transitive_edges(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'leaf.c').write_text('int koma_leaf(void) { return 0; }\n')
            (root / 'parent.c').write_text('int koma_leaf(void); int koma_parent(void) { return koma_leaf(); }\n')
            leaf, parent = root / 'libkoma_leaf.so.1', root / 'libkoma_parent.so.1'
            subprocess.run(['cc', '-shared', '-fPIC', str(root / 'leaf.c'),
                            '-Wl,-soname,' + leaf.name, '-o', str(leaf)], check=True)
            subprocess.run(['cc', '-shared', '-fPIC', str(root / 'parent.c'), str(leaf),
                            '-Wl,-rpath,' + str(root), '-o', str(parent)], check=True)
            chain = appimage.dependency_chain(parent, leaf.name, dict(appimage.dependencies(parent)))
            self.assertEqual(chain, [str(parent), leaf.name + ' => ' + str(leaf)])

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
