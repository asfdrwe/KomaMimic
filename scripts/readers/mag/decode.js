// SPDX-License-Identifier: GPL-3.0-only
// MAG (MAKI02), independently implemented from Woody RINN's MAG format.
// Format references and a walkthrough: docs/development_script_readers.md
function decode(input, context) {
    const bytes = new Uint8Array(input);
    const view = new DataView(input);
    function fail(message) { throw new Error("MAG: " + message); }
    function range(offset, size) {
        if (!Number.isSafeInteger(offset) || offset < 0 || size < 0 || offset + size > bytes.length)
            fail("truncated or invalid offset");
    }
    function u16(offset) { range(offset, 2); return view.getUint16(offset, true); }
    function u32(offset) { range(offset, 4); return view.getUint32(offset, true); }
    const signature = "MAKI02  ";
    range(0, 8);
    for (let i = 0; i < 8; ++i) if (bytes[i] !== signature.charCodeAt(i)) fail("not a MAKI02 image");
    let header = 8;
    while (header < bytes.length && bytes[header] !== 0x1a) ++header;
    if (header === bytes.length) fail("missing comment terminator");
    while (header < bytes.length && bytes[header] !== 0) ++header;
    range(header, 32);
    const machine = bytes[header + 1], flags = bytes[header + 2], mode = bytes[header + 3];
    if (machine === 3 && flags !== 0 && flags !== 4 && flags !== 0x54)
        fail("this sample supports MSX palette modes only; screen 8 / YJK is not supported");
    const bpp = machine === 3 ? 4 : (mode & 128 ? 8 : 4);
    const ppb = 8 / bpp;
    const left = u16(header + 4), top = u16(header + 6);
    const right = u16(header + 8), bottom = u16(header + 10);
    const width = right - left + 1, height = bottom - top + 1;
    const repeat = (machine === 3 ? flags === 4 : bpp === 4 && (mode & 1)) ? 2 : 1;
    const paddedLeft = Math.floor(left / ppb) & ~3;
    const stride = ((Math.floor(right / ppb) + 4) & ~3) - paddedLeft;
    if (width < 1 || height < 1 || stride < 4 || stride * ppb * height > context.maxPixels
        || width * height * repeat > context.maxPixels) fail("invalid or oversized dimensions");
    const aStart = header + u32(header + 12), bStart = header + u32(header + 16);
    const pStart = header + u32(header + 24);
    // Stream boundaries are authoritative. Some historical writers stored
    // unreliable length fields at +20 / +28 (see the format notes).
    if (aStart < header + 32 || bStart < aStart || pStart < bStart) fail("overlapping streams");
    range(aStart, 0); range(bStart, 0); range(pStart, 0);
    const paletteCount = Math.min(256, Math.floor((aStart - header - 32) / 3));
    if (paletteCount < (1 << bpp)) fail("incomplete palette");
    let significant = machine === 3 ? 3 : machine === 0x68 ? 5 : machine === 0x99 ? 8 : 4;
    if (paletteCount === 256 && machine !== 3 && machine !== 0x88) significant = 8;
    let deca = "";
    for (let i = 32; i < Math.min(43, header); ++i) deca += String.fromCharCode(bytes[i]);
    if (machine === 3 && deca === "Deca loader") significant = paletteCount === 256 ? 8 : 4;
    function component(value) {
        const high = value >>> (8 - significant);
        // Replicate high bits, rather than preserving unspecified low bits.
        return significant === 8 ? high : significant === 5 ? (high << 3) | (high >>> 2)
            : significant === 4 ? high * 17 : (high << 5) | (high << 2) | (high >>> 1);
    }
    const palette = new Uint8Array(paletteCount * 3);
    for (let i = 0; i < paletteCount; ++i) {
        const p = header + 32 + i * 3;
        palette[i * 3] = component(bytes[p + 1]);
        palette[i * 3 + 1] = component(bytes[p]);
        palette[i * 3 + 2] = component(bytes[p + 2]);
    }
    const packed = new Uint8Array(stride * height), actions = new Uint8Array(stride / 4);
    const dx = [0,1,2,4,0,1,0,1,2,0,1,2,0,1,2,0];
    const dy = [0,0,0,0,1,1,2,2,2,4,4,4,8,8,8,16];
    let aBit = 0, bp = bStart, pixel = pStart;
    for (let y = 0; y < height; ++y) for (let x = 0; x < actions.length; ++x) {
        const ap = aStart + (aBit >>> 3);
        if (ap >= bStart) fail("flag A exhausted");
        if (bytes[ap] & (128 >>> (aBit & 7))) {
            if (bp >= pStart) fail("flag B exhausted");
            actions[x] ^= bytes[bp++];
        }
        ++aBit;
        for (let half = 0; half < 2; ++half) {
            const code = (actions[x] >>> (half ? 0 : 4)) & 15;
            const col = x * 4 + half * 2, target = y * stride + col;
            if (code === 0) {
                range(pixel, 2);
                packed[target] = bytes[pixel++]; packed[target + 1] = bytes[pixel++];
            } else {
                if (y < dy[code] || col < dx[code] * 2) fail("back-reference outside decoded image");
                const from = target - dy[code] * stride - dx[code] * 2;
                packed[target] = packed[from]; packed[target + 1] = packed[from + 1];
            }
        }
    }
    const rgba = new Uint8Array(width * height * repeat * 4);
    const crop = left - paddedLeft * ppb;
    for (let y = 0; y < height * repeat; ++y) for (let x = 0; x < width; ++x) {
        const position = crop + x, value = packed[Math.floor(y / repeat) * stride + Math.floor(position / ppb)];
        const index = bpp === 8 ? value : (value >>> (position & 1 ? 0 : 4)) & 15;
        const dest = (y * width + x) * 4;
        rgba[dest] = palette[index * 3]; rgba[dest + 1] = palette[index * 3 + 1];
        rgba[dest + 2] = palette[index * 3 + 2]; rgba[dest + 3] = 255;
    }
    return {width: width, height: height * repeat, pixels: rgba.buffer};
}
