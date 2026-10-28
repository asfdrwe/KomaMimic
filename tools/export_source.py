#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Export reviewed public source; private material is never an input dependency."""
import argparse
import hashlib
import json
import re
from pathlib import Path, PurePosixPath
import shutil

POLICY_FILE = 'tools/public_source.json'


def _relative(value):
    path = PurePosixPath(value)
    if not value or path.is_absolute() or '..' in path.parts or str(path) != value or '\\' in value or ':' in value or '\0' in value:
        raise ValueError(f'Invalid policy path: {value}')
    return value


def load_policy(root):
    root = Path(root).resolve()
    policy_path = root / POLICY_FILE
    if policy_path.is_symlink():
        raise ValueError('Symlinked source policy')
    policy = json.loads(policy_path.read_text(encoding='utf-8'))
    if policy['schema'] != 1 or policy['runtime_sample_data'] is not False:
        raise ValueError('Unsupported source policy')
    for name in policy['root_files'] + policy['documents'] + policy['excluded_paths'] + list(policy['source_trees']):
        _relative(name)
    if len(policy['documents']) != len(set(policy['documents'])):
        raise ValueError('Duplicate reviewed document')
    if any(not name.startswith('docs/') or not name.endswith('.md') for name in policy['documents']):
        raise ValueError('Reviewed documents must be Markdown below docs/')
    paths = set()
    for asset in policy.get('reviewed_assets', []):
        name = _relative(asset['path'])
        screenshot = name.startswith('docs/site/assets/screenshots/') and Path(name).suffix == '.png'
        if ((not name.startswith('assets/') and not screenshot) or Path(name).suffix not in
                ('.svg', '.png', '.ico', '.icns', '.desktop')
                or name in paths or asset.get('license') != 'GPL-3.0-only'
                or not re.fullmatch('[0-9a-f]{64}', asset.get('sha256', ''))):
            raise ValueError(f'Invalid reviewed asset: {name}')
        paths.add(name)
    return policy


def _excluded(relative, policy):
    return '__pycache__' in relative.parts or any(
        relative == PurePosixPath(prefix) or PurePosixPath(prefix) in relative.parents
        for prefix in policy['excluded_paths'])


def _checked_file(root, relative):
    path = root / relative
    for parent in (path, *path.parents):
        if parent == root:
            break
        if parent.is_symlink():
            raise ValueError(f'Symlink in public source: {relative}')
    if not path.is_file():
        raise ValueError(f'Public source missing: {relative}')
    return path


def public_files(root):
    root = Path(root).resolve()
    policy = load_policy(root)
    names = set(policy['root_files']) | set(policy['documents']) | {POLICY_FILE}
    for name, suffixes in policy['source_trees'].items():
        directory = root / name
        if directory.is_symlink() or not directory.is_dir():
            raise ValueError(f'Source directory missing or symlinked: {name}')
        for path in sorted(directory.rglob('*')):
            relative = path.relative_to(root)
            if _excluded(PurePosixPath(relative.as_posix()), policy):
                continue
            if path.is_symlink():
                raise ValueError(f'Symlink in public source tree: {relative}')
            if path.is_file() and path.suffix in suffixes:
                names.add(relative.as_posix())
    # Product assets are individually reviewed; sample images remain excluded.
    assets = {entry['path']: entry for entry in policy.get('reviewed_assets', [])}
    directory = root / 'assets'
    if directory.is_symlink():
        raise ValueError('Symlinked public assets directory')
    for path in directory.rglob('*'):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            raise ValueError(f'Symlink in public assets: {relative}')
        if path.is_file() and relative not in assets:
            raise ValueError(f'Unclassified public asset: {relative}')
    for name, entry in assets.items():
        path = _checked_file(root, name)
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
            raise ValueError(f'Reviewed asset hash mismatch: {name}')
        names.add(name)
    # An unreviewed document must be classified before an export can succeed.
    directory = root / 'docs'
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError('Public docs directory missing or symlinked')
    for path in directory.rglob('*'):
        relative = path.relative_to(root)
        if path.is_symlink():
            raise ValueError(f'Symlink in public docs: {relative}')
        if path.is_file() and relative.as_posix() not in policy['documents'] and relative.as_posix() not in assets:
            raise ValueError(f'Unclassified public document: {relative}')
    for name in names:
        if _excluded(PurePosixPath(name), policy):
            raise ValueError(f'Public/private policy overlap: {name}')
    return [_checked_file(root, name) for name in sorted(names)]


def export(root, output, manifest=None):
    root, output = Path(root).resolve(), Path(output).resolve()
    policy = load_policy(root)
    trees = [*policy['source_trees'], 'docs', 'assets']
    if output == root or any(output.is_relative_to(root / name) for name in trees):
        raise ValueError('Output must be outside the selected source directories')
    selected = public_files(root)
    if manifest is not None:
        manifest = Path(manifest).absolute()
        if manifest.exists() or manifest.is_symlink():
            raise ValueError('Manifest must be a new file')
        manifest = manifest.resolve()
        if manifest.is_relative_to(output) or any(
                manifest.is_relative_to(root / name) for name in trees):
            raise ValueError('Manifest must be outside exported/source directories')
    output.mkdir(parents=True, exist_ok=False)
    records = []
    for path in selected:
        relative = path.relative_to(root).as_posix()
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        records.append({'path': relative, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    if manifest is not None:
        with manifest.open('x', encoding='utf-8') as stream:
            json.dump({'schema': 1, 'selection': 'public-repository', 'files': records}, stream, indent=2)
            stream.write('\n')
    return len(selected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, help='new inventory file outside the exported/source directories')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    count = export(root, args.output, args.manifest)
    print(f'Exported {count} source files to {args.output}')


if __name__ == '__main__':
    main()
