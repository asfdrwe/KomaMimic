// SPDX-License-Identifier: GPL-3.0-only
#include "videoview.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <algorithm>
#include <cmath>

VideoPreferences::VideoPreferences(QSettings &settings, QWidget *parent)
    : QWidget(parent), m_settings(settings) {
  setObjectName("videoPreferencesPage");
  auto *form = new QFormLayout(this);
  const auto check = [&](const QString &key, const QString &label,
                         bool fallback) {
    auto *c = new QCheckBox(this);
    c->setObjectName(key);
    c->setChecked(settings.value(key, fallback).toBool());
    m_editors[key] = c;
    form->addRow(label, c);
  };
  const auto number = [&](const QString &key, const QString &label,
                          int fallback, int low, int high) {
    auto *c = new QSpinBox(this);
    c->setObjectName(key);
    c->setRange(low, high);
    c->setValue(settings.value(key, fallback).toInt());
    m_editors[key] = c;
    form->addRow(label, c);
  };
  const auto choice = [&](const QString &key, const QString &label,
                          const QStringList &items, int fallback) {
    auto *c = new QComboBox(this);
    c->setObjectName(key);
    c->addItems(items);
    c->setCurrentIndex(std::clamp(settings.value(key, fallback).toInt(), 0,
                                  int(items.size() - 1)));
    m_editors[key] = c;
    form->addRow(label, c);
  };
  check("video/autoplay", tr("開いたら再生する"), true);
  check("video/resume", tr("記録位置から開始する（OFFは先頭）"), true);
  check("video/restoreAB", tr("A-B反復の有効状態を復元する"), false);
  check("video/pauseMinimized", tr("最小化時に一時停止する"), false);
  check("video/animations", tr("対応するGIF・APNG・WebPを動画モードで開く"),
        false);
  check("video/inhibit", tr("動画の表示・再生中に自動消灯を抑止する"), true);
  check("video/abSeekDisables",
        tr("区間外シーク時にA-Bを解除する（OFFはAへ戻す）"), false);
  choice("video/decode", tr("動画デコード（再起動後に適用）"),
         {tr("自動"), tr("ソフトウェア")}, 0);
  number("video/microSeek", tr("微小シーク（秒）"), 3, 1, 86400);
  number("video/seek", tr("通常シーク（秒）"), 10, 1, 86400);
  number("video/longSeek", tr("長時間シーク（秒）"), 60, 1, 86400);
  number("video/volumeStep", tr("音量の刻み（パーセントポイント）"), 5, 1, 100);
  for (const auto &key : {QString("video/rateMin"), QString("video/rateMax")}) {
    auto *c = new QDoubleSpinBox(this);
    c->setObjectName(key);
    c->setDecimals(2);
    c->setRange(.05, 16);
    c->setValue(settings.value(key, key.endsWith("Min") ? .25 : 4).toDouble());
    m_editors[key] = c;
    form->addRow(
        key.endsWith("Min") ? tr("最小速度（倍）") : tr("最大速度（倍）"), c);
  }
  auto *rates = new QLineEdit(
      settings.value("video/rates", "0.25,0.5,0.75,1,1.25,1.5,2,3,4")
          .toString(),
      this);
  rates->setObjectName("video/rates");
  m_editors["video/rates"] = rates;
  form->addRow(tr("速度候補（カンマ区切り）"), rates);
  choice("video/bar", tr("通常画面の操作バー"),
         {tr("自動"), tr("常時表示"), tr("非表示")}, 1);
  choice("video/fullscreenBar", tr("全画面の操作バー"),
         {tr("自動"), tr("常時表示"), tr("非表示")}, 0);
  number("video/hideMs", tr("操作バーを隠すまで（ミリ秒）"), 3000, 100, 60000);
  m_error = new QLabel(this);
  m_error->setObjectName("videoPreferencesError");
  m_error->setWordWrap(true);
  form->addRow(m_error);
  auto *note = new QLabel(
      tr("音程を維持して速度を変更します。内蔵字幕・形式はQtの対応範囲です。\n"
         "デコード方式の変更だけでは、保存済みの表紙を再生成しません。"),
      this);
  note->setWordWrap(true);
  form->addRow(note);
}
bool VideoPreferences::validate() {
  const auto low =
      qobject_cast<QDoubleSpinBox *>(m_editors["video/rateMin"])->value();
  const auto high =
      qobject_cast<QDoubleSpinBox *>(m_editors["video/rateMax"])->value();
  const auto list =
      qobject_cast<QLineEdit *>(m_editors["video/rates"])->text().split(',');
  bool valid = low <= 1 && high >= 1;
  QList<double> values;
  for (const auto &part : list) {
    bool ok = false;
    const auto value = part.trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(value) || value < low || value > high ||
        values.contains(value))
      valid = false;
    values.append(value);
  }
  if (!values.contains(1.))
    valid = false;
  m_error->setText(valid
                       ? QString{}
                       : tr("速度範囲は1倍を含め、候補は範囲内の正数を重複なく"
                            "指定してください。候補には1倍を含めてください。"));
  return valid;
}
bool VideoPreferences::save() {
  const auto before = m_settings.value("video/decode", 0).toInt();
  for (auto i = m_editors.cbegin(); i != m_editors.cend(); ++i) {
    QVariant value;
    if (auto *c = qobject_cast<QCheckBox *>(i.value()))
      value = c->isChecked();
    else if (auto *c = qobject_cast<QComboBox *>(i.value()))
      value = c->currentIndex();
    else if (auto *c = qobject_cast<QSpinBox *>(i.value()))
      value = c->value();
    else if (auto *c = qobject_cast<QDoubleSpinBox *>(i.value()))
      value = c->value();
    else if (auto *c = qobject_cast<QLineEdit *>(i.value()))
      value = c->text();
    m_settings.setValue(i.key(), value);
  }
  return before != m_settings.value("video/decode", 0).toInt();
}
