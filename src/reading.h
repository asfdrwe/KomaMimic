// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QVector>
enum class BoundaryMode { Stop, RepeatSource, AdvanceSource };
struct ReadingOptions {
    bool spread = true;
    bool rightToLeft = true;
    bool coverAlone = true;
    bool leadingBlank = false;
    BoundaryMode boundaryMode = BoundaryMode::Stop;
};
QVector<int> visiblePages(int count, int anchor, const ReadingOptions &options);
int nextAnchor(int count, int anchor, const ReadingOptions &options);
int previousAnchor(int count, int anchor, const ReadingOptions &options);
