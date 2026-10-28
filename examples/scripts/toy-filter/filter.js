// SPDX-License-Identifier: GPL-3.0-only
function filter(image, context) {
    return context.settings.invert ? [{op: 'invert'}] : [];
}
