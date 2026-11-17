# SPDX-License-Identifier: GPL-3.0-only
"""The macOS candidate must build and deploy Qt's FFmpeg media backend."""
import hashlib
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import setup_macos_multimedia as setup


class MacMultimediaTests(unittest.TestCase):
    def test_source_must_match_the_installed_qt_and_verified_upstream_url(self):
        version = '6.12.0'
        url = 'https://download.qt.io/official_releases/qt/6.12/6.12.0/submodules/qtmultimedia-everywhere-src-6.12.0.tar.xz'
        formula = dict(versions=dict(stable=version), urls=dict(stable=dict(url=url, checksum='a' * 64)))
        self.assertEqual(setup.source_identity(formula, version)['sha256'], 'a' * 64)
        with self.assertRaisesRegex(ValueError, 'must match'):
            setup.source_identity(formula, '6.11.2')
        formula['urls']['stable']['url'] = 'https://example.invalid/source.tar.xz'
        with self.assertRaisesRegex(ValueError, 'source URL'):
            setup.source_identity(formula, version)

    def test_configure_enables_ffmpeg_and_keeps_installation_separate(self):
        cmd = list(map(str, setup.configure_command(Path('src'), Path('build'), Path('private/install'),
                                                    [Path('brew/qtbase'), Path('brew/qtdeclarative')],
                                                    Path('brew/ffmpeg'), Path('brew/vulkan-headers/include'))))
        self.assertIn('-DQT_FEATURE_ffmpeg=ON', cmd)
        self.assertIn('-DQT_DEFAULT_MEDIA_BACKEND=ffmpeg', cmd)
        self.assertIn('-DFFMPEG_DIR=' + str(Path('brew/ffmpeg')), cmd)
        self.assertIn('-DCMAKE_INSTALL_PREFIX=' + str(Path('private/install')), cmd)
        self.assertIn('-DQT_ADDITIONAL_PACKAGES_PREFIX_PATH=' + ';'.join(map(str, [Path('brew/qtbase'), Path('brew/qtdeclarative')])), cmd)
        self.assertIn('-DCMAKE_OSX_ARCHITECTURES=arm64', cmd)
        self.assertIn('-DVulkan_INCLUDE_DIR=' + str(Path('brew/vulkan-headers/include')), cmd)

    def test_vulkan_headers_are_required_before_downloading_and_building_qt(self):
        with tempfile.TemporaryDirectory() as temporary:
            prefix = Path(temporary)
            with self.assertRaisesRegex(ValueError, 'install vulkan-headers'):
                setup.vulkan_headers(prefix)
            header = prefix / 'include/vulkan/vulkan.h'
            header.parent.mkdir(parents=True)
            header.write_text('/* Vulkan header fixture */')
            self.assertEqual(setup.vulkan_headers(prefix), prefix / 'include')

    def test_source_checksum_and_archive_paths_are_checked_before_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / 'source.tar.xz'
            with tarfile.open(archive, 'w:xz') as stream:
                info = tarfile.TarInfo('qtmultimedia-everywhere-src-6.12.0/CMakeLists.txt')
                info.size = 5
                stream.addfile(info, io.BytesIO(b'cmake'))
            identity = dict(version='6.12.0', sha256='0' * 64)
            with self.assertRaisesRegex(ValueError, 'checksum'):
                setup.extract_source(archive, root / 'not-extracted', identity)
            self.assertFalse((root / 'not-extracted').exists())
            identity['sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
            self.assertTrue((setup.extract_source(archive, root / 'source', identity) / 'CMakeLists.txt').is_file())
            with tarfile.open(archive, 'w:xz') as stream:
                info = tarfile.TarInfo('../escape')
                stream.addfile(info)
            identity['sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
            with self.assertRaises(tarfile.TarError):
                setup.extract_source(archive, root / 'unsafe', identity)
            self.assertFalse((root / 'escape').exists())

    def test_native_only_plugin_directory_cannot_satisfy_backend_requirement(self):
        with tempfile.TemporaryDirectory() as temporary:
            prefix = Path(temporary)
            plugins = prefix / 'share/qt/plugins'
            (plugins / 'multimedia').mkdir(parents=True)
            (plugins / 'multimedia/libdarwinmediaplugin.dylib').write_bytes(b'native')
            with self.assertRaisesRegex(ValueError, 'FFmpeg plugin is missing'):
                setup.plugin_directory(prefix)
            (plugins / 'multimedia/libffmpegmediaplugin.dylib').write_bytes(b'ffmpeg')
            self.assertEqual(setup.plugin_directory(prefix), plugins)
