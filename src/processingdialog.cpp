// SPDX-License-Identifier: GPL-3.0-only
#include "processingdialog.h"
#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
ProcessingDialog::ProcessingDialog(const ImageOptions &options, QWidget *parent)
    : QDialog(parent) {
  setObjectName("processingDialog");
  setWindowTitle(QCoreApplication::translate("KomaMimic", "画像加工の設定"));
  resize(600, 690);
  auto *layout = new QVBoxLayout(this);
  auto *info = new QLabel(
      QCoreApplication::translate("KomaMimic", "表示する画像だけに適用します。元ファイルは変更しません。\n切り抜き（元画"
      "像のpixel）→余白除去→回転・反転→色調→シャープの順です。"),
      this);
  info->setWordWrap(true);
  layout->addWidget(info);
  m_previewTimer = new QTimer(this);
  m_previewTimer->setSingleShot(true);
  m_previewTimer->setInterval(180);
  connect(m_previewTimer, &QTimer::timeout, this,
          [this] { emit previewRequested(this->options()); });
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  auto *contents = new QWidget(scroll);
  scroll->setWidget(contents);
  layout->addWidget(scroll);
  auto *form = new QFormLayout(contents);
  const auto check = [&](const QString &label, const char *id) {
    auto *box = new QCheckBox(this);
    box->setObjectName(id);
    form->addRow(label, box);
    connect(box, &QCheckBox::toggled, this,
            [this] { m_previewTimer->start(); });
    return box;
  };
  const auto spin = [&](const QString &label, const char *id, int low,
                        int high) {
    auto *value = new QSpinBox(this);
    value->setObjectName(id);
    value->setRange(low, high);
    form->addRow(label, value);
    connect(value, &QSpinBox::valueChanged, this,
            [this] { m_previewTimer->start(); });
    return value;
  };
  m_enabled = check(QCoreApplication::translate("KomaMimic", "画像加工を有効"), "filterEnabled");
  m_brightness = spin(QCoreApplication::translate("KomaMimic", "明るさ（%）"), "filterBrightness", -100, 100);
  m_contrast = spin(QCoreApplication::translate("KomaMimic", "コントラスト"), "filterContrast", -100, 100);
  m_gamma = new QDoubleSpinBox(this);
  m_gamma->setObjectName("filterGamma");
  m_gamma->setRange(0.1, 4);
  m_gamma->setSingleStep(0.1);
  m_gamma->setDecimals(2);
  form->addRow(QCoreApplication::translate("KomaMimic", "ガンマ"), m_gamma);
  connect(m_gamma, &QDoubleSpinBox::valueChanged, this,
          [this] { m_previewTimer->start(); });
  m_gray = check(QCoreApplication::translate("KomaMimic", "グレースケール"), "filterGray");
  m_invert = check(QCoreApplication::translate("KomaMimic", "色反転"), "filterInvert");
  m_binary = check(QCoreApplication::translate("KomaMimic", "2値化"), "filterBinary");
  m_threshold = spin(QCoreApplication::translate("KomaMimic", "2値化の閾値"), "filterThreshold", 0, 255);
  m_rotation = new QComboBox(this);
  m_rotation->setObjectName("filterRotation");
  m_rotation->addItems({"0°", QCoreApplication::translate("KomaMimic", "右90°"), "180°", QCoreApplication::translate("KomaMimic", "左90°")});
  form->addRow(QCoreApplication::translate("KomaMimic", "回転"), m_rotation);
  connect(m_rotation, &QComboBox::currentIndexChanged, this,
          [this] { m_previewTimer->start(); });
  m_flipH = check(QCoreApplication::translate("KomaMimic", "左右反転"), "filterFlipH");
  m_flipV = check(QCoreApplication::translate("KomaMimic", "上下反転"), "filterFlipV");
  m_left = spin(QCoreApplication::translate("KomaMimic", "左を切り抜く（px）"), "filterCropLeft", 0, 65535);
  m_top = spin(QCoreApplication::translate("KomaMimic", "上を切り抜く（px）"), "filterCropTop", 0, 65535);
  m_right = spin(QCoreApplication::translate("KomaMimic", "右を切り抜く（px）"), "filterCropRight", 0, 65535);
  m_bottom = spin(QCoreApplication::translate("KomaMimic", "下を切り抜く（px）"), "filterCropBottom", 0, 65535);
  m_autoCrop = check(QCoreApplication::translate("KomaMimic", "白い余白を自動除去"), "filterAutoCrop");
  m_tolerance = spin(QCoreApplication::translate("KomaMimic", "白とみなす許容差"), "filterWhiteTolerance", 0, 100);
  m_sharpen = new QComboBox(this);
  m_sharpen->setObjectName("filterSharpen");
  m_sharpen->addItems({QCoreApplication::translate("KomaMimic", "なし"), QCoreApplication::translate("KomaMimic", "弱"), QCoreApplication::translate("KomaMimic", "強")});
  form->addRow(QCoreApplication::translate("KomaMimic", "アンシャープマスク"), m_sharpen);
  connect(m_sharpen, &QComboBox::currentIndexChanged, this,
          [this] { m_previewTimer->start(); });
  const auto extraSpin = [&](const QString &key, const QString &label, int low,
                             int high) {
    auto *value = spin(label, qPrintable("advanced_" + key), low, high);
    m_extra[key] = value;
  };
  for (const auto &[key, label] : QVector<QPair<QString, QString>>{
           {"blackPoint", QCoreApplication::translate("KomaMimic", "入力黒レベル")},
           {"whitePoint", QCoreApplication::translate("KomaMimic", "入力白レベル")},
           {"outputBlack", QCoreApplication::translate("KomaMimic", "出力黒レベル")},
           {"outputWhite", QCoreApplication::translate("KomaMimic", "出力白レベル")},
           {"curveShadows", QCoreApplication::translate("KomaMimic", "トーンカーブ 25%の出力")},
           {"curveMidtones", QCoreApplication::translate("KomaMimic", "トーンカーブ 50%の出力")},
           {"curveHighlights", QCoreApplication::translate("KomaMimic", "トーンカーブ 75%の出力")}})
    extraSpin(key, label, 0, 255);
  extraSpin("denoise", QCoreApplication::translate("KomaMimic", "ノイズ除去（0:無効 / 1:中央値）"), 0, 1);
  extraSpin("lineWidth", QCoreApplication::translate("KomaMimic", "線の太さ（負:細く / 正:太く）"), -3, 3);
  extraSpin("resizeWidth", QCoreApplication::translate("KomaMimic", "読み込み後の最大幅（0:無効）"), 0, 8192);
  extraSpin("resizeHeight", QCoreApplication::translate("KomaMimic", "読み込み後の最大高さ（0:無効）"), 0, 8192);
  for (const auto &[key, label] : QVector<QPair<QString, QString>>{
           {"borderLeft", QCoreApplication::translate("KomaMimic", "左に余白を追加（px）")},
           {"borderTop", QCoreApplication::translate("KomaMimic", "上に余白を追加（px）")},
           {"borderRight", QCoreApplication::translate("KomaMimic", "右に余白を追加（px）")},
           {"borderBottom", QCoreApplication::translate("KomaMimic", "下に余白を追加（px）")}})
    extraSpin(key, label, 0, 4096);
  extraSpin("textSize", QCoreApplication::translate("KomaMimic", "重ねる文字のサイズ（px）"), 8, 200);
  for (const auto &[key, label] :
       QVector<QPair<QString, QString>>{{"borderColor", QCoreApplication::translate("KomaMimic", "余白の色（#RRGGBB）")},
                                        {"textColor", QCoreApplication::translate("KomaMimic", "文字の色（#RRGGBB）")},
                                        {"overlay", QCoreApplication::translate("KomaMimic", "重ねる文字")}}) {
    auto *edit = new QLineEdit(contents);
    m_extra[key] = edit;
    form->addRow(label, edit);
    connect(edit, &QLineEdit::textEdited, this,
            [this] { m_previewTimer->start(); });
  }
  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel |
                               QDialogButtonBox::RestoreDefaults,
                           this);
  buttons->button(QDialogButtonBox::Save)->setObjectName("saveProcessing");
  buttons->button(QDialogButtonBox::Save)->setText(QCoreApplication::translate("KomaMimic", "適用して保存"));
  buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("KomaMimic", "キャンセル"));
  buttons->button(QDialogButtonBox::RestoreDefaults)->setText(QCoreApplication::translate("KomaMimic", "初期値に戻す"));
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(buttons->button(QDialogButtonBox::RestoreDefaults),
          &QPushButton::clicked, this, [this] {
            setOptions({});
            m_previewTimer->start();
          });
  layout->addWidget(buttons);
  setOptions(options);
  m_previewTimer->stop();
}
void ProcessingDialog::setOptions(const ImageOptions &o) {
  const auto v = advancedImageSettings(o);
  for (auto it = m_extra.begin(); it != m_extra.end(); ++it) {
    if (auto *spin = qobject_cast<QSpinBox *>(it.value()))
      spin->setValue(v.value(it.key()).toInt());
    else if (auto *edit = qobject_cast<QLineEdit *>(it.value()))
      edit->setText(v.value(it.key()).toString());
  }
  m_enabled->setChecked(o.enabled);
  m_brightness->setValue(o.brightness);
  m_contrast->setValue(o.contrast);
  m_gamma->setValue(o.gamma);
  m_gray->setChecked(o.gray);
  m_invert->setChecked(o.invert);
  m_binary->setChecked(o.binary);
  m_threshold->setValue(o.threshold);
  m_rotation->setCurrentIndex(o.rotation);
  m_flipH->setChecked(o.flipH);
  m_flipV->setChecked(o.flipV);
  m_left->setValue(o.crop.left());
  m_top->setValue(o.crop.top());
  m_right->setValue(o.crop.right());
  m_bottom->setValue(o.crop.bottom());
  m_autoCrop->setChecked(o.autoCrop);
  m_tolerance->setValue(o.whiteTolerance);
  m_sharpen->setCurrentIndex(o.sharpen);
}
ImageOptions ProcessingDialog::options() const {
  ImageOptions o;
  o.enabled = m_enabled->isChecked();
  o.brightness = m_brightness->value();
  o.contrast = m_contrast->value();
  o.gamma = m_gamma->value();
  o.gray = m_gray->isChecked();
  o.invert = m_invert->isChecked();
  o.binary = m_binary->isChecked();
  o.threshold = m_threshold->value();
  o.rotation = m_rotation->currentIndex();
  o.flipH = m_flipH->isChecked();
  o.flipV = m_flipV->isChecked();
  o.crop = QMargins(m_left->value(), m_top->value(), m_right->value(),
                    m_bottom->value());
  o.autoCrop = m_autoCrop->isChecked();
  o.whiteTolerance = m_tolerance->value();
  o.sharpen = m_sharpen->currentIndex();
  QVariantMap v;
  for (auto it = m_extra.begin(); it != m_extra.end(); ++it) {
    if (auto *spin = qobject_cast<QSpinBox *>(it.value()))
      v[it.key()] = spin->value();
    else if (auto *edit = qobject_cast<QLineEdit *>(it.value()))
      v[it.key()] = edit->text();
  }
  restoreAdvancedImageSettings(o, v);
  return o;
}
