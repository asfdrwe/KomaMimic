// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "media.h"
#include <functional>
#include <QVariantMap>
#include <QJsonObject>

// Registration reads metadata/source only; JavaScript runs exclusively in the worker.
QStringList scriptImageExtensions();
QByteArray scriptReaderFingerprint();
QString registerScriptReaders(const QString &directory);
// Build/validate a complete registry without executing scripts. Preview never
// changes the active registry; configuration replaces it atomically on success.
QString inspectScriptReaders(const QStringList &directories, QVariantList &packages);
QString configureScriptReaders(const QStringList &directories);
ImageResult decodeScriptImage(const QByteArray &bytes, const QString &name,
                              QSize thumbnail, bool &recognized, bool matchSuffix = true,
                              int page = 0, const QVariantMap &settings = {},
                              const std::function<bool()> &cancel = {});
ImageResult runImageScript(const QByteArray &source, const QString &scriptName,
                          const QByteArray &bytes, QSize thumbnail = {},
                          int timeoutMs = 10000,
                          const std::function<bool()> &cancel = {});

// Zero means unrecognized; -1 means a recognized reader failed inspection.
int scriptPageCount(const QByteArray &bytes, const QString &name, QString &error,
                    const QVariantMap &settings = {}, const std::function<bool()> &cancel = {});
QVariantMap scriptSettingsSchemas();
QStringList scriptFilterIds();
ImageResult applyScriptFilter(const QString &id, const QImage &image,
                              const QVariantMap &settings = {}, const std::function<bool()> &cancel = {});

QStringList scriptMultipageExtensions();

// Persist the filter ID, independent of registry/UI ordering.
QString configuredScriptFilter(const QVariant &value);
