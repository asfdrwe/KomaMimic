#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Render the authored SVG masters. Optional development dependencies: CairoSVG, Pillow."""
import argparse
from io import BytesIO
from pathlib import Path
import struct

SIZES = (16, 24, 32, 48, 64, 128, 256, 512, 1024)


def generate(directory):
    import cairosvg
    from PIL import Image

    rendered = {}
    images = {}
    for size in SIZES:
        # The wordmark is legible at 128 px and above. Small icons use the symbol.
        name = 'komamimic-symbol.svg' if size < 128 else 'komamimic.svg'
        data = cairosvg.svg2png(url=str(directory / name), output_width=size * 2,
                               output_height=size * 2)
        with Image.open(BytesIO(data)) as image:
            images[size] = image.convert('RGBA').resize((size, size), Image.Resampling.LANCZOS)
        stream = BytesIO()
        images[size].save(stream, format='PNG', optimize=False)
        rendered[f'komamimic-{size}.png'] = stream.getvalue()
    # Pass each actual image so Pillow does not rescale the lettered master at 16 px.
    stream = BytesIO()
    images[256].save(stream, format='ICO', sizes=[(n, n) for n in SIZES if n <= 256],
                     append_images=[images[n] for n in SIZES if n < 256])
    rendered['komamimic.ico'] = stream.getvalue()
    # Include both ordinary and Retina entries, including 16 and 32 px at 1x.
    # Reuse the rendered PNGs so small entries keep the unlettered symbol.
    entries = ((b'icp4', 16), (b'icp5', 32), (b'icp6', 64),
               (b'ic07', 128), (b'ic08', 256), (b'ic09', 512), (b'ic10', 1024),
               (b'ic11', 32), (b'ic12', 64), (b'ic13', 256), (b'ic14', 512))
    chunks, toc = [], []
    for kind, size in entries:
        payload = rendered[f'komamimic-{size}.png']
        header = kind + struct.pack('>I', len(payload) + 8)
        chunks.append(header + payload)
        toc.append(header)
    table = b''.join(toc)
    body = b'TOC ' + struct.pack('>I', len(table) + 8) + table + b''.join(chunks)
    rendered['komamimic.icns'] = b'icns' + struct.pack('>I', len(body) + 8) + body
    return rendered


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='compare to checked-in files without changing them')
    args = parser.parse_args()
    directory = Path(__file__).resolve().parents[1] / 'assets/icons'
    for name, data in generate(directory).items():
        path = directory / name
        if args.check:
            if not path.is_file() or path.read_bytes() != data:
                raise SystemExit(f'Icon differs: {name}; regenerate with the documented dependency versions')
        else:
            path.write_bytes(data)
    print('Icons verified' if args.check else 'Icons generated; review and update tools/public_source.json asset hashes')


if __name__ == '__main__':
    main()
