#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Relocation, launch isolation and integrity checks for the AppImage builder."""
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_appimage as appimage
import setup_linux_poppler as poppler_build


class AppImageTests(unittest.TestCase):
    def test_pack_exports_desktop_icon_and_matching_checksums(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage = root / 'stage'
            app = stage / 'AppDir'
            app.mkdir(parents=True)
            shutil.copy2(appimage.ROOT / 'assets/komamimic.desktop', app / 'komamimic.desktop')
            shutil.copy2(appimage.ROOT / 'assets/icons/komamimic-256.png', app / 'komamimic.png')
            original = (app / 'komamimic.desktop').read_bytes()
            (stage / 'BUILD_MANIFEST.json').write_text(json.dumps(dict(
                version='1.0.0', files={p.name: appimage.digest(p) for p in app.iterdir()})))
            destination = root / 'distribution'
            def create_image(command, **kwargs):
                Path(command[-1]).write_bytes(b'AppImage packaging fixture')
            with patch.object(appimage, 'download'), patch.object(appimage, 'run', side_effect=create_image):
                image = appimage.pack(stage, destination)
            manifest = json.loads((destination / 'BUILD_MANIFEST.json').read_text())
            self.assertEqual(set(manifest['desktop_integration']), {'komamimic.desktop', 'komamimic.png'})
            self.assertEqual((app / 'komamimic.desktop').read_bytes(), original)
            self.assertEqual((destination / 'komamimic.png').read_bytes(), (app / 'komamimic.png').read_bytes())
            for name, checksum in manifest['desktop_integration'].items():
                self.assertEqual(appimage.digest(destination / name), checksum)
            checksums = dict(line.split('  ', 1)[::-1] for line in
                             (destination / 'SHA256SUMS').read_text().splitlines())
            self.assertEqual(set(checksums), {image.name, 'BUILD_MANIFEST.json', 'komamimic.desktop', 'komamimic.png'})
            for name, checksum in checksums.items():
                self.assertEqual(appimage.digest(destination / name), checksum)

    @unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('gio')
                         and shutil.which('desktop-file-validate'), 'requires Linux desktop tools')
    def test_desktop_launch_preserves_home_and_file_arguments(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / 'Home 日本語 space $ quote\"'
            executable = home / '.local/bin/KomaMimic-1.0.0-linux-x86_64.AppImage'
            executable.parent.mkdir(parents=True)
            executable.write_text('#!' + sys.executable + '\nimport json, os, sys\n'
                                  'from pathlib import Path\n'
                                  'Path(os.environ["KOMA_TEST_REPORT"]).write_text(json.dumps(sys.argv[1:]))\n')
            executable.chmod(0o755)
            output = root / 'export'
            output.mkdir()
            app = root / 'AppDir'
            app.mkdir()
            shutil.copy2(appimage.ROOT / 'assets/komamimic.desktop', app / 'komamimic.desktop')
            shutil.copy2(appimage.ROOT / 'assets/icons/komamimic-256.png', app / 'komamimic.png')
            appimage.desktop_integration(app, executable, output)
            desktop = output / 'komamimic.desktop'
            subprocess.run(['desktop-file-validate', str(desktop)], check=True)
            files = [str(root / '日本語 & space.cbz'), str(root / '$(touch INJECTED) %F "quote".zip')]
            for name in files:
                Path(name).write_bytes(b'file argument fixture')
            report = root / 'launch.json'
            env = dict(os.environ, HOME=str(home), KOMA_TEST_REPORT=str(report),
                       XDG_CACHE_HOME=str(root / 'cache'), XDG_DATA_HOME=str(home / '.local/share'),
                       XDG_CONFIG_HOME=str(root / 'config'))
            for arguments in ([], files):
                report.unlink(missing_ok=True)
                subprocess.run(['gio', 'launch', str(desktop), *arguments], env=env,
                               cwd=root, capture_output=True, text=True, check=True, timeout=10)
                deadline = time.monotonic() + 5
                while not report.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertEqual(json.loads(report.read_text()), arguments)
            self.assertFalse((root / 'INJECTED').exists())

    @unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc'), 'requires native ELF compiler')
    def test_elf_audit_rejects_displaced_initializer_and_unmapped_string_table(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'sample.c'
            library = root / 'sample.so'
            source.write_text('void custom_init(void) {}\nint sample(void) {return 7;}\n')
            subprocess.run(['cc', '-shared', '-fPIC', '-Wl,-init,custom_init',
                            str(source), '-o', str(library)], check=True)
            appimage.audit_elf_layout(library)
            original = library.read_bytes()
            header = struct.unpack_from('<16sHHIQQQIHHHHHH', original)
            changed = bytearray(original)
            for index in range(header[10]):
                segment = struct.unpack_from('<IIQQQQQQ', original, header[5] + index * header[9])
                if segment[0] == 2:
                    for offset in range(segment[2], segment[2] + segment[5], 16):
                        if struct.unpack_from('<Q', original, offset)[0] == 12:
                            struct.pack_into('<Q', changed, offset + 8, 1)
            self.assertNotEqual(changed, original)
            library.write_bytes(changed)
            with self.assertRaisesRegex(ValueError, 'Invalid ELF initializer'):
                appimage.audit_elf_layout(library)
            changed = bytearray(original)
            sections = [struct.unpack_from('<IIQQQQIIQQ', original, header[6] + i * header[11])
                        for i in range(header[12])]
            strings = sections[header[13]]
            names = original[strings[4]:strings[4] + strings[5]]
            for index, section in enumerate(sections):
                if names[section[0]:].split(b'\0', 1)[0] == b'.dynstr':
                    struct.pack_into('<Q', changed, header[6] + index * header[11] + 16, 1)
            library.write_bytes(changed)
            with self.assertRaisesRegex(ValueError, 'Invalid ELF section mapping'):
                appimage.audit_elf_layout(library)

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
        self.assertEqual(appimage.glibc_versions('GLIBC_2.28 GLIBC_ABI_DT_RELR'), {(2, 28), (2, 36)})
        self.assertEqual(appimage.glibc_versions('GLIBC_ABI_GNU2_TLS'), {(2, 43)})
        for unsupported in ('GLIBC_PRIVATE', 'GLIBC_ABI_UNKNOWN'):
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
