// SPDX-License-Identifier: GPL-3.0-only
#include "librarystore.h"
#include "archivepath.h"
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <algorithm>
namespace {
LibraryItem read(const QSqlQuery &query) {
  return {query.value(0).toString(),
          query.value(1).toString(),
          query.value(2).toString(),
          query.value(3).toString(),
          QJsonDocument::fromJson(query.value(4).toByteArray()).object(),
          QJsonDocument::fromJson(query.value(5).toByteArray()).object()};
}
} // namespace
LibraryStore::LibraryStore(const QString &path)
    : m_connection(QUuid::createUuid().toString()) {
  m_db = QSqlDatabase::addDatabase("QSQLITE", m_connection);
  m_db.setDatabaseName(path);
  if (!m_db.open()) {
    m_error = m_db.lastError().text();
    return;
  }
  QSqlQuery query(m_db);
  query.exec("PRAGMA busy_timeout=3000");
  query.exec("PRAGMA journal_mode=WAL");
  if (!query.exec(
          "CREATE TABLE IF NOT EXISTS items(id TEXT PRIMARY KEY,kind TEXT NOT "
          "NULL,path TEXT NOT NULL,title TEXT NOT NULL,metadata TEXT NOT NULL "
          "DEFAULT '{}',state TEXT NOT NULL DEFAULT '{}',modified INTEGER NOT "
          "NULL,UNIQUE(kind,path))"))
    m_error = query.lastError().text();
  query.exec("PRAGMA user_version=1");
}
LibraryStore::~LibraryStore() {
  flushPending();
  m_db.close();
  m_db = {};
  QSqlDatabase::removeDatabase(m_connection);
}
QVector<LibraryItem> LibraryStore::items(const QString &kind) const {
  QVector<LibraryItem> values;
  QSqlQuery query(m_db);
  query.prepare(
      "SELECT id,kind,path,title,metadata,state FROM items WHERE "
      "kind=? ORDER BY " +
      QString(kind == "history" ? "modified DESC,rowid DESC" : "rowid ASC"));
  query.addBindValue(kind);
  if (query.exec())
    while (query.next())
      values.append(read(query));
  for(const auto &pending:m_pending)if(pending.item.kind==kind) {
    const auto found=std::find_if(values.begin(),values.end(),[&](const auto &item){return item.id==pending.item.id;});
    if(found!=values.end())*found=pending.item;else values.append(pending.item);
  }
  if(kind=="history" && !m_pending.isEmpty())std::stable_sort(values.begin(),values.end(),[&](const auto &a,const auto &b){
    const auto x=m_pending.constFind(a.id),y=m_pending.constFind(b.id);
    if(x==m_pending.cend())return false;
    if(y==m_pending.cend())return true;
    return x->modified==y->modified?x->revision>y->revision:x->modified>y->modified;
  });
  if (kind != "history")
    std::stable_sort(values.begin(), values.end(),
                     [](const auto &a, const auto &b) {
                       return a.metadata.value("order").toDouble(1e15) <
                              b.metadata.value("order").toDouble(1e15);
                     });
  return values;
}
LibraryItem LibraryStore::item(const QString &id) const {
  if(const auto pending=m_pending.constFind(id);pending!=m_pending.cend())return pending->item;
  QSqlQuery query(m_db);
  query.prepare(
      "SELECT id,kind,path,title,metadata,state FROM items WHERE id=?");
  query.addBindValue(id);
  return query.exec() && query.next() ? read(query) : LibraryItem{};
}
QString LibraryStore::add(const QString &kind, const QString &path,
                          const QString &title) {
  for(const auto &pending:m_pending)if(pending.item.kind==kind && pending.item.path==path)return pending.item.id;
  QSqlQuery existing(m_db);
  existing.prepare("SELECT id FROM items WHERE kind=? AND path=?");
  existing.addBindValue(kind);
  existing.addBindValue(path);
  if (existing.exec() && existing.next())
    return existing.value(0).toString();
  const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if(m_deferred) {
    const auto name=title.isEmpty()?bookDisplayName(path):title;
    m_pending[id]={{id,kind,path,name.isEmpty()?path:name,{},{}},QDateTime::currentMSecsSinceEpoch(),++m_revision};
    return id;
  }
  QSqlQuery query(m_db);
  query.prepare(
      "INSERT INTO items(id,kind,path,title,modified) VALUES(?,?,?,?,?)");
  query.addBindValue(id);
  query.addBindValue(kind);
  query.addBindValue(path);
  query.addBindValue(title.isEmpty() ? (bookDisplayName(path).isEmpty()
                                            ? path
                                            : bookDisplayName(path))
                                     : title);
  query.addBindValue(QDateTime::currentMSecsSinceEpoch());
  return query.exec() ? id : QString{};
}
bool LibraryStore::update(const QString &id, const QJsonObject &metadata,
                          const QJsonObject &state) {
  if(m_deferred) {
    auto value=item(id);if(value.id.isEmpty())return false;
    value.metadata=metadata;value.state=state;
    if(!metadata.value("title").toString().isEmpty())value.title=metadata.value("title").toString();
    m_pending[id]={value,QDateTime::currentMSecsSinceEpoch(),++m_revision};return true;
  }
  QSqlQuery query(m_db);
  query.prepare("UPDATE items SET "
                "metadata=?,state=?,title=COALESCE(NULLIF(?,''),title),"
                "modified=? WHERE id=?");
  query.addBindValue(QString::fromUtf8(
      QJsonDocument(metadata).toJson(QJsonDocument::Compact)));
  query.addBindValue(
      QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact)));
  query.addBindValue(metadata.value("title").toString());
  query.addBindValue(QDateTime::currentMSecsSinceEpoch());
  query.addBindValue(id);
  return query.exec();
}
bool LibraryStore::remove(const QString &id) {
  if(!flushPending())return false;
  QSqlQuery query(m_db);
  query.prepare("DELETE FROM items WHERE id=?");
  query.addBindValue(id);
  return query.exec();
}
bool LibraryStore::clear(const QString &kind) {
  if(!flushPending())return false;
  QSqlQuery query(m_db);
  query.prepare("DELETE FROM items WHERE kind=?");
  query.addBindValue(kind);
  return query.exec();
}

