// SPDX-License-Identifier: GPL-3.0-only
// Return a bounded plan. Native pixel operations execute in the isolated worker.
function filter(image, context) {
    const s=context.settings, operations=[];
    if(s.grayscale)operations.push({op:'grayscale'});
    operations.push({op:'gamma',value:s.gamma},{op:'contrast',value:s.contrast});
    return operations;
}
