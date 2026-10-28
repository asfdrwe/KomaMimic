// SPDX-License-Identifier: GPL-3.0-only
#include "legacysettings.h"
#include "expression.h"
#include "readerfeatures.h"
#include "shortcuts.h"
#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <tuple>

LegacySettingsPlan
planLegacySettings(const QString &text,
                   const QHash<QString, QKeySequence> &bindings,
                   const QHash<QString, QKeySequence> &documentBindings) {
  LegacySettingsPlan plan;
  QMap<QString, QString> names;
  QString section;
  bool recognized = false;
  if (text.contains(QChar(0))) {
    plan.error = QCoreApplication::translate(
        "KomaMimic", "設定ファイルに不正な文字が含まれています。");
    return plan;
  }
  for (auto line : text.split('\n')) {
    line = line.trimmed();
    if (line.startsWith(QChar(0xfeff)))
      line.remove(0, 1);
    if (line.isEmpty() || line.startsWith(';') || line.startsWith('#'))
      continue;
    if (line.startsWith('[') && line.endsWith(']')) {
      section = line.mid(1, line.size() - 2).trimmed();
      continue;
    }
    const int equals = line.indexOf('=');
    if (section.isEmpty() || equals <= 0) {
      plan.error = QCoreApplication::translate(
          "KomaMimic", "INIのセクションまたは項目の書式が不正です。");
      return plan;
    }
    const auto key = section + '/' + line.left(equals).trimmed();
    auto value = line.mid(equals + 1).trimmed();
    if (value.size() >= 2 && value.startsWith('"') && value.endsWith('"'))
      value = value.mid(1, value.size() - 2);
    const auto normalized = key.toLower();
    if (names.contains(normalized)) {
      plan.report << QCoreApplication::translate("KomaMimic",
                                                 "重複項目は最後の値を使用：") +
                         key;
      plan.originals.remove(names[normalized]);
    }
    names[normalized] = key;
    plan.originals[key] = value;
    if (QStringList{"config", "autodisp", "toolBar", "keyboardkeyconfig"}
            .contains(section, Qt::CaseInsensitive))
      recognized = true;
  }
  if (!recognized || plan.originals.isEmpty()) {
    plan.error = QCoreApplication::translate(
        "KomaMimic", "MangaMeeyaの設定項目が見つかりません。");
    return plan;
  }
  QSet<QString> handled;
  const auto source = [&](const QString &key) {
    return names.value(key.toLower());
  };
  const auto integer = [&](const QString &key, int low, int high, int &value) {
    const auto name = source(key);
    if (name.isEmpty())
      return false;
    bool ok = false;
    const auto n = plan.originals[name].toLongLong(&ok);
    if (!ok || n < low || n > high) {
      handled.insert(name);
      plan.report << QCoreApplication::translate(
                         "KomaMimic", "保持（値が範囲外または不正）：") +
                         name;
      return false;
    }
    value = int(n);
    return true;
  };
  const auto add = [&](const QString &label, const QStringList &keys,
                       const QVariantMap &values) {
    auto category = LegacySettingsCategory::Reading;
    const auto destination = values.firstKey();
    if (destination.startsWith("keys/"))
      category = LegacySettingsCategory::Keyboard;
    else if (destination.startsWith("cache/"))
      category = LegacySettingsCategory::Cache;
    else if (destination.startsWith("toolbar/"))
      category = LegacySettingsCategory::Toolbar;
    plan.changes.append({label, keys, values, category});
    for (const auto &key : keys)
      handled.insert(key);
  };
  for (const auto &[oldKey, newKey, label, high] :
       QVector<std::tuple<QString, QString, QString, int>>{
           {"Config/PrepageForwardNum", "cache/ahead",
            QCoreApplication::translate("KomaMimic",
                                        "前方向の先読み（表示単位）"),
            20},
           {"Config/PrepageBackwardNum", "cache/behind",
            QCoreApplication::translate("KomaMimic",
                                        "逆方向の先読み（表示単位）"),
            20},
           {"AutoDisp/HalfPageVAllowance", "reading/splitVOverlap",
            QCoreApplication::translate("KomaMimic", "左右分割の重複幅（px）"),
            4096},
           {"AutoDisp/HalfPageHAllowance", "reading/splitHOverlap",
            QCoreApplication::translate("KomaMimic", "上下分割の重複幅（px）"),
            4096}}) {
    int n;
    if (integer(oldKey, 0, high, n))
      add(label, {source(oldKey)}, {{newKey, n}});
  }
  for (const auto &[oldKey, newKey, label] :
       QVector<std::tuple<QString, QString, QString>>{
           {"Config/AutoUpdate", "General/watch",
            QCoreApplication::translate("KomaMimic",
                                        "現在のファイルの更新を監視")},
           {"Config/AutoPageChange", "reading/scrollTurnsPage",
            QCoreApplication::translate(
                "KomaMimic", "スクロールで端まで読んだらページをめくる")},
           {"ToolBar/TrackBar", "toolbar/showPageBar",
            QCoreApplication::translate("KomaMimic",
                                        "中央にページバーを表示")}}) {
    int n;
    if (integer(oldKey, 0, 1, n))
      add(label, {source(oldKey)}, {{newKey, n == 1}});
  }
  int n;
  if (integer("Config/SubFolder", 1, 2, n))
    add(QCoreApplication::translate("KomaMimic", "サブフォルダの画像も読む"),
        {source("Config/SubFolder")}, {{"source/recursive", n == 1}});
  if (integer("Config/PageMode", 0, 2, n))
    add(QCoreApplication::translate("KomaMimic",
                                    "単ページ・見開き・自動見開き"),
        {source("Config/PageMode")},
        {{"reading/spread", n != 1}, {"reading/autoSpread", n == 0}});
  for (const auto &[oldKey, newKey] : QVector<QPair<QString, QString>>{
           {"AutoDisp/TwoPage", "reading/spreadCondition"},
           {"AutoDisp/HalfPageV", "reading/splitVCondition"},
           {"AutoDisp/HalfPageH", "reading/splitHCondition"},
           {"AutoDisp/HalfPageVPos", "reading/splitVPosition"},
           {"AutoDisp/HalfPageHPos", "reading/splitHPosition"}}) {
    const auto key = source(oldKey);
    if (key.isEmpty())
      continue;
    const auto value = plan.originals[key];
    const auto result = evaluateExpression(
        value,
        {{"width", 1200}, {"height", 1600}, {"page", 1}, {"totalpage", 100}});
    if (!result.error.isEmpty() || value.isEmpty()) {
      handled.insert(key);
      plan.report << QCoreApplication::translate(
                         "KomaMimic", "保持（未対応または不正な式）：") +
                         key;
    } else
      add(QCoreApplication::translate("KomaMimic", "自動組版の式"), {key},
          {{newKey, value}});
  }
  // Apply a key set simultaneously: exchanging two bindings must not depend
  // on INI ordering. Conflicting imports are rejected, leaving existing keys.
  QMap<QString, QString> keySources;
  QHash<QString, QKeySequence> proposed;
  for (auto it = plan.originals.cbegin(); it != plan.originals.cend(); ++it) {
    if (!it.key().startsWith("KeyBoardKeyConfig/CMD", Qt::CaseInsensitive))
      continue;
    handled.insert(it.key());
    bool ok = false;
    const int command =
        it.key().mid(QString("KeyBoardKeyConfig/CMD").size()).toInt(&ok);
    const auto id = ok ? legacyCommandId(command) : QString{};
    const int encoded = it.value().toInt(&ok);
    const auto key = ok ? legacyKeySequence(encoded) : QKeySequence{};
    if (!ok || id.isEmpty() || !bindings.contains(id) ||
        (key.isEmpty() && encoded != 0)) {
      plan.report << QCoreApplication::translate(
                         "KomaMimic", "保持（未対応のコマンド・キー）：") +
                         it.key();
      continue;
    }
    if (keySources.contains(id)) {
      const bool replace = proposed.value(id).isEmpty() && !key.isEmpty();
      plan.report << QCoreApplication::translate(
                         "KomaMimic", "保持（同じ操作への重複指定）：") +
                         (replace ? keySources.value(id) : it.key());
      if (!replace)
        continue;
    }
    keySources[id] = it.key();
    proposed[id] = key;
  }
  QHash<QString, ShortcutScope> scopes;
  for (const auto &spec : shortcutSpecs())
    scopes[spec.id] = spec.scope;
  const auto collides = [&](const QString &a, const QKeySequence &ka,
                            const QString &b, const QKeySequence &kb) {
    if (!scopes.contains(a) || !scopes.contains(b) || ka.isEmpty() ||
        kb.isEmpty())
      return false;
    if (scopes[a] != scopes[b] && scopes[a] != ShortcutScope::Global &&
        scopes[b] != ShortcutScope::Global)
      return false;
    const bool acceptPair=(a=="fullscreenGlobal" && (b=="thumbAccept" || b=="treeAccept"))
        || (b=="fullscreenGlobal" && (a=="thumbAccept" || a=="treeAccept"));
    if(acceptPair && ka==QKeySequence("Return") && kb==ka)return false;
    return ka.matches(kb) != QKeySequence::NoMatch;
  };
  const auto documentKey = [&](const QString &id, const QKeySequence &key) {
    return scopes.value(id) == ShortcutScope::Reader &&
                   documentBindings.contains(id)
               ? documentBindings.value(id)
               : key;
  };
  bool removed;
  do {
    removed = false;
    auto candidate = bindings;
    for (auto it = proposed.cbegin(); it != proposed.cend(); ++it)
      candidate[it.key()] = it.value();
    QSet<QString> rejected;
    for (auto it = proposed.cbegin(); it != proposed.cend(); ++it)
      for (auto other = candidate.cbegin(); other != candidate.cend();
           ++other) {
        if (it.key() == other.key())
          continue;
        if (collides(it.key(), it.value(), other.key(), other.value()) ||
            collides(it.key(), documentKey(it.key(), it.value()), other.key(),
                     documentKey(other.key(), other.value())))
          rejected.insert(it.key());
      }
    for (const auto &id : rejected) {
      proposed.remove(id);
      plan.report << QCoreApplication::translate("KomaMimic",
                                                 "保持（競合）：") +
                         keySources[id] + " → " + id;
      removed = true;
    }
  } while (removed);
  if (!proposed.isEmpty()) {
    QVariantMap values;
    QStringList sources;
    for (auto it = keySources.cbegin(); it != keySources.cend(); ++it)
      if (proposed.contains(it.key())) {
        values["keys/" + it.key()] =
            proposed[it.key()].toString(QKeySequence::PortableText);
        sources << it.value();
      }
    add(QCoreApplication::translate(
            "KomaMimic",
            "キーボードショートカット（競合しない項目をまとめて適用）"),
        sources, values);
  }
  const bool hasButtons =
      std::any_of(names.cbegin(), names.cend(), [](const QString &key) {
        return QRegularExpression("^ToolBar/Button[0-9]+$",
                                  QRegularExpression::CaseInsensitiveOption)
            .match(key)
            .hasMatch();
      });
  if (hasButtons) {
    int left = 0, right = 0;
    if (integer("ToolBar/LeftButtonNum", 0, 64, left) &&
        integer("ToolBar/RightButtonNum", 0, 64, right) && left + right <= 64) {
      QStringList groups[2], sources{source("ToolBar/LeftButtonNum"),
                                     source("ToolBar/RightButtonNum")};
      QSet<QString> seen;
      bool valid = true;
      for (int i = 1; i <= left + right; ++i) {
        const auto key = source(QString("ToolBar/Button%1").arg(i));
        int command;
        if (key.isEmpty() || !integer(key, 0, 175, command)) {
          valid = false;
          continue;
        }
        sources << key;
        handled.insert(key);
        const int group = i <= left ? 0 : 1;
        if (command == 0) {
          if (!groups[group].isEmpty() && groups[group].last() != "|")
            groups[group] << "|";
          continue;
        }
        const auto id = legacyCommandId(command);
        if (id.isEmpty() || !bindings.contains(id) || seen.contains(id)) {
          plan.report << QCoreApplication::translate(
                             "KomaMimic",
                             "保持（未対応または重複したボタン）：") +
                             key;
          if (id.isEmpty() || !bindings.contains(id))
            valid = false;
          continue;
        }
        groups[group] << id;
        seen.insert(id);
      }
      for (auto &group : groups)
        if (!group.isEmpty() && group.last() == "|")
          group.removeLast();
      if (valid)
        add(QCoreApplication::translate(
                "KomaMimic",
                "読書ツールバーの左右配置（空き枠は区切りへ変換）"),
            sources,
            {{"toolbar/leftActions", groups[0]},
             {"toolbar/rightActions", groups[1]}});
      else
        plan.report
            << QCoreApplication::translate(
                   "KomaMimic",
                   "保持（ツールバーの枠数とボタン定義を確認してください）：") +
                   "ToolBar";
    } else
      plan.report
          << QCoreApplication::translate(
                 "KomaMimic",
                 "保持（ツールバーの枠数とボタン定義を確認してください）：") +
                 "ToolBar";
  }
  for (auto it = plan.originals.cbegin(); it != plan.originals.cend(); ++it)
    if (!handled.contains(it.key()))
      plan.report << QCoreApplication::translate("KomaMimic",
                                                 "未対応・不明（原文保持）：") +
                         it.key();
  return plan;
}