bool LibraryStore::movePath(const QString &before, const QString &after) {
  if(!flushPending())return false;
  if(before==after)return true;
  if(!m_db.transaction())return false;
  QSqlQuery source(m_db);source.prepare("SELECT id,kind,path,title,metadata,state FROM items WHERE path=?");source.addBindValue(before);
  if(!source.exec()){m_db.rollback();return false;}
  QVector<LibraryItem> items;while(source.next())items.append(read(source));source.finish();
  for(const auto &from:items) {
    QJsonObject metadata=from.metadata,state=from.state;
    QSqlQuery destination(m_db);destination.prepare("SELECT id,kind,path,title,metadata,state FROM items WHERE path=? AND kind=?");
    destination.addBindValue(after);destination.addBindValue(from.kind);
    if(!destination.exec()){m_db.rollback();return false;}
    if(destination.next()) {
      const auto target=read(destination);destination.finish();
      metadata=target.metadata;state=target.state;
      for(auto i=from.metadata.begin();i!=from.metadata.end();++i)metadata[i.key()]=i.value();
      for(auto i=from.state.begin();i!=from.state.end();++i)state[i.key()]=i.value();
      QSqlQuery remove(m_db);remove.prepare("DELETE FROM items WHERE id=?");remove.addBindValue(target.id);
      if(!remove.exec()){m_db.rollback();return false;}
    }
    auto title=metadata.value("title").toString();
    if(title.isEmpty())title=from.title==QFileInfo(before).fileName() ? QFileInfo(after).fileName() : from.title;
    QSqlQuery update(m_db);update.prepare("UPDATE items SET path=?,title=?,metadata=?,state=?,modified=? WHERE id=?");
    update.addBindValue(after);update.addBindValue(title);
    update.addBindValue(QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact)));
    update.addBindValue(QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact)));
    update.addBindValue(QDateTime::currentMSecsSinceEpoch());update.addBindValue(from.id);
    if(!update.exec()){m_db.rollback();return false;}
  }
  if(m_db.commit())return true;
  m_db.rollback();return false;
}

QVector<LibraryStore::Pending> LibraryStore::pendingWrites() const {
    QVector<Pending> writes;for(const auto &value:m_pending)writes.append(value);
    std::sort(writes.begin(),writes.end(),[](const auto &a,const auto &b){return a.revision<b.revision;});return writes;
}
bool LibraryStore::writePending(const QString &path,const QVector<Pending> &writes,const std::function<bool()> &cancel) {
    if(writes.isEmpty())return true;
    if(cancel && cancel())return false;
    LibraryStore store(path);
    if(!store.error().isEmpty() || !store.m_db.transaction())return false;
    for(const auto &pending:writes) {
        if(cancel && cancel()){store.m_db.rollback();return false;}
        const auto &item=pending.item;
        QSqlQuery query(store.m_db);
        query.prepare("INSERT INTO items(id,kind,path,title,metadata,state,modified) VALUES(?,?,?,?,?,?,?) "
            "ON CONFLICT(id) DO UPDATE SET kind=excluded.kind,path=excluded.path,title=excluded.title,"
            "metadata=excluded.metadata,state=excluded.state,modified=excluded.modified");
        query.addBindValue(item.id);query.addBindValue(item.kind);query.addBindValue(item.path);query.addBindValue(item.title);
        query.addBindValue(QString::fromUtf8(QJsonDocument(item.metadata).toJson(QJsonDocument::Compact)));
        query.addBindValue(QString::fromUtf8(QJsonDocument(item.state).toJson(QJsonDocument::Compact)));query.addBindValue(pending.modified);
        if(!query.exec()){store.m_db.rollback();return false;}
    }
    if(cancel && cancel()){store.m_db.rollback();return false;}
    return store.m_db.commit();
}
void LibraryStore::acknowledge(const QVector<Pending> &writes) {
    for(const auto &write:writes)if(const auto found=m_pending.constFind(write.item.id);found!=m_pending.cend() && found->revision==write.revision)m_pending.remove(write.item.id);
}
bool LibraryStore::flushPending() {
    // Reject queued older snapshots before destructive/explicit DB operations.
    m_writeEpoch->fetch_add(1);
    const auto writes=pendingWrites();
    if(writes.isEmpty())return true;
    if(!writePending(m_db.databaseName(),writes))return false;
    acknowledge(writes);return true;
}
