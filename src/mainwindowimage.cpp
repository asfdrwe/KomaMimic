// SPDX-License-Identifier: GPL-3.0-only
#include "mainwindow.h"
#include "pageview.h"
#include "processingdialog.h"
#include <QAction>
#include <QActionGroup>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>

namespace {
const QVector<QPair<QString, ResizeAlgorithm>> algorithms = {
    {"resizeSmooth", ResizeAlgorithm::Smooth}, {"resizeArea", ResizeAlgorithm::Area}, {"resizeLanczos", ResizeAlgorithm::Lanczos3},
    {"resizeBicubic", ResizeAlgorithm::Bicubic}, {"resizeBilinear", ResizeAlgorithm::Bilinear},
    {"resizeAreaWeak", ResizeAlgorithm::AreaSharpWeak}, {"resizeAreaStrong", ResizeAlgorithm::AreaSharpStrong}, {"resizeNearest", ResizeAlgorithm::Nearest}
};
const QVector<QPair<QString, ScaleMode>> scales = {{"scaleFit", ScaleMode::Fit}, {"scaleWidth", ScaleMode::Width}, {"scaleHeight", ScaleMode::Height}, {"scaleActual", ScaleMode::Actual}};
const QVector<QPair<QString, bool ImageOptions::*>> toggles = {
    {"filterEnabled", &ImageOptions::enabled}, {"filterFlipH", &ImageOptions::flipH}, {"filterFlipV", &ImageOptions::flipV},
    {"filterGray", &ImageOptions::gray}, {"filterInvert", &ImageOptions::invert}, {"filterBinary", &ImageOptions::binary}, {"filterAutoCrop", &ImageOptions::autoCrop}
};
}
void MainWindow::buildImageActions() {
    auto *modeGroup = new QActionGroup(this); modeGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    for (const auto &[id, mode] : scales) {
        auto *action = m_actions[id]; action->setCheckable(true); modeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { m_view->setScaleMode(mode); });
    }
    connect(m_actions["zoomIn"], &QAction::triggered, this, [this] { m_view->zoomBy(1.25); });
    connect(m_actions["zoomOut"], &QAction::triggered, this, [this] { m_view->zoomBy(0.8); });
    m_actions["noUpscale"]->setCheckable(true);
    connect(m_actions["noUpscale"], &QAction::toggled, m_view, &PageView::setNoUpscale);
    auto *algorithmGroup = new QActionGroup(this);
    for (const auto &[id, algorithm] : algorithms) {
        auto *action = m_actions[id]; action->setText(resizeAlgorithmName(algorithm)); action->setCheckable(true);
        algorithmGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, algorithm] { setResizeAlgorithm(algorithm); });
    }
    auto *sharpenGroup = new QActionGroup(this);
    for (const auto &[id, value] : QVector<QPair<QString, int>>{{"sharpenNone",0}, {"sharpenWeak",1}, {"sharpenStrong",2}}) {
        auto *action = m_actions[id]; action->setCheckable(true); sharpenGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, value] { auto options = m_imageOptions; options.sharpen = value; setImageOptions(options); });
    }
    for (const auto &[id, member] : toggles) {
        m_actions[id]->setCheckable(true);
        connect(m_actions[id], &QAction::toggled, this, [this, member](bool enabled) { auto options = m_imageOptions; options.*member = enabled; setImageOptions(options); });
    }
    for (const auto &[id, turns] : QVector<QPair<QString, int>>{{"rotateRight",1}, {"rotateLeft",3}, {"rotateHalf",2}})
        connect(m_actions[id], &QAction::triggered, this, [this, turns] { auto options = m_imageOptions; options.rotation = (options.rotation + turns) % 4; setImageOptions(options); });
    connect(m_actions["filterReset"], &QAction::triggered, this, [this] { setImageOptions({}); });
    connect(m_actions["filterSettings"], &QAction::triggered, this, &MainWindow::editImageOptions);
    connect(m_view, &PageView::scaleChanged, this, [this] { updateImageActionChecks(); saveImageSettings(); });
}
void MainWindow::setResizeAlgorithm(ResizeAlgorithm algorithm) { m_view->setResizeAlgorithm(algorithm); }
void MainWindow::setImageOptions(const ImageOptions &options, bool save) {
    m_imageOptions = options; m_imageOptions.rotation = ((options.rotation % 4) + 4) % 4;
    m_imageOptions.gamma = std::isfinite(options.gamma) ? std::clamp(options.gamma, 0.1, 4.0) : 1;
    m_imageOptions.brightness = std::clamp(options.brightness, -100, 100); m_imageOptions.contrast = std::clamp(options.contrast, -100, 100);
    m_imageOptions.threshold = std::clamp(options.threshold, 0, 255); m_imageOptions.sharpen = std::clamp(options.sharpen, 0, 2);
    updateImageActionChecks(); render(); if (save) saveImageSettings();
}
void MainWindow::updateImageActionChecks() {
    for (const auto &[id, algorithm] : algorithms) { const QSignalBlocker blocker(m_actions[id]); m_actions[id]->setChecked(m_view->resizeAlgorithm() == algorithm); }
    for (const auto &[id, mode] : scales) { const QSignalBlocker blocker(m_actions[id]); m_actions[id]->setChecked(m_view->scaleMode() == mode); }
    { const QSignalBlocker blocker(m_actions["noUpscale"]); m_actions["noUpscale"]->setChecked(m_view->noUpscale()); }
    for (const auto &[id, member] : toggles) { const QSignalBlocker blocker(m_actions[id]); m_actions[id]->setChecked(m_imageOptions.*member); }
    for (const auto &[id, value] : QVector<QPair<QString, int>>{{"sharpenNone",0}, {"sharpenWeak",1}, {"sharpenStrong",2}}) {
        const QSignalBlocker blocker(m_actions[id]); m_actions[id]->setChecked(m_imageOptions.sharpen == value);
    }
}
void MainWindow::editImageOptions() {
    const auto original = m_imageOptions;
    ProcessingDialog dialog(original, this);
    connect(&dialog, &ProcessingDialog::previewRequested, this, [this](const ImageOptions &options) { setImageOptions(options, false); });
    if (dialog.exec() == QDialog::Accepted) setImageOptions(dialog.options()); else setImageOptions(original, false);
    focusCurrentView();
}
void MainWindow::restoreImageSettings() {
    m_view->setBackgroundColor(QColor(m_settings->value("display/background").toString()));
    ImageOptions options;
    for (const auto &[id, member] : toggles) options.*member = m_settings->value("processing/" + id, options.*member).toBool();
    options.rotation = m_settings->value("processing/rotation", 0).toInt();
    options.brightness = m_settings->value("processing/brightness", 0).toInt(); options.contrast = m_settings->value("processing/contrast", 0).toInt();
    options.gamma = m_settings->value("processing/gamma", 1).toDouble(); options.threshold = m_settings->value("processing/threshold", 128).toInt();
    options.whiteTolerance = m_settings->value("processing/whiteTolerance", 12).toInt(); options.sharpen = m_settings->value("processing/sharpen", 0).toInt();
    options.crop = QMargins(m_settings->value("processing/cropLeft",0).toInt(), m_settings->value("processing/cropTop",0).toInt(), m_settings->value("processing/cropRight",0).toInt(), m_settings->value("processing/cropBottom",0).toInt());
    restoreAdvancedImageSettings(options,m_settings->value("processing/advanced").toMap());
    setImageOptions(options, false);
    m_view->setResizeAlgorithm(ResizeAlgorithm(std::clamp(m_settings->value("display/algorithm", 0).toInt(), 0, 7)));
    const auto mode = ScaleMode(std::clamp(m_settings->value("display/scaleMode", 0).toInt(), 0, 6));
    if (mode == ScaleMode::Zoom) {
        m_view->setScaleMode(ScaleMode::Actual); m_view->zoomBy(m_settings->value("display/zoom", 1).toDouble());
    } else m_view->setScaleMode(mode);
    m_view->setNoUpscale(m_settings->value("display/noUpscale", false).toBool()); updateImageActionChecks();
}
void MainWindow::saveImageSettings() {
    if (m_restoring) return;
    for (const auto &[id, member] : toggles) m_settings->setValue("processing/" + id, m_imageOptions.*member);
    m_settings->setValue("processing/advanced",advancedImageSettings(m_imageOptions));
    m_settings->setValue("processing/rotation", m_imageOptions.rotation);
    m_settings->setValue("processing/brightness", m_imageOptions.brightness); m_settings->setValue("processing/contrast", m_imageOptions.contrast);
    m_settings->setValue("processing/gamma", m_imageOptions.gamma); m_settings->setValue("processing/threshold", m_imageOptions.threshold);
    m_settings->setValue("processing/whiteTolerance", m_imageOptions.whiteTolerance); m_settings->setValue("processing/sharpen", m_imageOptions.sharpen);
    m_settings->setValue("processing/cropLeft", m_imageOptions.crop.left()); m_settings->setValue("processing/cropTop", m_imageOptions.crop.top());
    m_settings->setValue("processing/cropRight", m_imageOptions.crop.right()); m_settings->setValue("processing/cropBottom", m_imageOptions.crop.bottom());
    m_settings->setValue("display/algorithm", int(m_view->resizeAlgorithm())); m_settings->setValue("display/scaleMode", int(m_view->scaleMode()));
    m_settings->setValue("display/zoom", m_view->zoomFactor()); m_settings->setValue("display/noUpscale", m_view->noUpscale()); m_settings->sync();
}
