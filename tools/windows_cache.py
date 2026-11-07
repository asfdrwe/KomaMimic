#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Identify Windows dependencies and save only new/completed binary-cache data."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
# Root-project metadata does not change dependency resolution. Keep every other
# field, including unknown future dependency settings, in the cache identity.
METADATA = {'$schema', 'name', 'version', 'version-string', 'version-semver',
            'version-date', 'port-version', 'description', 'license', 'homepage',
            'documentation', 'maintainers', 'contacts', 'summary'}


def dependency_identity(root):
    root = Path(root)
    manifest = json.loads((root / 'cmake/windows-fast/vcpkg.json').read_text(encoding='utf-8'))
    baseline = manifest.get('builtin-baseline', '')
    if not re.fullmatch(r'[0-9a-f]{40}', baseline):
        raise ValueError('Invalid vcpkg baseline')
    inputs = {}
    paths = [root / 'cmake/WindowsFast.cmake', *sorted((root / 'cmake/triplets').glob('*.cmake'))]
    for name in ('vcpkg-configuration.json', 'cmake/windows-fast/vcpkg-configuration.json'):
        if (root / name).is_file():
            paths.append(root / name)
    for path in paths:
        inputs[path.relative_to(root).as_posix()] = path.read_text(encoding='utf-8')
    payload = {'schema': 1, 'manifest': {k: v for k, v in manifest.items() if k not in METADATA},
               'inputs': inputs}
    digest = hashlib.sha256(json.dumps(payload, sort_keys=True, separators=(',', ':')).encode('utf-8')).hexdigest()
    return {'commit': baseline, 'dependency-key': digest}


def inventory(directory):
    directory = Path(directory)
    if directory.is_symlink():
        raise ValueError('Binary cache directory must not be a symlink')
    files = {}
    # vcpkg's files provider stores completed, ABI-addressed packages as ZIPs.
    # Ignore incomplete temporary files; stat only, never rehash large archives.
    for path in sorted(directory.rglob('*.zip')):
        if path.is_symlink() or not path.is_file():
            continue
        stat = path.stat()
        files[path.relative_to(directory).as_posix()] = [stat.st_size, stat.st_mtime_ns]
    return {'schema': 1, 'directory': str(directory.resolve()), 'files': files}


def save_decision(before, after, restored_key, family_prefix):
    if before.get('schema') != 1 or after.get('schema') != 1 or before['directory'] != after['directory']:
        raise ValueError('Binary cache snapshot does not match this directory')
    if not family_prefix:
        raise ValueError('Missing dependency cache family')
    files = after['files']
    changed = sum(before['files'].get(name) != value for name, value in files.items())
    removed = len(before['files'].keys() - files.keys())
    migrate = bool(files) and not restored_key.startswith(family_prefix)
    return {'save': bool(files) and (bool(changed) or bool(removed) or migrate), 'packages': len(files),
            'bytes': sum(value[0] for value in files.values()), 'changed': changed, 'removed': removed,
            'migrate': migrate, 'restored-key': restored_key, 'family-prefix': family_prefix}


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def github_outputs(path, values):
    if path:
        with Path(path).open('a', encoding='utf-8') as stream:
            for name, value in values.items():
                text = json.dumps(value) if isinstance(value, bool) else str(value)
                if '\n' in text or '\r' in text:
                    raise ValueError('Invalid multiline action output')
                stream.write(f'{name}={text}\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    identity = commands.add_parser('identity')
    identity.add_argument('--root', type=Path, default=ROOT)
    identity.add_argument('--github-output', type=Path)
    snapshot = commands.add_parser('snapshot')
    snapshot.add_argument('--directory', type=Path, required=True)
    snapshot.add_argument('--output', type=Path, required=True)
    assess = commands.add_parser('assess')
    assess.add_argument('--directory', type=Path, required=True)
    assess.add_argument('--before', type=Path, required=True)
    assess.add_argument('--restored-key', default='')
    assess.add_argument('--family-prefix', required=True)
    assess.add_argument('--output', type=Path, required=True)
    assess.add_argument('--github-output', type=Path)
    args = parser.parse_args()
    if args.command == 'identity':
        result = dependency_identity(args.root)
        github_outputs(args.github_output, result)
    elif args.command == 'snapshot':
        result = inventory(args.directory)
        write_json(args.output, result)
        result = {'packages': len(result['files']), 'directory': result['directory']}
    else:
        before = json.loads(args.before.read_text(encoding='utf-8'))
        result = save_decision(before, inventory(args.directory), args.restored_key, args.family_prefix)
        write_json(args.output, result)
        github_outputs(args.github_output, {'save': result['save']})
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit(str(error))
