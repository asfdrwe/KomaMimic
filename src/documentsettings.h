// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "documentsource.h"
#include <QSettings>
#include <QVariantMap>
#include <QSet>
QString documentBookKey(const QString &path);
QString documentFormatKey(const QString &path);
QVariantMap documentOptionDefaults();
QVariantMap documentOptions(const QSettings &settings, const QString &path,
                            bool includeBook = true, bool includeFormat = true);
void saveDocumentOptions(QSettings &settings, const QString &prefix,
                         const QVariantMap &values,
                         const QVariantMap &inherited);
TextOptions textOptions(const QVariantMap &values);
QVector<int> documentVisiblePages(int count, int anchor, int pageLayout,
                                 const QSet<int> &wide = {}, const QHash<int,int> &sides = {},
                                 const QSet<int> &solo = {}, bool rtl = false);
int advanceDocumentPage(int count, int anchor, int direction, int pageLayout,
                        bool onePage = false, const QSet<int> &wide = {}, const QHash<int,int> &sides = {},
                                 const QSet<int> &solo = {}, bool rtl = false);

// Source values win; unrelated destination preferences remain. Call after a successful rename.
void moveDocumentSettings(QSettings &settings, const QString &oldPath, const QString &newPath);
