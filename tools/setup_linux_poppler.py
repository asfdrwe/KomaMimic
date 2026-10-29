#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build Debian's maintained Poppler source on Rocky against WebEngine's TIFF ABI."""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import urllib.request

VERSION = '22.12.0'
REVISION = VERSION + '-2+deb12u2'
BASE = 'https://deb.debian.org/debian/pool/main/p/poppler/'


def checksums(text):
    section = text.split('Checksums-Sha256:\n', 1)[1]
    result = {}
    for line in section.splitlines():
        if not line.startswith(' '):
            break
        match = re.fullmatch(r'\s+([a-f0-9]{64})\s+(\d+)\s+([^/\s]+)', line)
        if not match or match[3] in ('.', '..'):
            raise ValueError('Unsafe or malformed Debian source checksum: ' + line)
        result[match[3]] = (match[1], int(match[2]))
    return result


def fetch(name, destination, expected=None):
    request = urllib.request.Request(BASE + name, headers={'User-Agent': 'KomaMimic-build'})
    with urllib.request.urlopen(request, timeout=120) as source, destination.open('xb') as target:
        shutil.copyfileobj(source, target)
    data = destination.read_bytes()
    checksum = hashlib.sha256(data).hexdigest()
    if expected and (checksum, len(data)) != expected:
        raise ValueError('Debian source checksum/size mismatch: ' + name)
    return checksum


def package_files(*names):
    return subprocess.check_output(['rpm', '-ql', *names], text=True).splitlines()


def unique_library(files, prefix):
    found = sorted({Path(p).resolve(strict=True) for p in files
                    if (Path(p).name == prefix or Path(p).name.startswith(prefix + '.')) and Path(p).is_file()})
    if len(found) != 1:
        raise ValueError('Expected one ' + prefix + ': ' + repr(found))
    return found[0]


def header_directory(files, filename):
    found = sorted({str(Path(p).parent) for p in files if Path(p).name == filename and Path(p).is_file()})
    if len(found) != 1:
        raise ValueError('Expected one header directory for ' + filename + ': ' + repr(found))
    return found[0]


def freetype_paths():
    # RX FreeType can live below lib64/freetype, outside FindFreetype's
    # default search paths. Follow the selected .pc file for both halves.
    def variable(name):
        return Path(subprocess.check_output(
            ['pkg-config', '--variable=' + name, 'freetype2'], text=True).strip())
    libdir = variable('libdir')
    include = variable('includedir')
    if not (include / 'ft2build.h').is_file() and (include / 'freetype2/ft2build.h').is_file():
        include /= 'freetype2'
    candidates = list(libdir.glob('libfreetype.so*'))
    if not any(p.is_file() for p in candidates):
        candidates = list((libdir / 'freetype').glob('libfreetype.so*'))
    library = unique_library([str(p) for p in candidates], 'libfreetype.so')
    for header in ('ft2build.h', 'freetype/freetype.h'):
        if not (include / header).is_file():
            raise ValueError('Missing selected FreeType header: ' + str(include / header))
    print('Selected FreeType headers/library:', include, library, flush=True)
    return include, library


def glib_mkenums():
    files = package_files('rx-glib2-devel')
    found = sorted({Path(p).resolve() for p in files
                    if Path(p).name == 'glib-mkenums' and Path(p).is_file()})
    if len(found) != 1:
        raise ValueError('Expected one installed RX glib-mkenums: ' + repr(found))
    # Poppler invokes this Python script via its selected interpreter;
    # validate that combination before compiling the library.
    subprocess.run([sys.executable, str(found[0]), '--help'], check=True)
    print('Selected GLib enum generator:', sys.executable, found[0], flush=True)
    return found[0]


def apply_patches(source):
    patches = source / 'debian/patches'
    for line in (patches / 'series').read_text().splitlines():
        fields = shlex.split(line, comments=True)
        if not fields:
            continue
        patch = (patches / fields[0]).resolve()
        if not patch.is_relative_to(patches.resolve()) or fields[1:]:
            raise ValueError('Unexpected Debian patch entry: ' + line)
        print('Applying Debian patch:', fields[0], flush=True)
        subprocess.run(['patch', '--batch', '--forward', '-p1', '-i', str(patch)], cwd=source, check=True)


