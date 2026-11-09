// SPDX-License-Identifier: GPL-3.0-only
#include "mainwindow.h"
#include <QCoreApplication>
#include "readerfeatures.h"
#include <QAction>
#include <QFileInfo>
#include <QJsonArray>
#include <QTimer>
void ReaderFeatures::runRequest(const QJsonObject &request) {
  const auto path = request.value("path").toString();
  const int entry = request.value("entry").toInt(-1);
  if (request.contains("pageMode")) {
    const int mode = request.value("pageMode").toInt();
    w->m_actions["spread"]->setChecked(mode != 1);
    w->m_settings->setValue("reading/autoSpread", mode == 0);
    w->m_actions["autoSpread"]->setChecked(mode == 0);
  }
  const auto add = request.value("add").toString();
  if (!add.isEmpty()) {
    m_library->add("playlist", add);
    rebuildLists();
  }
  if (!path.isEmpty()) {
    if (entry == 0 || entry == 2) {
      m_pendingId = m_library->add("history", path);
      m_intent = entry == 0 ? 0 : 2;
    }
    if ((QFileInfo(path).isDir() && entry < 0) || entry == 4)
      w->openDirectory(
          QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath());
    else
      w->loadBook(path, entry == 3 ? MainWindow::ViewMode::Pages
                                   : MainWindow::ViewMode::Reader);
  }
  auto *timer = new QTimer(this);
  timer->setInterval(20);
  auto attempts = std::make_shared<int>(0);
  auto initialized=std::make_shared<bool>(false);
  auto commandIndex=std::make_shared<int>(0);
  connect(timer, &QTimer::timeout, this, [this, timer, request, attempts, initialized, commandIndex] {
    const auto ready = [this] {
      return w->requestReady() &&
          (w->viewMode()!=MainWindow::ViewMode::Reader || !w->m_book || w->m_readerPresented);
    };
    // Apply --page as soon as the book is ready; only its target page needs
    // presentation before commands, not the initial page selected by opening.
    const bool canContinue=*initialized ? ready() : w->requestReady();
    if (!canContinue &&
        ++*attempts < 3000)
      return;
    if(!*initialized) {
    *initialized=true;
    if (request.contains("page") && w->pageCount())
      w->jump(
          std::clamp(request.value("page").toInt() - 1, 0, w->pageCount() - 1));
    if (request.value("fullscreen").toBool())
      w->setFullscreen(true);
    }
    const auto commands=request.value("commands").toArray();
    while(*commandIndex<commands.size()) {
      // A page selection or previous command can start asynchronous rendering.
      // Let that page be presented before a following mode change or quit.
      if(m_imageCommandsActive || (!ready() && *attempts<3000))return;
      const auto value=commands[(*commandIndex)++];
      const auto text = value.toString();
      bool numeric = false;
      const int id = text.toInt(&numeric);
      if (numeric)
        executeLegacy(id);
      else if (auto *action = w->command(text); action && action->isEnabled())
        action->trigger();
      else
        message(QCoreApplication::translate("KomaMimic", "実行できないコマンド: ") + text);
    }
    if(m_imageCommandsActive || (!ready() && *attempts<3000))return;
    timer->stop();timer->deleteLater();
  });
  timer->start();
}
void MainWindow::runRequest(const QJsonObject &request) {
  m_features->runRequest(request);
}
