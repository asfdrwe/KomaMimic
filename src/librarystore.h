// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QJsonObject>
#include <QSqlDatabase>
#include <QVector>
struct LibraryItem {
  QString id, kind, path, title;
  QJsonObject metadata, state;
};
class LibraryStore {
public:
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
};
