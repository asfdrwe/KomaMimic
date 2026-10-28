#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Install a matching official prebuilt Qt SDK; never compile Qt or WebEngine."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import sys
import venv

CONFIG = json.loads(Path(__file__).with_name('windows-fast.json').read_text(encoding='utf-8'))


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def verify_sdk(sdk):
    missing = [name for name in CONFIG['required_files'] if not (sdk / name).is_file()]
    if missing:
        raise ValueError('Incomplete Qt SDK: ' + ', '.join(missing))
    # Reject accidental ARM64/MinGW or another Qt version before vcpkg installs anything.
    core = sdk / 'bin/Qt6Core.dll'
    with core.open('rb') as stream:
        header = stream.read(64)
        if header[:2] != b'MZ' or len(header) < 64:
            raise ValueError('Invalid Qt6Core.dll PE header')
        stream.seek(struct.unpack_from('<I', header, 60)[0])
        if stream.read(6) != b'PE\0\0\x64\x86':
            raise ValueError('Expected an x64 Qt SDK')
    version = subprocess.check_output([str(sdk / 'bin/qtpaths.exe'), '--query', 'QT_VERSION'],
                                      text=True, encoding='utf-8').strip()
    if version != CONFIG['qt_version']:
        raise ValueError('Expected Qt ' + CONFIG['qt_version'] + ', found ' + version)
    if not (sdk / 'mkspecs/win32-msvc/qmake.conf').is_file():
        raise ValueError('Expected the MSVC SDK (not MinGW)')
    return dict(schema=1, provider='Qt official binary archives via aqtinstall',
                config=CONFIG, version=version, architecture='x64-msvc2022',
                files=[dict(path=name, sha256=digest(sdk / name)) for name in CONFIG['required_files']])


def installer_commands(python, output):
    commit = CONFIG['aqt_commit']
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('aqtinstall must be pinned to a full upstream commit')
    # The PyPI 3.3.0 release cannot resolve the Windows Qt 6.11 SDK repository.
    # This is upstream PR #1000's merge commit; never follow a moving branch.
    requirement = 'aqtinstall @ git+https://github.com/miurahr/aqtinstall.git@' + commit
    preflight = (
        'from aqt.metadata import QtRepoProperty, Version; '
        'actual = QtRepoProperty.extension_for_arch(' + repr(CONFIG['architecture']) +
        ', Version(' + repr(CONFIG['qt_version']) + ')); '
        'assert actual == ' + repr(CONFIG['sdk_directory']) +
        ', "aqtinstall lacks Windows Qt 6.11 repository support"; '
        'print("Qt repository architecture:", actual)')
    return [
        [python, '-m', 'pip', 'install', '--upgrade', '--disable-pip-version-check',
         requirement, 'py7zr==' + CONFIG['py7zr_version']],
        [python, '-c', preflight],
        [python, '-m', 'aqt', 'install-qt', 'windows', 'desktop', CONFIG['qt_version'],
         CONFIG['architecture'], '--outputdir', output, '--modules', *CONFIG['modules']]]


def setup(output, verify_only=False):
    if sys.version_info < (3, 10):
        raise ValueError('Qt SDK setup requires Python 3.10 or newer (py7zr 1.1.0)')
    if platform.system() != 'Windows':
        raise ValueError('Run this installer on native x64 Windows')
    output = output.resolve()
    sdk = output / CONFIG['qt_version'] / CONFIG['sdk_directory']
    if not sdk.exists() and not verify_only:
        output.mkdir(parents=True, exist_ok=True)
        environment = output / '.aqt-venv'
        if not (environment / 'Scripts/python.exe').is_file():
            venv.EnvBuilder(with_pip=True).create(environment)
        python = environment / 'Scripts/python.exe'
        env = dict(os.environ, PYTHONUTF8='1')
        commands = installer_commands(python, output)
        for command in commands:
            print('+', subprocess.list2cmdline([str(x) for x in command]), flush=True)
            subprocess.run([str(x) for x in command], env=env, check=True)
    # Do not reuse an incomplete cache silently, delete or rename the version folder
    # explicitly before retrying a failed SDK download. Never mix SDK versions.
    info = verify_sdk(sdk)
    (sdk / 'KOMAMIMIC_QT_SDK.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
    print('KOMAMIMIC_QT_ROOT=' + str(sdk))
    return sdk


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='SDK installation parent, e.g. H:/qt-fast')
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    try:
        setup(args.output, args.verify_only)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
