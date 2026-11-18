#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Update only libarchive while preserving the pinned vcpkg port's patches and dependencies."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tarfile
import tempfile

from fetch_release_inputs import ROOT, configuration, download, verify


def verify_source_patches(archive, version, port):
    # Check the actual verified release bytes, not a web-rendered source view.
    # Only three small CMake inputs are written; no compiler or build is run.
    relatives = ('CMakeLists.txt', 'libarchive/CMakeLists.txt', 'build/cmake/CreatePkgConfigFile.cmake')
    with tempfile.TemporaryDirectory(prefix='libarchive-patch-check-', dir=port.parent) as temporary:
        source = Path(temporary)
        with tarfile.open(archive) as upstream:
            for relative in relatives:
                target = source / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(upstream.extractfile('libarchive-' + version + '/' + relative).read())
        for name in ('fix-buildsystem.patch', 'fix-deps.patch'):
            result = subprocess.run(['git', 'apply', '--no-index', '--verbose', '--ignore-whitespace',
                                     '--whitespace=nowarn', str((port / name).resolve())],
                                    cwd=source, text=True, capture_output=True)
            print(result.stdout + result.stderr, end='', flush=True)
            if result.returncode:
                raise RuntimeError('Verified libarchive release source cannot apply ' + name)


def setup(vcpkg, output, prepared_inputs=None):
    name, entry = next((name, entry) for name, entry in configuration().items()
                       if name.startswith('libarchive-'))
    version = re.fullmatch(r'libarchive-([0-9.]+)\.tar\.gz', name)[1]
    original = vcpkg / 'ports/libarchive'
    recipe = (original / 'portfile.cmake').read_text(encoding='utf-8')
    block = re.search(r'^vcpkg_from_github\((.*?)^\)', recipe, re.M | re.S)
    if not block or 'REPO libarchive/libarchive' not in block[1]:
        raise ValueError('Unrecognized pinned libarchive port source block')
    patches = re.search(r'\bPATCHES\s+(.*)', block[1], re.S)
    if not patches:
        raise ValueError('Pinned libarchive port has no patch list')
    names = shlex.split(patches[1], comments=True)
    if names != ['fix-buildsystem.patch', 'fix-deps.patch'] or any(not (original / p).is_file() for p in names):
        raise ValueError('Unrecognized pinned libarchive patches')
    manifest = json.loads((original / 'vcpkg.json').read_text(encoding='utf-8'))
    if manifest.get('name') != 'libarchive':
        raise ValueError('Wrong vcpkg source port')
    output.parent.mkdir(parents=True, exist_ok=True)
    archive = prepared_inputs / name if prepared_inputs is not None else output.parent / name
    if prepared_inputs is None:
        download(entry, archive)
    verify(entry, archive)
    # Seed vcpkg's normal asset cache. Its standard download helper will verify
    # SHA-512 as well; the authoritative expected SHA-256 was checked above.
    downloads = vcpkg / 'downloads'
    downloads.mkdir(exist_ok=True)
    shutil.copy2(archive, downloads / name)
    checksum = hashlib.sha512(archive.read_bytes()).hexdigest()
    urls = ' '.join('"' + url + '"' for url in (entry['url'], *entry.get('mirrors', [])))
    replacement = (f'vcpkg_download_distfile(ARCHIVE\n    URLS {urls}\n'
                   f'    FILENAME "{name}"\n    SHA512 {checksum}\n)\n'
                   'vcpkg_extract_source_archive(SOURCE_PATH\n    ARCHIVE "${ARCHIVE}"\n'
                   '    PATCHES\n' + ''.join('        ' + p + '\n' for p in names) + ')')
    port = output / 'libarchive'
    output.mkdir(exist_ok=False)
    shutil.copytree(original, port)
    # Keep the dependency fixes and their license, with release-source context.
    for filename in ('fix-deps.patch', 'LICENSE-vcpkg.txt'):
        shutil.copy2(ROOT / 'cmake/ports/libarchive' / filename, port / filename)
    (port / 'portfile.cmake').write_text(recipe[:block.start()] + replacement + recipe[block.end():], encoding='utf-8')
    manifest['version'] = version
    manifest.pop('port-version', None)
    (port / 'vcpkg.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    verify_source_patches(archive, version, port)
    print('Prepared libarchive ' + version + ' overlay: ' + str(port), flush=True)
    return port


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vcpkg-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--prepared-inputs', type=Path)
    args = parser.parse_args()
    setup(args.vcpkg_root.resolve(), args.output.resolve(),
          args.prepared_inputs.resolve() if args.prepared_inputs is not None else None)
