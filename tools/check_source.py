#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check public source selection, document links and manifest consistency."""
import argparse
import ast
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote, urlsplit
from export_source import load_policy, public_files
from check_translations import check_translations


def check_tracked(root):
    """Reject tracked files that would be omitted from the reviewed source export."""
    root = Path(root).resolve()
    result = subprocess.run(['git', '-C', str(root), 'rev-parse', '--show-toplevel'],
                            check=True, capture_output=True, text=True, encoding='utf-8')
    if Path(result.stdout.strip()).resolve() != root:
        raise ValueError('Tracked-file check requires the public repository root')
    tracked = subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z']).decode('utf-8').split('\0')
    selected = {path.relative_to(root).as_posix() for path in public_files(root)}
    unexpected = sorted(set(filter(None, tracked)) - selected)
    if unexpected:
        raise ValueError('Tracked files outside public selection: ' + ', '.join(unexpected))


def check(root):
    root = Path(root).resolve()
    check_translations(root)
    selected = public_files(root)
    selected_set = set(selected)
    binary_assets = {root / entry['path'] for entry in load_policy(root).get('reviewed_assets', [])
                     if Path(entry['path']).suffix in ('.png', '.ico', '.icns')}
    for path in selected:
        if path in binary_assets:
            continue  # Exact bytes already checked against the reviewed SHA-256.
        content = path.read_text(encoding='utf-8')
        if '\0' in content:
            raise ValueError(f'Binary content: {path.relative_to(root)}')
        if path.suffix == '.py':
            ast.parse(content, filename=str(path))
        if path.suffix == '.json':
            json.loads(content)
        if path.suffix == '.md':
            for target in re.findall(r'\[[^\]]*\]\(([^)]+)\)', content):
                target = target.strip('<>')
                if urlsplit(target).scheme or target.startswith('#'):
                    continue
                local = (path.parent / unquote(urlsplit(target).path)).resolve()
                if local not in selected_set:
                    raise ValueError(f'{path.name}: unpublished or broken link {target}')
    manifest = json.loads((root / 'vcpkg.json').read_text(encoding='utf-8'))
    if not re.fullmatch('[0-9a-f]{40}', manifest['builtin-baseline']):
        raise ValueError('Invalid vcpkg baseline')
    if manifest['license'] != 'GPL-3.0-only':
        raise ValueError('Project license must be GPL-3.0-only')
    for name, year in (('pic', 2022), ('pic2', 2024)):
        script = (root / f'scripts/readers/{name}/decode.js').read_text(encoding='utf-8')
        provenance = json.loads((root / f'scripts/readers/{name}/upstream.json').read_text(encoding='utf-8'))
        if ('SPDX-License-Identifier: GPL-3.0-only AND 0BSD' not in script
                or f'SPDX-FileCopyrightText: {year} kaleido' not in script
                or 'Permission to use, copy, modify, and/or distribute' not in script
                or 'THE SOFTWARE IS PROVIDED "AS IS"' not in script
                or provenance['originalLicense'] != '0BSD'
                or not re.fullmatch('[0-9a-f]{64}', provenance['normalizedSourceSha256'])):
            raise ValueError(f'Missing {name} third-party license or provenance')
    for path in selected:
        if path.suffix in ('.cpp', '.h', '.py', '.cmake', '.sh', '.js') or path.name == 'CMakeLists.txt':
            if 'SPDX-License-Identifier: GPL-3.0-only' not in path.read_text(encoding='utf-8'):
                raise ValueError(f'Missing project license: {path.name}')
    cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8')
    version = re.search(r'project\(KomaMimic VERSION ([0-9.]+)', cmake).group(1)
    if manifest['version-string'] != version:
        raise ValueError('CMake and vcpkg versions differ')
    if 'setApplicationVersion(KOMAMIMIC_VERSION)' not in (root / 'src/main.cpp').read_text(encoding='utf-8'):
        raise ValueError('CLI version must use the CMake project version')
    if f'v{version}' not in (root / 'README.md').read_text(encoding='utf-8'):
        raise ValueError('README version differs from CMake')
    workflow = (root / '.github/workflows/build.yml').read_text(encoding='utf-8')
    for action in re.findall(r'uses:\s*(\S+)', workflow):
        if not re.fullmatch(r'actions/[a-z-]+@[0-9a-f]{40}', action):
            raise ValueError(f'Unpinned action: {action}')
    print(f'Public source: {len(selected)} files ({len(binary_assets)} reviewed binary assets); links, hashes, versions and action pins OK')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tracked', action='store_true', help='also reject unreviewed Git-tracked files at this repository root')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    check(root)
    if args.tracked:
        check_tracked(root)
