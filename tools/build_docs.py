#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build preserved v1 routes and a separate v2 guide into one Pages artifact."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import re
import subprocess
import sys
import tarfile
import tempfile

V1_TAG = 'v1.7.3'
V1_COMMIT = '28b42a6ad17547f316074cf9346306bef3a36aa0'
ROOT = Path(__file__).resolve().parents[1]


def frozen_v1(root, destination):
    """Read only the pinned release, never main or an unverified moving tag."""
    actual = subprocess.check_output(
        ['git', '-C', str(root), 'rev-parse', V1_TAG + '^{commit}'], text=True).strip()
    if actual != V1_COMMIT:
        raise ValueError('The v1 documentation tag differs from its pinned commit')
    archive = destination.parent / 'v1-docs.tar'
    subprocess.run(['git', '-C', str(root), 'archive', '--format=tar',
                    '--output', str(archive), V1_COMMIT, 'mkdocs.yml', 'docs/site'], check=True)
    destination.mkdir()
    with tarfile.open(archive) as source:
        source.extractall(destination, filter='data')
    return hashlib.sha256(archive.read_bytes()).hexdigest()


def build_guide(source, destination, config_file, major, base_url):
    import yaml  # Installed by requirements-docs.txt in the documentation job.
    config = yaml.safe_load((source / 'mkdocs.yml').read_text(encoding='utf-8'))
    config['docs_dir'] = str((source / config.get('docs_dir', 'docs')).resolve())
    config['site_name'] = 'KomaMimic v' + str(major) + ' Guide'
    config['site_url'] = base_url + ('v2/' if major == 2 else '')
    config['nav'].append({'Versions / バージョン': [
        {'v1': base_url}, {'v2': base_url + 'v2/'}]})
    config_file.write_text(yaml.safe_dump(config, allow_unicode=True, sort_keys=False), encoding='utf-8')
    subprocess.run([sys.executable, '-m', 'mkdocs', 'build', '--strict',
                    '--config-file', str(config_file), '--site-dir', str(destination)], check=True)


def combine(v1, v2, output):
    if output.exists():
        raise ValueError('Documentation output must be a new directory')
    # Keep every existing v1 URL at the root. Build v2 in its own namespace.
    # Copy from a separate tree so /v1 never recursively copies itself.
    shutil.copytree(v1, output)
    shutil.copytree(v1, output / 'v1')
    shutil.copytree(v2, output / 'v2')


def build(root, output):
    import yaml
    root, output = Path(root).resolve(), Path(output).resolve()
    if output.exists():
        raise ValueError('Documentation output must be a new directory')
    output.parent.mkdir(parents=True, exist_ok=True)
    version = re.search(r'project\(KomaMimic VERSION ([0-9.]+)', (root / 'CMakeLists.txt').read_text(encoding='utf-8')).group(1)
    if not version.startswith('2.'):
        raise ValueError('The documentation source must be v2')
    base_url = yaml.safe_load((root / 'mkdocs.yml').read_text(encoding='utf-8'))['site_url'].rstrip('/') + '/'
    with tempfile.TemporaryDirectory(prefix='docs-build-', dir=output.parent) as temporary:
        work = Path(temporary)
        digest = frozen_v1(root, work / 'frozen-v1')
        build_guide(work / 'frozen-v1', work / 'v1-site', work / 'v1.yml', 1, base_url)
        build_guide(root, work / 'v2-site', work / 'v2.yml', 2, base_url)
        combine(work / 'v1-site', work / 'v2-site', output)
        (output / 'DOCUMENTATION-VERSIONS.json').write_text(json.dumps(dict(
            v1=dict(tag=V1_TAG, commit=V1_COMMIT, archive_sha256=digest, paths=['/', '/v1/']),
            v2=dict(path='/v2/', version=version, development=False)), indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build(args.root, args.output)
