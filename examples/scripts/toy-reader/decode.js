// SPDX-License-Identifier: GPL-3.0-only
function decode(input, context) {
    const b = new Uint8Array(input);
    if (b.length !== 6 || b[0] !== 84 || b[1] !== 79 || b[2] !== 89)
        throw new Error("expected TOY and three RGB bytes");
    const rgba = new Uint8Array([b[3], b[4], b[5], 255]);
    return {width: 1, height: 1, pixels: rgba.buffer};
}