def relocatable_data_patch(source):
    path = source / 'poppler/GlobalParams.cc'
    original = path.read_text()
    pattern = r'(GlobalParams::GlobalParams\(const char \*customPopplerDataDir\)[\s\S]*?\n\{)'
    replacement = r'\1\n    if (!customPopplerDataDir) customPopplerDataDir = std::getenv("POPPLER_DATADIR");'
    changed, count = re.subn(pattern, replacement, original, count=1)
    if count != 1:
        raise ValueError('Unrecognized Poppler data directory initializer')
    changed = '#include <cstdlib>\n' + changed
    patch = ''.join(difflib.unified_diff(original.splitlines(keepends=True), changed.splitlines(keepends=True),
                                       fromfile='a/poppler/GlobalParams.cc', tofile='b/poppler/GlobalParams.cc'))
    (source / 'komamimic-relocatable-data.patch').write_text(patch)
    path.write_text(changed)
    return hashlib.sha256(patch.encode()).hexdigest()


def build(output):
    output = output.resolve()
    work = output / 'poppler-work'
    work.mkdir(exist_ok=False)
    dsc_name = 'poppler_' + REVISION + '.dsc'
    dsc = work / dsc_name
    dsc_hash = fetch(dsc_name, dsc)
    descriptor = dsc.read_text()
    if '\nVersion: ' + REVISION + '\n' not in descriptor or '\nSource: poppler\n' not in descriptor:
        raise ValueError('Unexpected Debian source identity/version')
    records = checksums(descriptor)
    names = ['poppler_' + VERSION + '.orig.tar.xz', 'poppler_' + REVISION + '.debian.tar.xz']
    for name in names:
        fetch(name, work / name, records[name])
    # Verify checksums before extracting. Python 3.11's data filter rejects
    # escaping paths, devices and unsafe archive links.
    with tarfile.open(work / names[0]) as archive:
        archive.extractall(work, filter='data')
    source = work / ('poppler-' + VERSION)
    with tarfile.open(work / names[1]) as archive:
        archive.extractall(source, filter='data')
    apply_patches(source)
    data_patch_hash = relocatable_data_patch(source)
    tiff = unique_library(package_files('libtiff6'), 'libtiff.so.6')
    tiff_include = header_directory(package_files('libtiff6-devel'), 'tiffio.h')
    jpeg = unique_library(list(map(str, Path(os.environ['KOMA_PROBE_JPEG_LIBDIR']).glob('libjpeg.so.62*'))),
                          'libjpeg.so.62')
    jpeg_include = header_directory(package_files('libjpeg-turbo-devel'), 'jpeglib.h')
    freetype_include, freetype = freetype_paths()
    mkenums = glib_mkenums()
    # RX's devel Cairo symlink may be broken. Use a private build-only link
    # and corrected pkg-config copy, leaving the installed RPM files intact.
    dependencies = work / 'dependencies'
    (dependencies / 'lib').mkdir(parents=True)
    (dependencies / 'pkgconfig').mkdir()
    (dependencies / 'lib/libcairo.so').symlink_to(os.environ['KOMA_PROBE_CAIRO_LIBRARY'])
    pcdir = subprocess.check_output(['pkg-config', '--variable=pcfiledir', 'cairo'], text=True).strip()
    cairo_pc = (Path(pcdir) / 'cairo.pc').read_text()
    cairo_pc = re.sub(r'^libdir=.*$', 'libdir=' + str(dependencies / 'lib'), cairo_pc, flags=re.MULTILINE)
    (dependencies / 'pkgconfig/cairo.pc').write_text(cairo_pc)
    env = dict(os.environ)
    env['PKG_CONFIG_PATH'] = str(dependencies / 'pkgconfig') + ':' + env.get('PKG_CONFIG_PATH', '')
    env['LD_LIBRARY_PATH'] = ':'.join([str(tiff.parent), str(freetype.parent), os.environ['KOMA_PROBE_JPEG_LIBDIR'],
                                    os.environ['KOMA_PROBE_CAIRO_LIBDIR'],
                                    subprocess.check_output(['pkg-config', '--variable=libdir', 'glib-2.0'], text=True).strip(),
                                    env.get('LD_LIBRARY_PATH', '')])
    # Validate private .pc dependencies too; a version/--exists query alone
    # does not guarantee that compilation flags can be retrieved.
    subprocess.run(['pkg-config', '--print-errors', '--cflags', '--libs',
                    'fontconfig', 'freetype2', 'cairo', 'gio-2.0'], env=env, check=True)
    prefix = output / 'poppler-prefix'
    command = ['cmake', '-S', str(source), '-B', str(output / 'poppler-build'), '-G', 'Ninja',
               '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=ON',
               '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_INSTALL_LIBDIR=lib',
               '-DCMAKE_PREFIX_PATH=/opt/rx', '-DCMAKE_LIBRARY_PATH=' + str(dependencies / 'lib'),
               '-DTIFF_INCLUDE_DIR=' + tiff_include, '-DTIFF_LIBRARY=' + str(tiff), '-DTIFF_LIBRARY_RELEASE=' + str(tiff),
               '-DJPEG_INCLUDE_DIR=' + jpeg_include, '-DJPEG_LIBRARY=' + str(jpeg), '-DJPEG_LIBRARY_RELEASE=' + str(jpeg),
               '-DFREETYPE_INCLUDE_DIR_ft2build=' + str(freetype_include),
               '-DFREETYPE_INCLUDE_DIR_freetype2=' + str(freetype_include),
               '-DFREETYPE_LIBRARY_RELEASE=' + str(freetype),
               '-DGLIB2_MKENUMS=' + str(mkenums),
               '-DPYTHON_EXECUTABLE=' + sys.executable, '-DPython3_EXECUTABLE=' + sys.executable,
               '-DENABLE_GLIB=ON', '-DENABLE_QT5=OFF', '-DENABLE_QT6=OFF', '-DENABLE_CPP=OFF', '-DENABLE_UTILS=OFF',
               '-DENABLE_GOBJECT_INTROSPECTION=OFF', '-DENABLE_GTK_DOC=OFF', '-DBUILD_GTK_TESTS=OFF',
               '-DBUILD_QT5_TESTS=OFF', '-DBUILD_QT6_TESTS=OFF', '-DBUILD_CPP_TESTS=OFF', '-DBUILD_MANUAL_TESTS=OFF',
               '-DENABLE_BOOST=OFF', '-DENABLE_NSS3=OFF', '-DENABLE_LIBCURL=OFF',
               '-DENABLE_LIBOPENJPEG=openjpeg2', '-DENABLE_LIBJPEG=ON', '-DENABLE_LIBPNG=ON', '-DENABLE_LIBTIFF=ON']
    print('Poppler configuration:', shlex.join(command), flush=True)
    subprocess.run(command, env=env, check=True)
    subprocess.run(['cmake', '--build', str(output / 'poppler-build'), '--parallel', '4'], env=env, check=True)
    subprocess.run(['cmake', '--install', str(output / 'poppler-build')], env=env, check=True)
    env['LD_LIBRARY_PATH'] = str(prefix / 'lib') + ':' + env['LD_LIBRARY_PATH']
    env['PKG_CONFIG_PATH'] = str(prefix / 'lib/pkgconfig') + ':' + env['PKG_CONFIG_PATH']
    subprocess.run(['pkg-config', '--modversion', 'poppler-glib'], env=env, check=True)
    subprocess.run(['pkg-config', '--print-errors', '--cflags', '--libs', 'poppler-glib', 'cairo'],
                   env=env, check=True)
    for library in (prefix / 'lib').glob('*.so'):
        needed = subprocess.check_output(['readelf', '-d', str(library)], text=True, env=env)
        print(needed, flush=True)
        if '[libtiff.so.5]' in needed:
            raise ValueError('Poppler still depends on TIFF ABI 5')
    notices = output / 'poppler-notices'
    notices.mkdir()
    for name in ('COPYING', 'COPYING3', 'AUTHORS', 'README-XPDF', 'debian/copyright'):
        shutil.copy2(source / name, notices / name.replace('/', '-'))
    metadata = dict(version=REVISION, source_descriptor_url=BASE + dsc_name, source_descriptor_sha256=dsc_hash,
                    inputs={name: records[name] for name in names}, configure=command, tiff=str(tiff),
                    relocatable_data_patch_sha256=data_patch_hash)
    (output / 'poppler-source.json').write_text(json.dumps(metadata, indent=2) + '\n')
    with tarfile.open(output / ('poppler-' + REVISION + '-source.tar.gz'), 'w:gz') as archive:
        archive.add(source, arcname=source.name)
        archive.add(dsc, arcname=dsc_name)
    with open(os.environ['GITHUB_ENV'], 'a') as stream:
        stream.write('PKG_CONFIG_PATH=' + env['PKG_CONFIG_PATH'] + '\n')
        stream.write('LD_LIBRARY_PATH=' + env['LD_LIBRARY_PATH'] + '\n')
        stream.write('KOMA_PROBE_POPPLER_LIBRARY=' + str(prefix / 'lib/libpoppler-glib.so') + '\n')
        stream.write('POPPLER_DATADIR=/usr/share/poppler\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    build(parser.parse_args().output)
