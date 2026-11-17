# SPDX-License-Identifier: GPL-3.0-only
"""Private OS directories for automated checks and all their children."""
import os
from pathlib import Path
import sys


def isolated_environment(root, base=None, system=None):
    # macOS /var aliases /private/var; Qt and Python must agree on one root.
    root = Path(root).resolve()
    env = dict(os.environ if base is None else base)
    paths = {
        'HOME': root / 'home',
        'XDG_CONFIG_HOME': root / 'config',
        'XDG_CACHE_HOME': root / 'cache',
        'XDG_DATA_HOME': root / 'data',
        'XDG_STATE_HOME': root / 'state',
        'XDG_RUNTIME_DIR': root / 'run',
        'TMPDIR': root, 'TMP': root, 'TEMP': root,
    }
    system = system or sys.platform
    if system == 'darwin':
        paths['CFFIXED_USER_HOME'] = paths['HOME']
    if system == 'win32':
        paths.update(USERPROFILE=paths['HOME'], APPDATA=paths['HOME'] / 'AppData/Roaming',
                     LOCALAPPDATA=paths['HOME'] / 'AppData/Local')
        env['HOMEDRIVE'] = paths['HOME'].drive
        env['HOMEPATH'] = str(paths['HOME'])[len(paths['HOME'].drive):]
    for name, path in paths.items():
        path.mkdir(parents=True, exist_ok=True)
        env[name] = str(path)
    paths['XDG_RUNTIME_DIR'].chmod(0o700)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['KOMAMIMIC_TEST_ROOT'] = str(root)
    return env

