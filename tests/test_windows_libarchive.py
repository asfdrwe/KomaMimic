# SPDX-License-Identifier: GPL-3.0-only
"""Preserve the pinned port and its dependency graph while updating libarchive alone."""
import hashlib
import difflib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import setup_windows_libarchive as overlay
verify_source_patches = overlay.verify_source_patches

# Preserve the blank lines present in the release input and in the CI failure.
CRYPTO_SOURCE = '''# release fixture
#
# Find mbed TLS
IF(ENABLE_MBEDTLS)
  FIND_PACKAGE(MbedTLS)
  IF(MBEDTLS_FOUND)
    SET(HAVE_LIBMBEDCRYPTO 1)
    LIST(APPEND ADDITIONAL_LIBS ${MBEDCRYPTO_LIBRARY})
    INCLUDE_DIRECTORIES(${MBEDTLS_INCLUDE_DIRS})

    CMAKE_PUSH_CHECK_STATE()
    LIST(APPEND CMAKE_REQUIRED_INCLUDES ${MBEDTLS_INCLUDE_DIRS})
  ENDIF(MBEDTLS_FOUND)
ENDIF(ENABLE_MBEDTLS)

IF(ENABLE_NETTLE)
  FIND_PACKAGE(Nettle)
  IF(NETTLE_FOUND)
    SET(HAVE_LIBNETTLE 1)
    LIST(APPEND ADDITIONAL_LIBS ${NETTLE_LIBRARIES})
    INCLUDE_DIRECTORIES(${NETTLE_INCLUDE_DIR})

    CMAKE_PUSH_CHECK_STATE()
    LIST(APPEND CMAKE_REQUIRED_INCLUDES ${NETTLE_INCLUDE_DIR})
  ENDIF(NETTLE_FOUND)
ENDIF(ENABLE_NETTLE)

IF(ENABLE_OPENSSL AND NOT CMAKE_SYSTEM_NAME MATCHES "Darwin")
  FIND_PACKAGE(OpenSSL)
  IF(OPENSSL_FOUND)
    SET(HAVE_LIBCRYPTO 1)
    INCLUDE_DIRECTORIES(${OPENSSL_INCLUDE_DIR})
    LIST(APPEND ADDITIONAL_LIBS ${OPENSSL_CRYPTO_LIBRARY})

    CMAKE_PUSH_CHECK_STATE()
    SET(CMAKE_REQUIRED_LIBRARIES ${OPENSSL_CRYPTO_LIBRARY})
    SET(CMAKE_REQUIRED_INCLUDES ${OPENSSL_INCLUDE_DIR})
  ENDIF(OPENSSL_FOUND)
ENDIF()
'''


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
        self.preflight = mock.patch.object(overlay, 'verify_source_patches')
        self.preflight_mock = self.preflight.start()
        self.addCleanup(self.preflight.stop)

    def test_preserves_original_port_patches_dependencies_and_cached_source(self):
        with mock.patch.object(overlay, 'download') as download:
            port = overlay.setup(self.vcpkg, self.output, self.inputs)
        download.assert_not_called()
        self.preflight_mock.assert_called_once_with(self.inputs / self.name, '3.8.11', port)
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

    def patch_fixture(self):
        archive = self.root / 'release-fixture.tar.gz'
        with tarfile.open(archive, 'w:gz') as release:
            for name, data in (('CMakeLists.txt', CRYPTO_SOURCE.encode()),
                               ('libarchive/CMakeLists.txt', b'# subdirectory fixture\n'),
                               ('build/cmake/CreatePkgConfigFile.cmake', b'# pkgconfig fixture\n')):
                info = tarfile.TarInfo('libarchive-3.8.11/' + name)
                info.size = len(data)
                release.addfile(info, io.BytesIO(data))
        port = self.root / 'patches'
        port.mkdir()
        baseline = ''.join(difflib.unified_diff(CRYPTO_SOURCE.splitlines(True),
            CRYPTO_SOURCE.replace('# release fixture', '# buildsystem fixture', 1).splitlines(True),
            fromfile='a/CMakeLists.txt', tofile='b/CMakeLists.txt'))
        (port / 'fix-buildsystem.patch').write_text(baseline)
        return archive, port

    @unittest.skipUnless(shutil.which('git'), 'requires Git patch application')
    def test_release_crypto_patch_preserves_blank_context_inside_a_parent_repository(self):
        archive, port = self.patch_fixture()
        # The generated overlay is inside the checkout in CI. --no-index must
        # apply to the extracted files without modifying that checkout's index.
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True, capture_output=True)
        source = (overlay.ROOT / 'cmake/ports/libarchive/fix-deps.patch').read_text()
        blocks = source.split('@@ ')
        selected = []
        needles = ('+    LIST(APPEND LIBARCHIVE_LIBS_PRIVATE ${MBEDCRYPTO_LIBRARY})',
                   '+    STRING(APPEND LIBSREQUIRED " nettle")',
                   '+    LIST(APPEND ADDITIONAL_LIBS OpenSSL::Crypto)')
        for block in blocks[1:]:
            if any(needle in block for needle in needles):
                selected.append('@@ ' + block)
        self.assertEqual(len(selected), 3)
        (port / 'fix-deps.patch').write_text('--- a/CMakeLists.txt\n+++ b/CMakeLists.txt\n' + ''.join(selected))
        before = hashlib.sha256(archive.read_bytes()).digest()
        verify_source_patches(archive, '3.8.11', port)
        self.assertEqual(hashlib.sha256(archive.read_bytes()).digest(), before)
        self.assertFalse((self.root / '.git/index').exists())

    @unittest.skipUnless(shutil.which('git'), 'requires Git patch application')
    def test_source_preflight_rejects_the_reported_blank_stripped_patch(self):
        archive, port = self.patch_fixture()
        # Exact failed hunk shape from the Windows diagnostic, which omitted
        # the release file's blank line before CMAKE_PUSH_CHECK_STATE().
        (port / 'fix-deps.patch').write_text('''--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -825,6 +845,7 @@
   IF(MBEDTLS_FOUND)
     SET(HAVE_LIBMBEDCRYPTO 1)
     LIST(APPEND ADDITIONAL_LIBS ${MBEDCRYPTO_LIBRARY})
+    LIST(APPEND LIBARCHIVE_LIBS_PRIVATE ${MBEDCRYPTO_LIBRARY})
     INCLUDE_DIRECTORIES(${MBEDTLS_INCLUDE_DIRS})
     CMAKE_PUSH_CHECK_STATE()
     LIST(APPEND CMAKE_REQUIRED_INCLUDES ${MBEDTLS_INCLUDE_DIRS})
''')
        with self.assertRaisesRegex(RuntimeError, 'release source cannot apply fix-deps.patch'):
            verify_source_patches(archive, '3.8.11', port)
