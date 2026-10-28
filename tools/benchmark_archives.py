#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate synthetic temporary fixtures and benchmark ZIP, solid 7z, nested ZIP."""
import argparse
import json
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import zipfile
import zlib


def png(data, size=256):
    def chunk(kind, value):
        return struct.pack('>I', len(value)) + kind + value + struct.pack('>I', zlib.crc32(kind + value))
    rows = b''.join(b'\0' + data[y*size*3:(y+1)*size*3] for y in range(size))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows, 1)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    probe = args.probe.resolve()
    with tempfile.TemporaryDirectory(prefix='koma-archive-benchmark-') as directory:
        root = Path(directory)
        random_source = random.Random(900)
        base = random_source.randbytes(256*256*3)
        for i in range(120):
            # Same artwork with small page changes is a useful solid-compression workload.
            data = bytes((x + i) % 256 for x in base)
            (root / f'{i:04}.png').write_bytes(png(data))
        with zipfile.ZipFile(root / 'pages.zip', 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
            for path in sorted(root.glob('*.png')):
                archive.write(path, path.name)
        subprocess.run(['7z', 'a', '-bd', '-t7z', '-ms=on', '-m0=lzma2', '-mx=1', 'solid.7z', '*.png'],
                       cwd=root, check=True, stdout=subprocess.DEVNULL)
        listing = subprocess.check_output(['7z', 'l', '-slt', 'solid.7z'], cwd=root, text=True)
        with zipfile.ZipFile(root / 'nested.zip', 'w', compression=zipfile.ZIP_STORED) as archive:
            archive.write(root / 'pages.zip', 'inside.zip')
        report = {'fixture': '120 RGB PNGs 256x256; generated seed=900',
                  'cache': '32 MiB encoded, fresh Book for each run; OS cache is not flushed',
                  'solid': [line for line in listing.splitlines() if line.startswith(('Solid =', 'Blocks ='))]}
        for name in ('pages.zip', 'solid.7z', 'nested.zip'):
            report[name] = json.loads(subprocess.check_output([str(probe), str(root / name)], text=True))
        args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(args.output)


if __name__ == '__main__':
    main()
