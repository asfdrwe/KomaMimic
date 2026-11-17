// SPDX-License-Identifier: GPL-3.0-only
#include "displayjobs.h"
#include "mainwindow.h"
#include "readerfeatures.h"
#include "readertoolbar.h"
#include "shortcuts.h"
#include "thumbnailsource.h"
#include "videoview.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QVBoxLayout>

void MainWindow::buildVideoActions() {
  connect(m_video, &VideoView::painted, this, [this] {
    if (m_mode != ViewMode::Video)
      return;
    m_readerPresented = true;
    m_settings->setValue("lastBook", m_currentPath);
    m_settings->setValue("lastDirectory", m_directory);
    if (m_features)
      m_features->requestBackgroundWork();
  });
  for (const auto &spec : shortcutSpecs())
    if (spec.scope == ShortcutScope::Video && spec.id != "videoSettings" &&
        spec.id != "videoRefreshCover")
      connect(m_actions[spec.id], &QAction::triggered, this,
              [this, id = spec.id] {
                if (m_mode == ViewMode::Video)
                  m_video->command(id);
              });
  connect(m_actions["videoPreferences"], &QAction::triggered, this,
          [this] { m_features->preferences("video"); });
  connect(m_actions["videoSettings"], &QAction::triggered, this,
          &MainWindow::videoSettings);
  connect(m_actions["videoRefreshCover"], &QAction::triggered, this,
          &MainWindow::refreshVideoCover);
  connect(m_video, &VideoView::coverRequested, this,
          &MainWindow::registerVideoCover);
  connect(m_video, &VideoView::framePresented, this, [this] {
    if (m_mode != ViewMode::Video)
      return;
    if (!m_renderReady) {
      m_loading = false;
      m_renderReady = true;
      refreshState();
    }
  });
  connect(m_video, &VideoView::changed, this, [this] {
    if (m_mode == ViewMode::Video) {
      refreshVideoActions();
      if (m_features)
        m_features->updateInhibit();
    }
  });
  connect(m_video, &VideoView::errorOccurred, this,
          [this](const QString &error) {
            if (m_mode == ViewMode::Video) {
              m_loading = false;
              m_lastError = error;
              refreshState();
            }
          });
  connect(m_video, &VideoView::ended, this, [this] {
    if (m_mode != ViewMode::Video)
      return;
    if (m_options.boundaryMode == BoundaryMode::RepeatSource)
      m_video->command("videoRestart");
    else if (m_options.boundaryMode == BoundaryMode::AdvanceSource)
      requestAdjacentBook(1, ViewMode::Reader);
  });
}
void MainWindow::refreshVideoActions() {
  if (!m_video)
    return;
  const bool active = m_mode == ViewMode::Video;
  for (const auto &spec : shortcutSpecs())
    if (spec.scope == ShortcutScope::Video)
      m_actions[spec.id]->setEnabled(active);
  m_actions["videoCover"]->setEnabled(active && m_video->ready());
  for (const auto &id : {"videoPlayPause", "videoStop", "videoRestart"})
    m_actions[id]->setEnabled(active && m_video->m_loaded);
  m_actions["videoAB"]->setCheckable(true);
  {
    QSignalBlocker b(m_actions["videoAB"]);
    m_actions["videoAB"]->setChecked(active && m_video->state().ab);
  }
  if (active) {
    const auto *p = m_video->player();
    const bool seek = p && p->isSeekable();
    for (const auto &id :
         {"next", "previous", "nextOne", "previousOne", "first", "last"})
      m_actions[id]->setEnabled(seek);
    for (const auto &id :
         {"videoNext", "videoPrevious", "videoNextOne", "videoPreviousOne",
          "videoLongNext", "videoLongPrevious", "videoFirst", "videoLast",
          "videoGoTime", "videoSetA", "videoSetB"})
      m_actions[id]->setEnabled(seek);
    m_actions["videoAB"]->setEnabled(seek && m_video->state().a >= 0 &&
                                     m_video->state().b > m_video->state().a);
    m_actions["thumbnails"]->setEnabled(false);
    for (const auto &id : {"scaleFit", "scaleActual", "zoomIn", "zoomOut",
                           "closeBook", "nextBook", "previousBook"})
      m_actions[id]->setEnabled(true);
    m_actions["nextBook"]->setEnabled(!archiveNeighborsKnown() ||
                                      !adjacentArchive(1).isEmpty());
    m_actions["previousBook"]->setEnabled(!archiveNeighborsKnown() ||
                                          !adjacentArchive(-1).isEmpty());
    setWindowTitle(QFileInfo(m_currentPath).fileName() + " — KomaMimic");
    const auto state = m_video->state();
    const QStringList methods{tr("無効"), "Blend", "Bob", "Linear"};
    m_actions["videoFieldOrder"]->setText(state.bottomFirst
                                              ? tr("フィールド順：BFF → TFF")
                                              : tr("フィールド順：TFF → BFF"));
    m_actions["videoDeinterlace"]->setText(
        tr("方式巡回：%1（%2）")
            .arg(methods[int(state.deinterlace)],
                 state.bottomFirst ? "BFF" : "TFF"));
    m_status->setText(m_video->error().isEmpty()
                          ? tr("動画：%1  %2倍  %3（%4）%5")
                                .arg(QFileInfo(m_currentPath).fileName())
                                .arg(state.rate)
                                .arg(methods[int(state.deinterlace)],
                                     state.bottomFirst ? "BFF" : "TFF",
                                     state.ab ? tr("  A-B反復中") : QString{})
                          : m_video->error());
    m_toolbar->hide();
  } else {
    m_actions["thumbnails"]->setEnabled(true);
    if (!isFullScreen())
      m_toolbar->setVisible(m_toolbarBeforeFullscreen);
  }
  if (m_videoMenu)
    for (auto *action : m_videoMenu->actions())
      if (action->menu())
        action->setEnabled(active);
}
void MainWindow::registerVideoCover() {
  if (m_mode != ViewMode::Video || !m_video->ready())
    return;
  m_video->rememberCover();
  const auto pixels = m_video->coverImage();
  const auto path = m_currentPath;
  const auto policy = namePolicyFor(path);
  const auto cache = m_thumbnailDiskCache;
  m_displayJobs->retain("video-cover");
  m_displayJobs->submit<ImageResult>(
      DisplayJobs::Lane::Display, "video-cover", path,
      [path, policy, cache, pixels](const auto &cancel) {
        return storeVideoCover(path, policy, cache, pixels, cancel);
      },
      [this, path](ImageResult result, bool cancelled) {
        if (cancelled || result.image.isNull())
          return;
        for (int i = 0; i < m_library->count(); ++i)
          if (m_library->item(i)->data(PathRole).toString() == path)
            m_library->setThumbnail(i, result.image, {});
      });
}
void MainWindow::refreshVideoCover() {
  if (m_mode != ViewMode::Video)
    return;
  const auto path = m_currentPath, config = m_settings->fileName();
  const auto cache = m_thumbnailDiskCache;
  const auto policy = namePolicyFor(path);
  m_displayJobs->retain("video-cover");
  m_displayJobs->submit<ImageResult>(
      DisplayJobs::Lane::Display, "video-cover", path,
      [path, config, cache, policy](const auto &cancel) {
        return cachedCover(path, policy, cache, cancel, true, 0, false, false,
                           0, {}, false, nullptr, config);
      },
      [this, path](ImageResult image, bool cancelled) {
        if (cancelled)
          return;
        if (!image.error.isEmpty()) {
          statusBar()->showMessage(image.error, 10000);
          return;
        }
        for (int i = 0; i < m_library->count(); ++i)
          if (m_library->item(i)->data(PathRole).toString() == path)
            m_library->setThumbnail(i, image.image, {});
      });
}
void MainWindow::videoSettings() {
  if (m_mode != ViewMode::Video)
    return;
  QDialog dialog(this);
  dialog.setObjectName("videoSettingsDialog");
  dialog.setWindowTitle(tr("動画の個別設定"));
  auto *layout = new QVBoxLayout(&dialog);
  auto *form = new QFormLayout;
  layout->addLayout(form);
  const auto state = m_video->state();
  auto *rate = new QDoubleSpinBox(&dialog);
  rate->setObjectName("videoFileRate");
  const auto options = VideoOptions::load(*m_settings);
  rate->setRange(options.rateMin, options.rateMax);
  rate->setSingleStep(.25);
  rate->setValue(state.rate);
  form->addRow(tr("速度（倍）"), rate);
  auto *aspect = new QComboBox(&dialog);
  aspect->addItems({tr("動画内指定"), "1:1", "4:3", "16:9"});
  aspect->setCurrentIndex(state.aspect);
  form->addRow(tr("アスペクト比"), aspect);
  auto *deint = new QComboBox(&dialog);
  deint->addItems({tr("無効"), "Blend", "Bob", "Linear"});
  deint->setCurrentIndex(int(state.deinterlace));
  form->addRow(tr("デインタレース"), deint);
  auto *order = new QComboBox(&dialog);
  order->addItems({tr("上フィールド先（TFF）"), tr("下フィールド先（BFF）")});
  order->setCurrentIndex(state.bottomFirst ? 1 : 0);
  form->addRow(tr("フィールド順"), order);
  auto *note =
      new QLabel(tr("トラック・内蔵字幕・A-"
                    "B位置は動画メニューから変更できます。\n現在の時刻、選択、"
                    "速度、表示設定はこの動画ごとに保存します。"),
                 &dialog);
  note->setWordWrap(true);
  layout->addWidget(note);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted)
    return;
  m_video->setRate(rate->value());
  m_video->setAspect(aspect->currentIndex());
  m_video->setDeinterlace(DeinterlaceMode(deint->currentIndex()));
  m_video->setBottomFirst(order->currentIndex() == 1);
}
