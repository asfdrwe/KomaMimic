// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "imageprocessing.h"
#include <QDialog>
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QTimer;
class ProcessingDialog : public QDialog {
  Q_OBJECT
public:
  explicit ProcessingDialog(const ImageOptions &options,
                            QWidget *parent = nullptr);
  ImageOptions options() const;
signals:
  void previewRequested(const ImageOptions &options);

private:
  void setOptions(const ImageOptions &options);
  QCheckBox *m_enabled, *m_gray, *m_invert, *m_binary, *m_autoCrop, *m_flipH,
      *m_flipV;
  QSpinBox *m_brightness, *m_contrast, *m_threshold, *m_tolerance, *m_left,
      *m_top, *m_right, *m_bottom;
  QDoubleSpinBox *m_gamma;
  QComboBox *m_rotation, *m_sharpen;
  QTimer *m_previewTimer;
  QHash<QString, QWidget *> m_extra;
};
