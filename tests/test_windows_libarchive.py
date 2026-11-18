# SPDX-License-Identifier: GPL-3.0-only
"""Preserve the pinned port and its dependency graph while updating libarchive alone."""
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import setup_windows_libarchive as overlay


class WindowsLibarchiveTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='libarchive-overlay-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.vcpkg = self.root / 'vcpkg'
        self.original = self.vcpkg / 'ports/libarchive'
        self.original.mkdir(parents=True)
        self.recipe = ('vcpkg_from_github(\n    OUT_SOURCE_PATH SOURCE_PATH\n'
                       '    REPO libarchive/libarchive\n    REF "v${VERSION}"\n'
                       '    SHA512 old_source_hash\n    HEAD_REF master\n    PATCHES\n'
                       '        fix-buildsystem.patch\n        fix-deps.patch\n)\n'
                       'set(KEEP_ORIGINAL_RECIPE_AND_DEPENDENCIES ON)\n')
        (self.original / 'portfile.cmake').write_text(self.recipe)
        self.manifest = dict(name='libarchive', version='3.8.9', dependencies=['zlib', {'name':'vcpkg-cmake','host':True}],
                             features={'crypto':dict(dependencies=['openssl'])})
        (self.original / 'vcpkg.json').write_text(json.dumps(self.manifest))
        for name in ('fix-buildsystem.patch', 'fix-deps.patch', 'usage', 'copyright'):
            (self.original / name).write_bytes(('Original upstream file ' + name).encode())
        self.name = 'libarchive-3.8.11.tar.gz'
        self.data = b'original pinned release archive fixture'
        self.entry = dict(url='https://primary.invalid/' + self.name,
                          sha256=hashlib.sha256(self.data).hexdigest())
        self.inputs = self.root / 'prepared'
        self.inputs.mkdir()
        (self.inputs / self.name).write_bytes(self.data)
        self.output = self.root / 'overlay'
        self.config = mock.patch.object(overlay, 'configuration', return_value={self.name:self.entry})
        self.config.start()
        self.addCleanup(self.config.stop)

    def test_preserves_original_port_patches_dependencies_and_cached_source(self):
        with mock.patch.object(overlay, 'download') as download:
            port = overlay.setup(self.vcpkg, self.output, self.inputs)
        download.assert_not_called()
        self.assertEqual((self.original / 'portfile.cmake').read_text(), self.recipe)
        manifest = json.loads((port / 'vcpkg.json').read_text())
        self.assertEqual(manifest, {**self.manifest, 'version':'3.8.11'})
        for name in ('fix-buildsystem.patch', 'usage', 'copyright'):
            self.assertEqual((port / name).read_bytes(), (self.original / name).read_bytes())
        for name in ('fix-deps.patch', 'LICENSE-vcpkg.txt'):
            self.assertEqual((port / name).read_bytes(), (overlay.ROOT / 'cmake/ports/libarchive' / name).read_bytes())
        self.assertEqual((self.vcpkg / 'downloads' / self.name).read_bytes(), self.data)
        recipe = (port / 'portfile.cmake').read_text()
        self.assertIn(hashlib.sha512(self.data).hexdigest(), recipe)
        self.assertTrue(recipe.endswith('set(KEEP_ORIGINAL_RECIPE_AND_DEPENDENCIES ON)\n'))
        self.assertNotIn('SKIP_', recipe)

    @unittest.skipUnless(shutil.which('cmake'), 'requires CMake')
    def test_generated_cmake_uses_the_verified_cache_and_preserves_patch_order(self):
        port = overlay.setup(self.vcpkg, self.output, self.inputs)
        script = self.root / 'check.cmake'
        script.write_text('''function(vcpkg_download_distfile output)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "FILENAME;SHA512" "URLS")
  set(archive "${DOWNLOADS}/${arg_FILENAME}")
  file(SHA512 "${archive}" actual)
  if(NOT actual STREQUAL arg_SHA512)
    message(FATAL_ERROR "Cached source hash differs")
  endif()
  set(${output} "${archive}" PARENT_SCOPE)
endfunction()
function(vcpkg_extract_source_archive output)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "ARCHIVE" "PATCHES")
  if(NOT arg_PATCHES STREQUAL "fix-buildsystem.patch;fix-deps.patch")
    message(FATAL_ERROR "Original patches were not retained in order")
  endif()
  file(WRITE "${REPORT}" "cached source and original patches verified")
endfunction()
include("${PORTFILE}")
''')
        report = self.root / 'cmake-result.txt'
        subprocess.run(['cmake', '-DDOWNLOADS=' + str(self.vcpkg / 'downloads'),
                        '-DPORTFILE=' + str(port / 'portfile.cmake'), '-DREPORT=' + str(report),
                        '-P', str(script)], check=True, capture_output=True)
        self.assertEqual(report.read_text(), 'cached source and original patches verified')

    def test_changed_or_missing_prepared_source_is_rejected_before_overlay_creation(self):
        archive = self.inputs / self.name
        archive.write_bytes(b'changed source')
        with mock.patch.object(overlay, 'download') as download:
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                overlay.setup(self.vcpkg, self.output, self.inputs)
            archive.unlink()
            with self.assertRaises(FileNotFoundError):
                overlay.setup(self.vcpkg, self.output, self.inputs)
        download.assert_not_called()
        self.assertFalse(self.output.exists())

    def test_standalone_build_downloads_the_same_pinned_source_once(self):
        with mock.patch('urllib.request.urlopen', return_value=io.BytesIO(self.data)) as fetch:
            overlay.setup(self.vcpkg, self.output)
        self.assertEqual(fetch.call_count, 1)
        self.assertEqual((self.vcpkg / 'downloads' / self.name).read_bytes(), self.data)

    def test_unrecognized_baseline_port_fails_before_download(self):
        (self.original / 'portfile.cmake').write_text('different upstream port layout')
        with mock.patch.object(overlay, 'download') as download:
            with self.assertRaisesRegex(ValueError, 'Unrecognized pinned'):
                overlay.setup(self.vcpkg, self.output)
        download.assert_not_called()
