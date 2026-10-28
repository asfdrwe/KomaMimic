// SPDX-License-Identifier: GPL-3.0-only
function inspect(input, context) {
    const b = new Uint8Array(input);
    if (b.length !== 4 || b[0] !== 84 || b[1] !== 80 || b[2] !== 71 || b[3] !== 50)
        throw new Error("expected TPG2");
    return {pages: 2};
}
function decode(input, context) {
    inspect(input, context);
    const p = context.pageIndex;
    if (p !== 0 && p !== 1) throw new Error("invalid page");
    const rgba = new Uint8Array(p === 0 ? [255,0,0,255] : [0,0,255,255]);
    return {width: 1, height: 1, pixels: rgba.buffer};
}
