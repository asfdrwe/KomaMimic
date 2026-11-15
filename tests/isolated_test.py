#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run a test and its children with private standard OS directories."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def test_environment(root):
    root = Path(root).absolute()
    env = os.environ.copy()
    paths = {
        'HOME': root / 'home',
        'XDG_CONFIG_HOME': root / 'config',
        'XDG_CACHE_HOME': root / 'cache',
        'XDG_DATA_HOME': root / 'data',
        'XDG_STATE_HOME': root / 'state',
        'XDG_RUNTIME_DIR': root / 'run',
        'TMPDIR': root, 'TMP': root, 'TEMP': root,
    }
    if sys.platform == 'darwin':
        paths['CFFIXED_USER_HOME'] = paths['HOME']
    if os.name == 'nt':
        paths.update(USERPROFILE=paths['HOME'], APPDATA=paths['HOME'] / 'AppData/Roaming',
                     LOCALAPPDATA=paths['HOME'] / 'AppData/Local')
        env['HOMEDRIVE'] = paths['HOME'].drive
        env['HOMEPATH'] = str(paths['HOME'])[len(paths['HOME'].drive):]
    for name, path in paths.items():
        path.mkdir(parents=True, exist_ok=True)
        env[name] = str(path)
    paths['XDG_RUNTIME_DIR'].chmod(0o700)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    return env


if __name__ == '__main__':
    command = sys.argv[1:]
    if not command:
        raise SystemExit('A test command is required')
    with tempfile.TemporaryDirectory(prefix='k') as directory:
        raise SystemExit(subprocess.run(command, env=test_environment(directory), check=False).returncode)
