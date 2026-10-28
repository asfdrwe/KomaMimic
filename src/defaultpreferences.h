// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QVariantMap>
class QSettings;
// Independently expressed supported preferences; no legacy INI is distributed.
QVariantMap mangaPreferenceDefaults();
void initializeMangaPreferences(QSettings &settings);
