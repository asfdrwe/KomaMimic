// SPDX-License-Identifier: GPL-3.0-only
#include "idleinhibitor.h"
#include <QCoreApplication>
#include "mainwindow.h"
#include "readerfeatures.h"
#include <QApplication>
#include <QLabel>
#include <QStatusBar>
#include <QTimer>

IdleInhibitor *ReaderFeatures::idleInhibitor() const {
  return m_inhibitor.data();
}
void ReaderFeatures::initializeInhibit(IdleInhibitor *inhibitor) {
  m_inhibitor = inhibitor ? inhibitor : IdleInhibitor::instance();
  m_inhibitIndicator = new QLabel(w->statusBar());
  m_inhibitIndicator->setObjectName("inhibitIndicator");
  m_inhibitIndicator->setTextFormat(Qt::PlainText);
  w->statusBar()->addPermanentWidget(m_inhibitIndicator);
  connect(m_inhibitor, &IdleInhibitor::statusChanged, this,
          &ReaderFeatures::updateInhibitStatus);
  connect(qApp, &QGuiApplication::applicationStateChanged, this,
          [this] { updateInhibit(); });
  connect(qApp, &QGuiApplication::focusWindowChanged, this,
          [this] { QTimer::singleShot(0, this, [this] { updateInhibit(); }); });
  updateInhibitStatus();
}
void ReaderFeatures::updateInhibit() {
  if (!m_inhibitor)
    return;
  const IdleInhibitContext context{
      // Qt serializes either spelling of this INI section as [%General].
      // Use the preserved spelling when reading it in a new process.
      w->m_settings->value("General/inhibit", false).toBool(),
      w->pageCount() > 0,
      w->viewMode() == MainWindow::ViewMode::Reader,
      w->isVisible(),
      w->isMinimized(),
      w->isActiveWindow() &&
          QGuiApplication::applicationState() == Qt::ApplicationActive,
      property("closing").toBool()};
  m_inhibitor->setRequested(this, context.wanted());
  updateInhibitStatus();
}

void ReaderFeatures::updateInhibitStatus() {
  if (!m_inhibitor || !m_inhibitIndicator)
    return;
  const auto state = m_inhibitor->state();
  const auto description = m_inhibitor->description();
  w->setProperty("idleInhibitionStatus", description);
  w->setProperty("idleInhibitionState", int(state));
  const auto error = m_inhibitor->error();
  if (!error.isEmpty()) {
    w->setProperty("lastIdleInhibitionError", error);
    if (error != property("inhibitError").toString())
      w->statusBar()->showMessage(description, 10000);
    setProperty("inhibitError", error);
  } else if (state == IdleInhibitor::State::Active) {
    w->setProperty("lastIdleInhibitionError", QString{});
    setProperty("inhibitError", QString{});
  }
  const bool enabled = w->m_settings->value("General/inhibit", false).toBool();
  m_inhibitIndicator->setVisible(enabled);
  switch (state) {
  case IdleInhibitor::State::Inactive:
    m_inhibitIndicator->setText(QCoreApplication::translate("KomaMimic", "消灯抑制：待機"));
    break;
  case IdleInhibitor::State::Acquiring:
    m_inhibitIndicator->setText(QCoreApplication::translate("KomaMimic", "消灯抑制：要求中"));
    break;
  case IdleInhibitor::State::Active:
    m_inhibitIndicator->setText(QCoreApplication::translate("KomaMimic", "消灯抑制中"));
    break;
  case IdleInhibitor::State::Failed:
    m_inhibitIndicator->setText(QCoreApplication::translate("KomaMimic", "消灯抑制：失敗"));
    break;
  }
  auto detail = description;
  const auto last = w->property("lastIdleInhibitionError").toString();
  if (!last.isEmpty() && state != IdleInhibitor::State::Failed)
    detail += QCoreApplication::translate("KomaMimic", "\n直近の失敗：") + last;
  m_inhibitIndicator->setToolTip(detail);
}
