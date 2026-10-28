// SPDX-License-Identifier: GPL-3.0-only
#include "reading.h"
#include <algorithm>
QVector<int> visiblePages(int count, int anchor, const ReadingOptions &o) {
    if (count <= 0 || anchor < 0 || anchor >= count) return {};
    QVector<int> result{anchor};
    if (o.spread && !((o.coverAlone || o.leadingBlank) && anchor == 0) && anchor + 1 < count)
        result.append(anchor + 1);
    return result;
}
int nextAnchor(int count, int anchor, const ReadingOptions &o) {
    const int next = anchor + visiblePages(count, anchor, o).size();
    return next < count ? next : anchor;
}
int previousAnchor(int count, int anchor, const ReadingOptions &o) {
    if (count <= 0) return 0;
    // After a one-page alignment change, do not skip page 2 at the cover boundary.
    if (o.spread && (o.coverAlone || o.leadingBlank) && anchor > 1) return std::max(1, anchor - 2);
    return std::max(0, anchor - (o.spread ? 2 : 1));
}
