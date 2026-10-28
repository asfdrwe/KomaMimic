// SPDX-License-Identifier: GPL-3.0-only AND 0BSD
// Yanagisawa PIC reader. Header, cache and chain decoding adapted from:
// https://github.com/kaleicon/wuimg/blob/trunk/src/lib/pic.c
// Original portion: SPDX-License-Identifier: 0BSD
// SPDX-FileCopyrightText: 2022 kaleido
// New integration and bounds checking: Copyright 2026 KomaMimic contributors.
//
// Copyright (C) 2018 by kaleido <https://codeberg.org/kaleido>
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted.
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
// OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
// CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

function decode(input, context) {
    const bytes = new Uint8Array(input), view = new DataView(input);
    function fail(message) { throw new Error("PIC: " + message); }
    function range(at, count) {
        if (!Number.isSafeInteger(at) || at < 0 || count < 0 || at + count > bytes.length)
            fail("truncated or invalid offset");
    }
    function u16(at) { range(at, 2); return view.getUint16(at, false); }
    range(0, 3);
    if (bytes[0] !== 80 || bytes[1] !== 73 || bytes[2] !== 67) fail("not a Yanagisawa PIC image");
    let header = 3;
    while (header < bytes.length && bytes[header] !== 0x1a) ++header;
    if (header === bytes.length) fail("missing comment terminator");
    ++header;
    while (header < bytes.length && bytes[header] !== 0) ++header;
    ++header; // The dummy terminator and reserved header byte are distinct.
    range(header, 8);
    if (bytes[header] !== 0) fail("invalid reserved header byte");
    const type = bytes[header+1] & 15, mode = bytes[header+1] >>> 4;
    const depth = u16(header+2), width = u16(header+4), height = u16(header+6);
    const count = width * height;
    if (!width || !height || count > context.maxPixels) fail("invalid or oversized dimensions");
    let start = header+8, paletteBits = 0, shared = false;
    function extra() {
        range(start, 6);
        if (![0, 1, 15].includes(mode)) fail("unsupported mode with extra header");
        if (mode === 15 && (!bytes[start+4] || !bytes[start+5])) fail("missing explicit pixel aspect");
        // X/Y placement and pixel aspect are metadata, not stored raster size.
        start += 6;
    }
    switch (type) {
    case 0: // X68000: GRB, optional common intensity bit.
        if (mode !== 0 || ![4,8,15,16].includes(depth)) fail("unsupported X68000 mode/depth");
        shared = depth !== 15;
        if (depth <= 8) paletteBits = 5;
        break;
    case 1: // PC-88VA.
        if (mode & 2) fail("PC-88VA tiled mode is not supported");
        if (mode > 1 || ![8,12,16].includes(depth)) fail("unsupported PC-88VA mode/depth");
        if (!((width === 320 && [200,400,408].includes(height))
            || (width === 640 && [200,204,400].includes(height)))) fail("invalid PC-88VA screen dimensions");
        break;
    case 2: // FM-TOWNS: observed mode 0 has generic-style extra fields.
        if (depth !== 15) fail("unsupported FM-TOWNS depth");
        if (mode === 0) extra();
        else if (mode !== 5 && mode !== 12) fail("unsupported FM-TOWNS mode");
        break;
    case 3: // Mac: RGB555 without the indexed color cache.
        if (depth !== 15 || mode !== 0) fail("unsupported Mac mode/depth");
        break;
    case 15: // Generic: GRB, explicit palette precision for indexed images.
        extra();
        if (![4,8,12,15,16,24].includes(depth)) fail("unsupported generic depth");
        shared = depth === 16;
        if (depth <= 8) {
            range(start, 1); paletteBits = bytes[start++];
            if (paletteBits < 1 || paletteBits > 8) fail("invalid palette precision");
        }
        break;
    default: fail("unsupported machine type");
    }
    let at = start*8;
    function read(n) {
        if (n < 0 || n > 26 || at+n > bytes.length*8) fail("compressed stream exhausted");
        let value = 0;
        for (let i = 0; i < n; ++i, ++at) value = value*2 + ((bytes[at >>> 3] >>> (7-(at & 7))) & 1);
        return value;
    }
    function expand(value, bits) {
        let result = 0, filled = 0;
        while (filled < 8) {
            const n = Math.min(bits, 8-filled);
            result = (result << n) | (value >>> (bits-n)); filled += n;
        }
        return result;
    }
    function rgb(r,g,b) { return (r << 16) | (g << 8) | b; }
    function grb(value, bits, intensity) {
        const mask = (1 << bits)-1, shift = intensity ? 1 : 0, common = intensity ? value & 1 : 0;
        const g = (((value >>> (bits*2+shift)) & mask) << shift) | common;
        const r = (((value >>> (bits+shift)) & mask) << shift) | common;
        const b = (((value >>> shift) & mask) << shift) | common;
        return rgb(expand(r,bits+shift), expand(g,bits+shift), expand(b,bits+shift));
    }
    let palette;
    if (paletteBits) {
        palette = new Uint32Array(1 << depth);
        for (let i = 0; i < palette.length; ++i)
            palette[i] = grb(read(paletteBits*3+(shared ? 1 : 0)), paletteBits, shared);
        at = Math.ceil(at/8)*8;
    }
    if (at >= bytes.length*8) fail("missing compressed data");
    function literal() {
        const value = read(depth);
        if (palette) return palette[value];
        if (type === 3) return rgb(expand(value >>> 10,5), expand((value >>> 5)&31,5), expand(value&31,5));
        if (type === 1) {
            if (depth === 8) return rgb(expand((value >>> 2)&7,3), expand(value >>> 5,3), expand(value&3,2));
            if (depth === 16) return rgb(expand((value >>> 5)&31,5), expand(value >>> 10,6), expand(value&31,5));
        }
        return grb(value, depth === 16 ? 5 : depth/3, shared);
    }
    const cached = depth > 8 && type !== 3;
    const colors = new Uint32Array(128), newer = new Uint8Array(128), older = new Uint8Array(128);
    for (let i = 0; i < 128; ++i) { newer[i] = (i+1)&127; older[i] = (i-1)&127; }
    let first = 0;
    function color() {
        if (!cached) return literal();
        if (!read(1)) {
            const value = literal(); first = newer[first]; colors[first] = value; return value;
        }
        const index = read(7);
        if (index !== first) {
            older[newer[index]] = older[index]; newer[older[index]] = newer[index];
            newer[index] = newer[first]; older[index] = first;
            older[newer[first]] = index; newer[first] = index; first = index;
        }
        return colors[index];
    }
    function length() {
        let bits = 1;
        while (read(1)) if (++bits > 26) fail("run length overflow");
        return Math.pow(2,bits)-1+read(bits);
    }
    // Alpha serves as the change-point mask; all stored colors are opaque.
    const points = new Uint32Array(count);
    let position = -1, chainWork = 0;
    while (position < count-1) {
        position += length();
        if (position >= count) break; // Final run extends past the raster.
        const value = (color() | 0xff000000) >>> 0;
        points[position] = value;
        if (read(1)) {
            let target = position;
            for (;;) {
                const direction = read(2);
                let dx;
                if (direction === 0) {
                    if (!read(1)) break;
                    dx = read(1) ? 2 : -2;
                } else dx = direction-2;
                target += width+dx;
                // Historical encoders can end a chain at the raster boundary.
                // Linear positions intentionally permit crossing a row edge.
                if (target < 0 || target >= count) break;
                if (++chainWork > Math.min(count*8, context.maxPixels*4)) fail("chain work limit exceeded");
                points[target] = value;
            }
        }
    }
    const pixels = new Uint8Array(count*4);
    let current = 0xff000000;
    for (let i = 0; i < count; ++i) {
        if (points[i]) current = points[i];
        pixels[i*4] = (current >>> 16)&255; pixels[i*4+1] = (current >>> 8)&255;
        pixels[i*4+2] = current&255; pixels[i*4+3] = 255;
    }
    return {width: width, height: height, pixels: pixels.buffer};
}
