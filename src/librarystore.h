// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QJsonObject>
#include <QSqlDatabase>
#include <QVector>
#include <QHash>
#include <functional>
#include <atomic>
#include <memory>
struct LibraryItem {
  QString id, kind, path, title;
  QJsonObject metadata, state;
};
class LibraryStore {
public:
  struct Pending {LibraryItem item;qint64 modified=0;quint64 revision=0;};
  void setDeferredWrites(bool enabled){m_deferred=enabled;}
  QVector<Pending> pendingWrites() const;
  static bool writePending(const QString &path,const QVector<Pending> &writes,const std::function<bool()> &cancel={});
  void acknowledge(const QVector<Pending> &writes);
  bool flushPending();
  std::shared_ptr<std::atomic<quint64>> writeEpoch() const {return m_writeEpoch;}
  explicit LibraryStore(const QString &path);
  ~LibraryStore();
  QString error() const { return m_error; }
  QVector<LibraryItem> items(const QString &kind) const;
  LibraryItem item(const QString &id) const;
  QString add(const QString &kind, const QString &path,
              const QString &title = {});
  bool update(const QString &id, const QJsonObject &metadata,
              const QJsonObject &state);
  bool remove(const QString &id);
  bool movePath(const QString &before,const QString &after);
  bool clear(const QString &kind);

private:
  QString m_connection, m_error;
  QSqlDatabase m_db;
  bool m_deferred=false;
  quint64 m_revision=0;
  std::shared_ptr<std::atomic<quint64>> m_writeEpoch=std::make_shared<std::atomic<quint64>>(0);
  QHash<QString,Pending> m_pending;
};
