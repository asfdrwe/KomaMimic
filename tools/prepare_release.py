#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Create a deterministic, source-only release without private workspace files."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import tarfile

from check_source import check
from export_source import public_files


def prepare(root, output, epoch):
    root, output = root.resolve(), output.resolve()
    check(root)
    files = public_files(root)
    version = re.search(r'project\(KomaMimic VERSION ([0-9.]+)',
                        (root / 'CMakeLists.txt').read_text(encoding='utf-8')).group(1)
    name = f'KomaMimic-{version}'
    # Never replace an existing release directory or follow an output symlink.
    output.mkdir(parents=True, exist_ok=False)
    records = []
    archive_path = output / f'{name}-source.tar.gz'
    with archive_path.open('xb') as raw:
        with gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode='w', format=tarfile.PAX_FORMAT) as archive:
                for path in files:
                    relative = path.relative_to(root).as_posix()
                    data = path.read_bytes()
                    records.append({'path': relative, 'sha256': hashlib.sha256(data).hexdigest()})
                    entry = tarfile.TarInfo(f'{name}/{relative}')
                    entry.size = len(data)
                    entry.mode = 0o755 if path.suffix in {'.sh', '.py'} else 0o644
                    entry.mtime = epoch
                    archive.addfile(entry, io.BytesIO(data))
    manifest = {'name': 'KomaMimic', 'version': version, 'distribution': 'source-only',
                'license': 'GPL-3.0-only', 'epoch': epoch, 'files': records,
                'third_party_notices': ['README.md', 'scripts/readers/pic/decode.js',
                                        'scripts/readers/pic2/decode.js']}
    manifest_path = output / 'SOURCE_MANIFEST.json'
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    checksums = []
    for path in (archive_path, manifest_path):
        checksums.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n')
    (output / 'SHA256SUMS').write_text(''.join(checksums), encoding='ascii')
    print(f'Source release: {archive_path} ({len(records)} files)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='new directory; must not exist')
    parser.add_argument('--epoch', type=int, default=int(os.environ.get('SOURCE_DATE_EPOCH', '0')))
    args = parser.parse_args()
    if args.epoch < 0 or args.epoch > 0xffffffff:
        parser.error('epoch must be 0..4294967295')
    prepare(Path(__file__).resolve().parents[1], args.output, args.epoch)


if __name__ == '__main__':
    main()
