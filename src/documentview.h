// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "publication.h"
#include <QWidget>
#include <QJsonObject>
class QWebEngineView;
class QWebEngineProfile;
class QTimer;
class DocumentView : public QWidget {
  Q_OBJECT
public:
  explicit DocumentView(QWidget *parent = nullptr);
  ~DocumentView() override;
  static void registerScheme();
  static QJsonObject readerMessages();
  void open(std::shared_ptr<const Publication> publication,
            const TextOptions &options, const DocumentLocation &location = {});
  void clear();
  void jump(int page);
  void find(const QString &text);
  void back();
  bool auxiliaryActive() const { return m_auxiliary; }
  void stepNote(int direction);
  void navigate(const QString &section, const QString &fragment, int offset);
  DocumentLocation location() const { return m_location; }
  bool ready() const { return m_ready; }
  bool vertical() const { return m_vertical; }
signals:
  void nextRequested();
  void previousRequested();
  void laidOut(const QVector<DocumentLocation> &locations);
  void positionChanged(int page, const DocumentLocation &location);
  void failed(const QString &error);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void poll();
  QWebEngineView *m_web;
  QWebEngineProfile *m_profile;
  QObject *m_handler;
  QTimer *m_timer;
  DocumentLocation m_location;
  bool m_ready = false, m_polling = false;
  bool m_vertical = false, m_auxiliary = false;
  quint64 m_generation = 0;
  int m_wheel = 0;
  QVector<DocumentLocation> m_locations;
};
