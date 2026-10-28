// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QKeySequence>
#include <QMap>
#include <QVariantMap>
#include <QVector>

enum class LegacySettingsCategory { Reading, Cache, Keyboard, Toolbar };

struct LegacySettingChange {
  QString label;
  QStringList sources;
  QVariantMap values;
  LegacySettingsCategory category = LegacySettingsCategory::Reading;
};
struct LegacySettingsPlan {
  QVector<LegacySettingChange> changes;
  QMap<QString, QString> originals;
  QStringList report;
  QString error;
};
// Does not write settings or execute any imported command/script.
LegacySettingsPlan
planLegacySettings(const QString &text,
                   const QHash<QString, QKeySequence> &bindings,
                   const QHash<QString, QKeySequence> &documentBindings);
