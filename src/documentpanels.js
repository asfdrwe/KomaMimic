// SPDX-License-Identifier: GPL-3.0-only
// Geometry shared by the trusted renderer and its engine-independent tests.
window.komaPanels = (() => {
    const width = 1200, height = 1600, gap = 24;
    function plan(anchor, count, pageLayout, rightToLeft, wide = [], sides = [], solo = []) {
        if (anchor < 0 || anchor >= count) return [];
        const columns = pageLayout === 1 ? 2 : 1;
        if (wide[anchor]) return [{page: anchor, slot: 0, wide: true}];
        const firstSlot=rightToLeft ? columns-1 : 0;
        const slot=sides[anchor] ? (sides[anchor]===-1 ? 0 : columns-1) : firstSlot;
        if(columns===2 && (solo[anchor] || slot!==firstSlot)) return [{page:anchor,slot,solo:!!solo[anchor]}];
        const result = [];
        for (let n = 0; n < columns && anchor + n < count; ++n) {
            if (n && (wide[anchor+n] || solo[anchor+n] || (sides[anchor+n] && (sides[anchor+n]===-1 ? 0 : 1)===firstSlot))) break;
            result.push({page: anchor + n, slot: rightToLeft ? columns - 1 - n : n});
        }
        return result;
    }
    function geometry(viewWidth, viewHeight, pageLayout, slot, wide = false) {
        const columns = pageLayout === 1 || wide ? 2 : 1;
        const total = width * columns + gap * (columns - 1);
        const scale = Math.max(.001, Math.min(viewWidth / total, viewHeight / height));
        return {scale, left: Math.max(0, (viewWidth - total * scale) / 2) + (wide ? 0 : slot) * (width + gap) * scale,
                top: Math.max(0, (viewHeight - height * scale) / 2),
                width: (wide ? total : width) * scale, height: height * scale};
    }
    // Fit the authored viewport into its page slot without changing the CSS
    // viewport used to lay out the publication (notably vertical writing).
    function contentGeometry(panel, contentWidth, contentHeight) {
        const scale = Math.min(panel.width / contentWidth, panel.height / contentHeight);
        const width = contentWidth * scale, height = contentHeight * scale;
        return {scale, left: panel.left + (panel.width - width) / 2,
                top: panel.top + (panel.height - height) / 2, width, height};
    }
    return {plan, geometry, contentGeometry};
})();
