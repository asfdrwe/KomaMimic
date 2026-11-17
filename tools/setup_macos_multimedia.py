#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build matching Qt Multimedia with its FFmpeg backend (Python 3.12+).

Homebrew disables that backend on macOS. Keep its Qt installation untouched;
install this one module under the requested work directory instead.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import urllib.request

from run_validation import execute, save_json
from stage_validation import tool


def source_identity(formula, qt_version):
    stable = formula['urls']['stable']
    url, checksum = stable['url'], stable['checksum']
    version = formula['versions']['stable']
    if version != qt_version:
        raise ValueError('Qt Multimedia source and installed Qt base must match: '
                         + version + ' != ' + qt_version)
    expected = f'https://download.qt.io/official_releases/qt/{version.rsplit(".", 1)[0]}/{version}/submodules/qtmultimedia-everywhere-src-{version}.tar.xz'
    if url != expected or not re.fullmatch(r'[0-9a-f]{64}', checksum):
        raise ValueError('Unexpected Qt Multimedia source URL or checksum')
    return dict(version=version, url=url, sha256=checksum)


def extract_source(archive, destination, identity):
    if hashlib.sha256(archive.read_bytes()).hexdigest() != identity['sha256']:
        raise ValueError('Qt Multimedia source checksum mismatch')
    # data filtering prevents absolute/escaping members and unsafe links.
    with tarfile.open(archive) as stream:
        stream.extractall(destination, filter='data')
    source = destination / ('qtmultimedia-everywhere-src-' + identity['version'])
    if not (source / 'CMakeLists.txt').is_file():
        raise ValueError('Qt Multimedia source layout mismatch')
    return source


def configure_command(source, build, prefix, qt_prefixes, ffmpeg):
    return ['cmake', '-S', source, '-B', build, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_OSX_ARCHITECTURES=arm64',
            '-DCMAKE_INSTALL_PREFIX=' + str(prefix),
            '-DCMAKE_PREFIX_PATH=' + ';'.join(map(str, [*qt_prefixes, ffmpeg])),
            '-DQT_ADDITIONAL_PACKAGES_PREFIX_PATH=' + ';'.join(map(str, qt_prefixes)),
            '-DFFMPEG_DIR=' + str(ffmpeg), '-DQT_FEATURE_ffmpeg=ON',
            '-DQT_DEFAULT_MEDIA_BACKEND=ffmpeg',
            '-DQT_BUILD_EXAMPLES=OFF', '-DQT_BUILD_TESTS=OFF',
            '-DQT_NO_APPLE_SDK_AND_XCODE_CHECK=ON']


def plugin_directory(prefix):
    roots = [prefix / name for name in ('plugins', 'share/qt/plugins', 'lib/qt6/plugins')]
    for root in roots:
        if (root / 'multimedia/libffmpegmediaplugin.dylib').is_file():
            return root
    raise ValueError('Built Qt Multimedia FFmpeg plugin is missing')


def setup(output, jobs):
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise ValueError('Use native macOS arm64')
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    metadata = json.loads(subprocess.check_output(['brew', 'info', '--json=v2', '--installed'], text=True))
    formulae = {formula['name']: formula for formula in metadata['formulae']}
    names = ['qtbase', *sorted(n for n in formulae if re.fullmatch(r'qt[a-z0-9]+', n) and n != 'qtbase')]
    prefixes = [Path(subprocess.check_output(['brew', '--prefix', n], text=True).strip()) for n in names]
    qt_version = subprocess.check_output([tool(prefixes[0] / 'bin', 'qtpaths', 'qtpaths6'), '--qt-version'], text=True).strip()
    identity = source_identity(formulae['qtmultimedia'], qt_version)
    save_json(output / 'SOURCE.json', identity)
    archive = output / ('qtmultimedia-' + qt_version + '.tar.xz')
    with urllib.request.urlopen(identity['url'], timeout=120) as response, archive.open('wb') as stream:
        shutil.copyfileobj(response, stream)
    source = extract_source(archive, output / 'source', identity)
    prefix, build = output / 'install', output / 'build'
    ffmpeg = Path(subprocess.check_output(['brew', '--prefix', 'ffmpeg'], text=True).strip())
    commands = [configure_command(source, build, prefix, prefixes, ffmpeg),
                ['cmake', '--build', build, '--parallel', str(jobs)],
                ['cmake', '--install', build]]
    for name, command in zip(('configure', 'build', 'install'), commands):
        if execute(command, output / (name + '.log'))['returncode']:
            raise RuntimeError('Qt Multimedia ' + name + ' failed; see ' + str(output))
    plugins = plugin_directory(prefix)
    if not (prefix / 'lib/cmake/Qt6Multimedia/Qt6MultimediaConfig.cmake').is_file():
        raise ValueError('Built Qt Multimedia CMake package is missing')
    report = dict(schema=1, source=identity, prefix=str(prefix), plugins=str(plugins),
                  archive=str(archive), commands=[[str(x) for x in cmd] for cmd in commands])
    save_json(output / 'MULTIMEDIA_BUILD.json', report)
    print('Qt Multimedia FFmpeg backend installed: ' + str(prefix))
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, choices=range(1, 9), default=3)
    args = parser.parse_args()
    setup(args.output, args.jobs)
