// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QStringList>
#include <QVector>
class QSettings;
namespace I18n {
struct Language { QString code, name; };
QVector<Language> languages();
QString resolve(const QString &selection, const QStringList &preferences);
QString initialize(const QString &selection);
QString currentLanguage();
QString savedLanguage(const QSettings &settings);
QString formatLabel(const QString &stableName);
QString startupConfigPath(const QStringList &arguments);
}
