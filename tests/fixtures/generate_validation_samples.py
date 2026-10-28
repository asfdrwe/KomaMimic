#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate portable, original manual-test inputs using only Python's stdlib."""
import argparse
import binascii
import hashlib
import gzip
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import zipfile
import zlib


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', binascii.crc32(kind + data))


def pixels(number, width=480, height=720):
    """Numbered pages, colored quadrants, white border; no external font/art."""
    colors = [(224, 80, 80), (60, 160, 220), (80, 190, 100), (230, 180, 50)]
    segments = ['abcedf', 'bc', 'abged', 'abgcd', 'fgbc', 'afgcd', 'afgecd', 'abc', 'abcdefg', 'abfgcd']
    boxes = {'a': (40, 20, 140, 35), 'b': (125, 20, 140, 110),
             'c': (125, 110, 140, 200), 'd': (40, 185, 140, 200),
             'e': (40, 110, 55, 200), 'f': (40, 20, 55, 110), 'g': (40, 100, 140, 115)}
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            color = colors[(x >= width // 2) + 2 * (y >= height // 2)]
            if min(x, y, width - 1 - x, height - 1 - y) < 12:
                color = (255, 255, 255)
            # The large black digit remains visible at thumbnail size.
            sx, sy = x * 180 // width, y * 220 // height
            if any(boxes[s][0] <= sx < boxes[s][2] and boxes[s][1] <= sy < boxes[s][3]
                   for s in segments[number % 10]):
                color = (0, 0, 0)
            row.extend(color)
        rows.append(bytes(row))
    return b''.join(rows)


def png(raw, width, height):
    rows = b''.join(b'\0' + raw[y * width * 3:(y + 1) * width * 3] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def apng():
    w, h = 128, 96
    result = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    result += chunk(b'acTL', struct.pack('>II', 2, 0))
    sequence = 0
    for index, color in enumerate((b'\xff\0\0', b'\0\0\xff')):
        result += chunk(b'fcTL', struct.pack('>IIIIIHHBB', sequence, w, h, 0, 0, 1, 2, 0, 0))
        sequence += 1
        data = zlib.compress((b'\0' + color * w) * h)
        result += chunk(b'IDAT', data) if not index else chunk(b'fdAT', struct.pack('>I', sequence) + data)
        if index:
            sequence += 1
    return result + chunk(b'IEND', b'')


def gif():
    w, h = 128, 96
    result = b'GIF89a' + struct.pack('<HHBBB', w, h, 0x80, 0, 0) + b'\xff\0\0\0\0\xff'
    result += b'\x21\xff\x0bNETSCAPE2.0\x03\x01\0\0\0'
    for color in (0, 1):
        codes = [item for _ in range(w * h) for item in (4, color)] + [5]
        packed = sum(code << (3 * i) for i, code in enumerate(codes)).to_bytes((len(codes) * 3 + 7) // 8, 'little')
        result += b'\x21\xf9\x04\x00\x32\x00\x00\x00'
        result += b'\x2c' + struct.pack('<HHHHB', 0, 0, w, h, 0) + b'\x02'
        result += b''.join(bytes([len(packed[i:i + 255])]) + packed[i:i + 255] for i in range(0, len(packed), 255)) + b'\0'
    return result + b'\x3b'


def tiff():
    # Two uncompressed grayscale pages: black, then white.
    size = 2 + 9 * 12 + 4
    result = b'II' + struct.pack('<HI', 42, 8)
    for index in range(2):
        result += struct.pack('<H', 9)
        for tag, kind, value in ((256, 4, 64), (257, 4, 64), (258, 3, 8), (259, 3, 1),
                                 (262, 3, 1), (273, 4, 8 + size * 2 + index * 4096),
                                 (277, 3, 1), (278, 4, 64), (279, 4, 4096)):
            result += struct.pack('<HHII', tag, kind, 1, value)
        result += struct.pack('<I', 8 + size if index == 0 else 0)
    return result + b'\0' * 4096 + b'\xff' * 4096


def pdf():
    objects = [b'<< /Type /Catalog /Pages 2 0 R >>',
               b'<< /Type /Pages /Kids [4 0 R 7 0 R 10 0 R] /Count 3 >>',
               b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>']
    for number in range(1, 4):
        page = 4 + (number - 1) * 3
        objects.append(f'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 480 720] /Resources << /Font << /F1 3 0 R >> /XObject << /Im {page+2} 0 R >> >> /Contents {page+1} 0 R >>'.encode())
        stream = f'q 480 0 0 720 0 0 cm /Im Do Q\nBT /F1 28 Tf 30 670 Td (PAGE {number}) Tj ET\n'.encode()
        objects.append(f'<< /Length {len(stream)} >>\nstream\n'.encode() + stream + b'endstream')
        image = zlib.compress(pixels(number, 128, 192))
        objects.append(f'<< /Type /XObject /Subtype /Image /Width 128 /Height 192 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /Length {len(image)} >>\nstream\n'.encode() + image + b'\nendstream')
    result = b'%PDF-1.4\n%\xe2\xe3\xcf\xd3\n'
    offsets = [0]
    for index, obj in enumerate(objects, 1):
        offsets.append(len(result))
        result += f'{index} 0 obj\n'.encode() + obj + b'\nendobj\n'
    start = len(result)
    result += f'xref\n0 {len(offsets)}\n0000000000 65535 f \n'.encode()
    result += b''.join(f'{offset:010d} 00000 n \n'.encode() for offset in offsets[1:])
    return result + f'trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\nstartxref\n{start}\n%%EOF\n'.encode()


def archive(path, entries):
    with zipfile.ZipFile(path, 'x') as stream:
        for name, data in entries:
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_STORED if name == 'mimetype' else zipfile.ZIP_DEFLATED
            stream.writestr(info, data)


def cp932_zip(image):
    name = '表紙ソ.png'.encode('cp932')  # ソ contains a 0x5c trailing byte.
    crc = binascii.crc32(image)
    local = struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, 0, 0, 0, 33, crc, len(image), len(image), len(name), 0) + name + image
    central = struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 20, 20, 0, 0, 0, 33, crc, len(image), len(image), len(name), 0, 0, 0, 0, 0, 0) + name
    return local + central + struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, 1, 1, len(central), len(local), 0)


def epub(path, image, fixed=False):
    body = ''.join(f'<p id="p{i}">段落{i}：日本語の表示、句読点、読書位置を確認します。<ruby>漫画<rt>まんが</rt></ruby>。ABC 123。</p>' for i in range(1, 121))
    metadata = '<meta property="rendition:layout">pre-paginated</meta>' if fixed else ''
    manifest = '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="cover" href="cover.png" media-type="image/png" properties="cover-image"/>'
    manifest += ''.join(f'<item id="c{i}" href="c{i}.xhtml" media-type="application/xhtml+xml"/>' for i in range(1, 4))
    package = f'''<?xml version="1.0" encoding="UTF-8"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">urn:komamimic:synthetic:{'fixed' if fixed else 'flow'}</dc:identifier><dc:title>自作検証書籍</dc:title><dc:language>ja</dc:language><meta property="dcterms:modified">2026-10-24T00:00:00Z</meta>{metadata}</metadata><manifest>{manifest}</manifest><spine page-progression-direction="rtl">{''.join(f'<itemref idref="c{i}"/>' for i in range(1,4))}</spine></package>'''
    entries = [('mimetype', b'application/epub+zip'), ('META-INF/container.xml', b'<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OPS/book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'), ('OPS/book.opf', package.encode()), ('OPS/cover.png', image)]
    nav = '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>目次</title></head><body><nav epub:type="toc"><ol>' + ''.join(f'<li><a href="c{i}.xhtml">第{i}章</a></li>' for i in range(1, 4)) + '</ol></nav></body></html>'
    entries.append(('OPS/nav.xhtml', nav.encode()))
    for i in range(1, 4):
        content = f'<p id="p20">固定ページ {i}</p><img src="cover.png" width="400" height="600"/>' if fixed else body
        xml = f'<html xmlns="http://www.w3.org/1999/xhtml"><head><title>第{i}章</title><meta name="viewport" content="width=480,height=720"/><style>body{{writing-mode:vertical-rl;}} img{{max-width:100%;}}</style></head><body><h1>第{i}章</h1><a href="c{(i%3)+1}.xhtml#p20">次の章へのリンク</a>{content}</body></html>'
        entries.append((f'OPS/c{i}.xhtml', xml.encode()))
    archive(path, entries)


def generate(output, worker=None):
    output = Path(output).resolve()
    if output.exists():
        raise ValueError('Output exists; choose a new directory')
    output.mkdir(parents=True)
    def write(name, data):
        p = output / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data.encode('utf-8') if isinstance(data, str) else data)
    pages = [(f'{i:02}.png', png(pixels(i), 480, 720)) for i in range(1, 7)]
    for name, data in pages:
        write('images/' + name, data)
    write('images/07-wide.png', png(pixels(7, 960, 480), 960, 480))
    write('animation/gif/two-frames.gif', gif())
    write('animation/apng/two-frames.apng', apng())
    write('animation/tiff/two-pages.tiff', tiff())
    write('日本語 空白/ページ & 01.png', pages[0][1])
    archive(output / '日本語 空白/書庫 & 01.cbz', pages)
    (output / 'books').mkdir()
    archive(output / 'books/01-first.zip', pages)
    archive(output / 'books/02-next.zip', [(name, pages[5 - i][1]) for i, (name, _) in enumerate(pages)])
    write('books/03-cp932.zip', cp932_zip(pages[0][1]))
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w', format=tarfile.USTAR_FORMAT) as package:
        for name, data in pages:
            entry = tarfile.TarInfo(name)
            entry.size, entry.mode, entry.mtime = len(data), 0o644, 0
            package.addfile(entry, io.BytesIO(data))
    write('books/05-pages.tar', buffer.getvalue())
    compressed = io.BytesIO()
    with gzip.GzipFile(fileobj=compressed, mode='wb', filename='', mtime=0) as package:
        package.write(buffer.getvalue())
    write('books/06-pages.tar.gz', compressed.getvalue())
    text = '検証用テキスト\n\n' + '\n\n'.join(f'段落{i}：｜漫画《まんが》を読むための自作データです。日本語 ABC 123。［＃改ページ］' if i % 20 == 0 else f'段落{i}：｜漫画《まんが》と日本語、記号 ABC 123 を確認します。' for i in range(1, 161))
    markdown = '# 見出し1\n\n本文と **強調**、[本文内リンク](#見出し2)。\n\n![自作画像](cover.png)\n\n## 見出し2\n\n' + '\n\n'.join(f'段落{i}：Markdownの文字サイズ・検索・読書再開を確認します。' for i in range(1, 100))
    write('documents/aozora-utf8.txt', text)
    write('documents/aozora-cp932.txt', text.encode('cp932'))
    write('documents/sample.md', markdown)
    write('documents/cover.png', pages[0][1])
    write('documents/three-pages.pdf', pdf())
    epub(output / 'documents/reflow.epub', pages[0][1])
    epub(output / 'documents/fixed.epub', pages[0][1], True)
    archive(output / 'books/04-mixed-documents.zip', [('first.txt', text.encode()), ('second.md', markdown.encode()), ('cover.png', pages[0][1])])
    write('import/synthetic.ini', '; 自作した移行検証専用INI。原作INIのコピーではありません。\n[Config]\nPageMode=1\nPrepageForwardNum=3\nPrepageBackwardNum=1\n[KeyBoardKeyConfig]\nCMD1=0\n[ToolBar]\nTrackBar=0\n[Unknown]\nSyntheticValue=keep-me\n')
    write('import/invalid-values.ini', '[Config]\nPageMode=99\nPrepageForwardNum=-1\n[AutoDisp]\nTwoPage=@height>\n[KeyBoardKeyConfig]\nCMD999=65\n')
    write('broken/bad.png', b'\x89PNG\r\n\x1a\ntruncated')
    write('broken/bad.zip', b'PK\x03\x04truncated')
    write('broken/bad.pdf', b'%PDF-1.4\ntruncated')
    command = [sys.executable, str(Path(__file__).with_name('generate_script_samples.py')), '--output', str(output / 'script-readers')]
    if worker:
        command += ['--verify-worker', str(Path(worker).resolve())]
    subprocess.run(command, check=True)
    records = [{'path': p.relative_to(output).as_posix(), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(output.rglob('*')) if p.is_file()]
    write('FIXTURES.json', json.dumps({'schema': 1, 'license': 'GPL-3.0-only', 'origin': 'original synthetic inputs', 'files': records}, ensure_ascii=False, indent=2) + '\n')
    print(f'Generated {len(records)} files: {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--verify-worker', type=Path)
    args = parser.parse_args()
    generate(args.output, args.verify_worker)
