// SPDX-License-Identifier: GPL-3.0-only AND 0BSD
// PIC2 reader: container, raw and RLE code Copyright 2026 KomaMimic contributors.
// Arithmetic decoder adapted from wuimg src/lib/pic2.c (0BSD):
// Original portion: SPDX-License-Identifier: 0BSD
// SPDX-FileCopyrightText: 2024 kaleido
// https://github.com/kaleicon/wuimg/blob/trunk/src/lib/pic2.c
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
// The notice above applies to the adapted arithmetic portion. See README.md (Third-party libraries and code).

function decode(input, context) {
    const bytes = new Uint8Array(input), view = new DataView(input);
    function fail(message) { throw new Error("PIC2: " + message); }
    function range(offset, size) {
        if (!Number.isSafeInteger(offset) || offset < 0 || size < 0 || offset + size > bytes.length)
            fail("truncated or invalid offset");
    }
    function u16(offset) { range(offset, 2); return view.getUint16(offset, false); }
    function u32(offset) { range(offset, 4); return view.getUint32(offset, false); }
    function tag(offset) {
        range(offset, 4);
        return String.fromCharCode(bytes[offset], bytes[offset+1], bytes[offset+2], bytes[offset+3]);
    }
    range(0, 124);
    if (tag(0) !== "P2DT") fail("not a P2DT image");
    for (const p of [30, 62, 94]) if (u16(p) !== 0x0d0a) fail("invalid text header");
    if (u16(96) !== 0x1a00) fail("missing text terminator");
    if (u16(98) !== 0) fail("paletted PIC2 is not supported");
    const depth = u16(110);
    if (depth !== 15 && depth !== 24) fail("this sample supports 15-bit and 24-bit PIC2 only");
    const bits = depth / 3, mask = (1 << bits) - 1;
    const width = u16(116), height = u16(118);
    if (!width || !height || width * height > context.maxPixels) fail("invalid or oversized canvas");
    let block = u32(106);
    if (block < 125) fail("invalid header length");
    range(block - 1, 1);
    if (bytes[block - 1] !== 0) fail("unterminated comment");
    const rgba = new Uint8Array(width * height * 4);
    function swapRG(color) {
        return ((color >>> bits) & mask) << (bits * 2) | ((color >>> (bits * 2)) & mask) << bits | (color & mask);
    }
    function rawColor(value) { return depth === 15 ? swapRG(value >>> 1) : value; }
    function bitReader(start, end) {
        let at = start * 8;
        return function(count) {
            if (count < 0 || count > 26 || at + count > end * 8) fail("compressed stream exhausted");
            let value = 0;
            for (let i = 0; i < count; ++i, ++at) value = value * 2 + ((bytes[at >>> 3] >>> (7 - (at & 7))) & 1);
            return value;
        };
    }
    function raw(start, end, w, h, little) {
        const size = depth === 15 ? 2 : 3;
        if (start + w * h * size !== end) fail("incorrect raw block length");
        const colors = new Uint32Array(w * h);
        for (let i = 0, p = start; i < colors.length; ++i, p += size) {
            const c = size === 3 ? bytes[p] * 65536 + bytes[p+1] * 256 + bytes[p+2]
                : little ? bytes[p] + bytes[p+1] * 256 : bytes[p] * 256 + bytes[p+1];
            colors[i] = rawColor(c);
        }
        return colors;
    }
    function fast(start, end, w, h) {
        const read = bitReader(start, end), colors = new Uint32Array(w * h);
        const cache = new Uint32Array(256 * 64), positions = new Uint8Array(256);
        let currentFlags = new Uint8Array(w), nextFlags = new Uint8Array(w);
        let currentChain = new Uint32Array(w), nextChain = new Uint32Array(w);
        let color = 0, remaining = 0, chained = 0;
        function length() {
            let ones = 0;
            while (read(1)) if (++ones > 26) fail("RLE length overflow");
            remaining = ones === 0 ? 0 : read(ones) + Math.pow(2, ones) - 1;
            chained = remaining === 1023 ? 1023 : 0;
            if (remaining > 1023) --remaining;
        }
        function nextColor() {
            const key = swapRG(color) >>> (depth - 8);
            if (read(1) === 0) {
                positions[key] = (positions[key] - 1) & 63;
                color = swapRG(read(depth)); cache[key * 64 + positions[key]] = color;
            } else color = cache[key * 64 + ((positions[key] + read(6)) & 63)];
        }
        function chain(x, y) {
            if (!read(1)) return;
            let delta = 0;
            if (!read(1)) { const side = read(1) ? -1 : 1; delta = side * (read(1) ? 1 : 2); }
            // Last-row chains still occupy bits, but have no visible target.
            if (y + 1 === h) return;
            if (x + delta < 0 || x + delta >= w) fail("RLE chain outside image");
            nextFlags[x + delta] = 1; nextChain[x + delta] = color;
        }
        length();
        for (let y = 0; y < h; ++y) {
            for (let x = 0; x < w; ++x) {
                if (currentFlags[x]) {
                    color = currentChain[x]; chain(x, y);
                    if (chained && --chained === 0) length();
                } else if (!chained && --remaining < 0) {
                    nextColor(); chain(x, y); length();
                }
                colors[y * w + x] = color;
            }
            [currentFlags, nextFlags] = [nextFlags, currentFlags]; nextFlags.fill(0);
            [currentChain, nextChain] = [nextChain, currentChain]; nextChain.fill(0);
        }
        return colors;
    }
    // Adapted 0BSD arithmetic decoder begins here. The byte-oriented JS port
    // bounds every read/chain; clamps channel predictors and uses GRB cache keys
    // for 15-bit files to match the historical encoder.
    function arithmetic(start, end, w, h) {
        if (end - start < 258) fail("missing arithmetic probability table");
        const probability = new Uint16Array(128);
        for (let i = 0; i < 128; ++i) {
            probability[i] = u16(start + i * 2);
        }
        const read = bitReader(start + 258, end);
        let upper = 65535, lower = u16(start + 256);
        if (lower >= upper) fail("invalid arithmetic initial value");
        function bit(code) {
            const split = Math.max(1, Math.floor((upper >>> 8) * probability[code] / 256));
            const result = lower >= split ? 1 : 0;
            if (result) { lower -= split; upper -= split; } else upper = split;
            while (upper < 32768) { upper *= 2; lower = lower * 2 + read(1); }
            if (lower >= upper) fail("invalid arithmetic interval");
            return result;
        }
        function number(code) {
            for (let i = 0; i < 8; ++i) if (bit(code + i)) {
                let value = (1 << i) - 1;
                for (let k = 0; k < i; ++k) value += bit(code + 8 + k) * (1 << k);
                return value;
            }
            return 255;
        }
        function channel(code, predictor) {
            const value = number(code);
            let result;
            if (predictor > mask / 2 && value > 2 * (mask - predictor)) result = mask - value;
            else if (predictor <= mask / 2 && value > 2 * predictor) result = value;
            else result = predictor + (value & 1 ? Math.floor(value / 2) + 1 : -value / 2);
            if (result < 0 || result > mask) fail("arithmetic color outside channel range");
            return result;
        }
        const colors = new Uint32Array(w * h), cache = new Uint32Array(512 * 32);
        const positions = new Uint8Array(512);
        // Padding accommodates probability updates at x-1, x+1, x+2.
        let currentFlags = new Uint16Array(w + 4), nextFlags = new Uint16Array(w + 4), next2Flags = new Uint16Array(w + 4);
        let currentChain = new Uint32Array(w), nextChain = new Uint32Array(w);
        let color = 0, cacheCode = 16;
        function nextColor(x, y) {
            const up = y ? colors[(y-1)*w+x] : 0;
            let key = ((up >>> (bits*3-9)) & 0x1c0) | ((up >>> (bits*2-6)) & 0x38) | ((up >>> (bits-3)) & 7);
            if (bits === 5) key = ((key & 0x1c0) >>> 3) | ((key & 0x38) << 3) | (key & 7);
            let target = positions[key];
            if (bit(cacheCode)) {
                cacheCode = 16;
                const r = (((up >>> (bits*2)) & mask) + ((color >>> (bits*2)) & mask)) >>> 1;
                const g = (((up >>> bits) & mask) + ((color >>> bits) & mask)) >>> 1;
                const b = ((up & mask) + (color & mask)) >>> 1;
                const ng = channel(32, g);
                const nr = channel(48, Math.max(0, Math.min(mask, r + ng - g)));
                const nb = channel(64, Math.max(0, Math.min(mask, b + ng - g)));
                color = (nr << (bits*2)) | (ng << bits) | nb;
                target = (target - 1) & 31; positions[key] = target;
            } else {
                cacheCode = 15;
                const value = number(17), a = (target + value) & 31, b = (target + Math.floor(value/2)) & 31;
                color = cache[key*32+a]; cache[key*32+a] = cache[key*32+b]; cache[key*32+b] = cache[key*32+target];
            }
            cache[key*32+target] = color;
        }
        function chain(x, code) {
            if (bit(code)) return;
            let i = 1;
            while (i < 5 && !bit(code+i)) ++i;
            const delta = Math.floor(i/2) * (i & 1 ? 1 : -1);
            if (x+delta < 0 || x+delta >= w) fail("arithmetic chain outside image");
            nextChain[x+delta] = color; nextFlags[x+delta+2] |= (80+6*i) << 8;
        }
        for (let y = 0; y < h; ++y) {
            for (let x = 0; x < w; ++x) {
                const p = x+2;
                let code = currentFlags[p] >>> 8;
                if (code) color = currentChain[x];
                else if (bit(currentFlags[p] & 15)) {
                    ++currentFlags[p+1]; ++currentFlags[p+2];
                    for (let d = -1; d <= 1; ++d) { ++nextFlags[p+d]; ++next2Flags[p+d]; }
                    nextColor(x, y); code = 80;
                }
                if (code && y+1 < h) chain(x, code);
                colors[y*w+x] = color;
            }
            [currentFlags, nextFlags, next2Flags] = [nextFlags, next2Flags, currentFlags]; next2Flags.fill(0);
            [currentChain, nextChain] = [nextChain, currentChain]; nextChain.fill(0);
        }
        return colors;
    }
    // End of the adapted arithmetic portion.
    function expand(value) { return bits === 8 ? value : (value << 3) | (value >>> 2); }
    let work = 0, count = 0, images = 0, terminated = false;
    while (block < bytes.length) {
        range(block, 8);
        const id = tag(block), size = u32(block+4);
        if (id === "\0\0\0\0" && size === 0) { terminated = true; break; }
        if (++count > 4096 || size < 8) fail("invalid block count/length");
        range(block, size);
        const end = block+size;
        if (id === "PDPI") {
            if (size !== 10) fail("invalid PDPI block");
        } else {
            if (!["P2BM", "P2BI", "P2SF", "P2SS"].includes(id)) fail("unsupported block " + id);
            if (size < 26) fail("short image block");
            const flags = u16(block+8), w = u16(block+10), h = u16(block+12);
            const x0 = u16(block+14), y0 = u16(block+16);
            if (flags > 1 || u32(block+22) !== 0) fail("unknown image flags/reserved field");
            work += w*h;
            if (!w || !h || x0+w > width || y0+h > height || work > context.maxPixels)
                fail("image block outside canvas or work limit");
            const transparent = rawColor(u32(block+18));
            const colors = id === "P2SS" ? arithmetic(block+26, end, w, h)
                : id === "P2SF" ? fast(block+26, end, w, h) : raw(block+26, end, w, h, id === "P2BI");
            for (let y = 0; y < h; ++y) for (let x = 0; x < w; ++x) {
                const color = colors[y*w+x];
                if ((flags & 1) && color === transparent) continue;
                const p = ((y0+y)*width+x0+x)*4;
                // Repeat significant bits to fill the eight-bit channel.
                rgba[p] = expand((color >>> (bits*2)) & mask); rgba[p+1] = expand((color >>> bits) & mask);
                rgba[p+2] = expand(color & mask); rgba[p+3] = 255;
            }
            ++images;
        }
        block = end;
    }
    if (!terminated || !images) fail("missing end marker or image block");
    return {width: width, height: height, pixels: rgba.buffer};
}
