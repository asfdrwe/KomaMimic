#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Create synthetic MAG/PIC/PIC2 images with reference PNGs (Python stdlib only)."""
import argparse
from functools import lru_cache
import json
from pathlib import Path
import struct
import subprocess
import zipfile
import zlib

WIDTH, HEIGHT = 128, 96
ROOT = Path(__file__).resolve().parents[2]


class Bits:
    def __init__(self):
        self.data = bytearray()
        self.count = 0

    def add(self, value, count=1):
        for bit in range(count - 1, -1, -1):
            if self.count % 8 == 0:
                self.data.append(0)
            self.data[-1] |= ((value >> bit) & 1) << (7 - self.count % 8)
            self.count += 1

    def gap(self, value):
        """Old PIC's positive length: n-1 ones, zero, n payload bits."""
        n = 1
        while value >= (1 << (n + 1)) - 1:
            n += 1
        self.add((1 << (n - 1)) - 1, n - 1)
        self.add(0)
        self.add(value - ((1 << n) - 1), n)


def indices(colors):
    return [colors - 1 if x in (0, WIDTH - 1) or y in (0, HEIGHT - 1)
            else (x // 8 + 2 * (y // 8)) % 16 if colors == 16
            else (x // 4 + (y // 8) * 32) % 256
            for y in range(HEIGHT) for x in range(WIDTH)]


def palette(colors):
    if colors == 16:
        return [tuple(85 * (i >> 3) + 170 * ((i >> bit) & 1) for bit in (2, 1, 0))
                for i in range(16)]
    return [(255 * (i >> 5) // 7, 255 * ((i >> 2) & 7) // 7, 85 * (i & 3))
            for i in range(256)]


def rgba(pixels):
    return bytes(channel for color in pixels for channel in (*color, 255))


def png(pixels):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + pixels[y * WIDTH * 4:(y + 1) * WIDTH * 4] for y in range(HEIGHT))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', WIDTH, HEIGHT, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def mag(colors):
    indexes, table = indices(colors), palette(colors)
    packed = (bytes(indexes) if colors == 256 else
              bytes((indexes[i] << 4) | indexes[i + 1] for i in range(0, len(indexes), 2)))
    stride = WIDTH if colors == 256 else WIDTH // 2
    actions = bytearray(stride // 4)
    flag_a, flag_b, literals = Bits(), bytearray(), bytearray()
    for y in range(HEIGHT):
        for x in range(stride // 4):
            at = y * stride + x * 4
            # Copy a pair of words from the preceding row when possible.
            action = 0x44 if y and packed[at:at + 4] == packed[at - stride:at - stride + 4] else 0
            change = actions[x] ^ action
            flag_a.add(bool(change))
            if change:
                flag_b.append(change)
            actions[x] = action
            if not action:
                literals += packed[at:at + 4]
    header = bytearray(32)
    header[3] = 128 if colors == 256 else 0
    struct.pack_into('<HHHH', header, 4, 0, 0, WIDTH - 1, HEIGHT - 1)
    a = 32 + 3 * colors
    b = a + len(flag_a.data)
    p = b + len(flag_b)
    struct.pack_into('<IIIII', header, 12, a, b, len(flag_b), p, len(literals))
    grb = bytes(c for r, g, b in table for c in (g, r, b))
    image = b'MAKI02  PC98 KomaMimic synthetic sample\x1a' + header + grb + flag_a.data + flag_b + literals
    return image, rgba([table[i] for i in indexes])


def channels(depth):
    values = [palette(16)[i] for i in indices(16)]
    if depth == 15:
        return [tuple(c * 31 // 255 for c in color) for color in values]
    return values


def fast(values, bits):
    stream = Bits()
    stream.add(0)  # Initial length zero: a color at the first pixel.
    for r, g, b in values:
        stream.add(0)  # Literal, GRB channel order.
        stream.add((g << (bits * 2)) | (r << bits) | b, bits * 3)
        stream.add(0)  # No downward chain.
        stream.add(0)  # Length zero: next pixel has its own color.
    return bytes(stream.data)


@lru_cache(maxsize=None)
def residuals(predictor, maximum):
    result = {}
    for n in range(maximum + 1):
        if predictor > maximum // 2 and n > 2 * (maximum - predictor):
            value = maximum - n
        elif predictor <= maximum // 2 and n > 2 * predictor:
            value = n
        else:
            value = predictor + (n // 2 + 1 if n & 1 else -n // 2)
        result[value] = n
    return result


def arithmetic(values, bits):
    # An encoder with an unbounded low accumulator, independent of JS's
    # bounded decoder. Always emit a change/literal and no chains or cache hits.
    probabilities = [12000 + (i * 733) % 44000 for i in range(128)]
    low, size, shifts = 0, 65535, 0

    def decision(context, value):
        nonlocal low, size, shifts
        split = max(1, ((size >> 8) * probabilities[context]) // 256)
        if value:
            low += split
            size -= split
        else:
            size = split
        while size < 32768:
            low *= 2
            size *= 2
            shifts += 1

    def number(context, value):
        for i in range(8):
            if value < (1 << (i + 1)) - 1:
                decision(context + i, 1)
                tail = value - ((1 << i) - 1)
                for k in range(i):
                    decision(context + 8 + k, (tail >> k) & 1)
                return
            decision(context + i, 0)

    maximum = (1 << bits) - 1
    left = (0, 0, 0)
    for at, color in enumerate(values):
        x, y = at % WIDTH, at // WIDTH
        neighbors = 1 + (x > 0) + (x + 1 < WIDTH)
        decision(min(x, 2) + min(y, 2) * neighbors, 1)
        decision(16, 1)  # Always a cache miss.
        up = values[at - WIDTH] if y else (0, 0, 0)
        r, g, b = ((a + c) // 2 for a, c in zip(up, left))
        nr, ng, nb = color
        number(32, residuals(g, maximum)[ng])
        number(48, residuals(max(0, min(maximum, r + ng - g)), maximum)[nr])
        number(64, residuals(max(0, min(maximum, b + ng - g)), maximum)[nb])
        if y + 1 < HEIGHT:
            decision(80, 1)  # No chain.
        left = color
    length = shifts + 16
    midpoint = (low + size // 2) << ((-length) % 8)
    return struct.pack('>128H', *probabilities) + midpoint.to_bytes((length + 7) // 8, 'big')


def pic2(kind, depth):
    values, bits = channels(depth), depth // 3
    if kind == 'P2SS':
        payload = arithmetic(values, bits)
    elif kind == 'P2SF':
        payload = fast(values, bits)
    elif depth == 24:
        payload = bytes(c for color in values for c in color)
    else:
        endian = '>' if kind == 'P2BM' else '<'
        payload = b''.join(struct.pack(endian + 'H', (g << 11) | (r << 6) | (b << 1)) for r, g, b in values)
    header = bytearray(125)
    header[:4] = b'P2DT'
    for p in (30, 62, 94):
        header[p:p + 2] = b'\r\n'
    struct.pack_into('>H', header, 96, 0x1a00)
    struct.pack_into('>I', header, 106, len(header))
    struct.pack_into('>HHHHH', header, 110, depth, 1, 1, WIDTH, HEIGHT)
    block = struct.pack('>4sIHHHHHII', kind.encode(), 26 + len(payload), 0, WIDTH, HEIGHT, 0, 0, 0, 0)
    expanded = values if bits == 8 else [tuple((c << 3) | (c >> 2) for c in color) for color in values]
    return header + block + payload + bytes(8), rgba(expanded)


def old_pic(depth):
    values, bits = channels(depth), depth // 3
    stream = Bits()
    for r, g, b in values:
        stream.gap(1)
        stream.add(0)  # Literal color; no cache hit.
        stream.add((g << (bits * 2)) | (r << bits) | b, depth)
        stream.add(0)  # No chain.
    stream.gap(1)  # Final run past the last pixel.
    header = struct.pack('>BBHHH', 0, 0 if depth == 15 else 15, depth, WIDTH, HEIGHT)
    if depth != 15:
        header += bytes(6)  # Generic model: offsets/aspect unspecified.
    expanded = values if bits == 8 else [tuple((c << 3) | (c >> 2) for c in color) for color in values]
    return b'PIC KomaMimic synthetic sample\x1a\0' + header + stream.data, rgba(expanded)


def verify(worker, reader, encoded, expected):
    source = (ROOT / 'scripts/readers' / reader / 'decode.js').read_text(encoding='utf-8')
    header = json.dumps(dict(apiVersion=1, source=source, name=reader + '-sample.js', timeoutMs=10000)).encode()
    request = struct.pack('>II', len(header), len(encoded)) + header + encoded
    result = subprocess.run([str(worker)], input=request, capture_output=True, timeout=15, check=True)
    if len(result.stdout) < 8:
        raise ValueError('Worker returned no frame: ' + result.stderr.decode(errors='replace'))
    h, p = struct.unpack('>II', result.stdout[:8])
    metadata = json.loads(result.stdout[8:8 + h])
    if (metadata != dict(width=WIDTH, height=HEIGHT) or p != len(expected)
            or result.stdout[8 + h:] != expected):
        raise ValueError('Decoded sample differs from reference: ' + str(metadata))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'tests/fixtures/generated/script-readers')
    parser.add_argument('--verify-worker', type=Path, help='Optional built komamimic-script-worker executable')
    args = parser.parse_args()
    output = args.output.resolve()
    archive = output.with_suffix('.zip')
    if output.exists() or archive.exists():
        parser.error('Output directory or ZIP already exists; choose a new --output')
    cases = [('01-mag16.mag', 'mag', mag(16)), ('02-mag256.mag', 'mag', mag(256))]
    for depth in (15, 24):
        for kind in ('P2BM', 'P2BI', 'P2SF', 'P2SS'):
            cases.append((f'{len(cases)+1:02}-pic2-{depth}-{kind}.p2', 'pic2', pic2(kind, depth)))
    for depth in (15, 24):
        cases.append((f'{len(cases)+1:02}-oldpic-{depth}.pic', 'pic', old_pic(depth)))
    if args.verify_worker:
        for name, reader, (encoded, expected) in cases:
            verify(args.verify_worker.resolve(), reader, encoded, expected)
            print('Verified every RGBA pixel:', name)
    output.mkdir(parents=True, exist_ok=False)
    with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_DEFLATED) as package:
        for name, _, (encoded, expected) in cases:
            for filename, content in ((name, encoded), (Path(name).stem + '.expected.png', png(expected))):
                (output / filename).write_bytes(content)
                entry = zipfile.ZipInfo(filename, date_time=(1980, 1, 1, 0, 0, 0))
                entry.compress_type = zipfile.ZIP_DEFLATED
                entry.create_system = 3
                entry.external_attr = 0o100644 << 16
                package.writestr(entry, content)
    (output / 'README.txt').write_text(
        'KomaMimic synthetic MAG/PIC/PIC2 samples (GPL-3.0-only)\n'
        'Each image is 128 x 96 pixels: colored tiles with a white border.\n'
        'Compare each .mag/.pic/.p2 with the matching .expected.png.\n'
        '15-bit PIC2 has quantized colors; its reference PNG includes this.\n'
        'The neighboring ZIP contains the same pairs for archive reading.\n'
        'Generated locally; no historic artwork, plugins or writer binaries.\n'
        'Passing these samples does not establish compatibility with historical writers.\n', encoding='utf-8')
    print(f'Created {len(cases)} samples + reference PNGs: {output}')
    print(f'Archive reading sample: {archive}')


if __name__ == '__main__':
    main()
