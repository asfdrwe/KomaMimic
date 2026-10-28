// SPDX-License-Identifier: GPL-3.0-only
#include "scriptreader.h"
#include "i18n.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QReadWriteLock>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

static void initializeReaderResources() {
    static const bool initialized=[] { Q_INIT_RESOURCE(readers); return true; }();
    (void)initialized;
}
namespace {
constexpr qsizetype MaxScriptBytes = 256 * 1024;
constexpr qsizetype MaxHeaderBytes = 1024 * 1024;
struct Reader {
    QString id, sourceName, kind = "reader"; int apiVersion=1; bool multipage=false; QVariantMap schema;
    QStringList extensions;
    QByteArray magic, source;
};
QString readPackage(const QString &directory, Reader &reader) {
    QFile manifest(directory + "/manifest.json");
    if (!manifest.open(QIODevice::ReadOnly)) return manifest.errorString();
    if (manifest.size() > 16384) return QCoreApplication::translate("KomaMimic", "スクリプトmanifestが大きすぎます。");
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(manifest.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return QCoreApplication::translate("KomaMimic", "不正なスクリプトmanifestです。");
    const auto object = document.object();
    reader.id = object.value("id").toString();
    reader.apiVersion=object.value("apiVersion").toInt();
    reader.kind=object.value("kind").toString("reader");
    if ((object.contains("multipage") && !object.value("multipage").isBool()) ||
        (object.contains("extensions") && !object.value("extensions").isArray()))
      return QCoreApplication::translate("KomaMimic", "不正なスクリプトmanifestです。");
    reader.multipage=object.value("multipage").toBool();
    if (object.contains("settings") && !object.value("settings").isObject())
      return QCoreApplication::translate("KomaMimic", "未対応の設定schemaです。");
    reader.schema=object.value("settings").toObject().toVariantMap();
    if(reader.kind=="filter" && (reader.id=="0" || reader.id=="1")) return QCoreApplication::translate("KomaMimic", "filter ID 0/1は旧開発設定用の予約値です。");
    if(reader.schema.size()>32)return QCoreApplication::translate("KomaMimic", "スクリプト設定は32項目までです。");
    for(auto i=reader.schema.cbegin();i!=reader.schema.cend();++i) {
      const auto spec=object.value("settings").toObject().value(i.key());
      if (!spec.isObject()) return QCoreApplication::translate("KomaMimic", "未対応の設定schemaです。");
      const auto field=i.value().toMap();
      if(!QRegularExpression("^[a-zA-Z][a-zA-Z0-9_]{0,31}$").match(i.key()).hasMatch() ||
         !QStringList{"boolean","integer","number"}.contains(field.value("type").toString())) return QCoreApplication::translate("KomaMimic", "未対応の設定schemaです。");
      if(field.value("type")=="boolean") {
        if (spec.toObject().contains("default") && !spec.toObject().value("default").isBool())
          return QCoreApplication::translate("KomaMimic", "真偽値設定schemaには真偽値を指定してください。");
      } else {
        const auto spec=object.value("settings").toObject().value(i.key()).toObject();
        for(const auto *name:{"minimum","maximum","default"}) if(spec.contains(name) && !spec.value(name).isDouble())return QCoreApplication::translate("KomaMimic", "数値設定schemaには数値を指定してください。");
        const double minimum=field.value("minimum",-1000000).toDouble(), maximum=field.value("maximum",1000000).toDouble();
        if(!std::isfinite(minimum) || !std::isfinite(maximum) || minimum>maximum || minimum< -1000000 || maximum>1000000)return QCoreApplication::translate("KomaMimic", "設定schemaの範囲が不正です。");
        const double value=field.value("default", minimum).toDouble();
        if (!std::isfinite(value) || value<minimum || value>maximum ||
            (field.value("type")=="integer" && (std::floor(minimum)!=minimum || std::floor(maximum)!=maximum || std::floor(value)!=value)))
          return QCoreApplication::translate("KomaMimic", "設定schemaの既定値または整数範囲が不正です。");
      }
    }
    const auto script = object.value("script").toString();
    const auto magic = object.value("magic").toString();
    if (object.value("apiVersion")!=QJsonValue(1) || !QStringList{"reader","filter"}.contains(reader.kind) || !QRegularExpression("^[A-Za-z0-9_.-]{1,64}$").match(reader.id).hasMatch()
        || !QRegularExpression("^[A-Za-z0-9_-]+\\.js$").match(script).hasMatch()
        || (reader.kind=="reader" && !QRegularExpression("^(?:[0-9a-fA-F]{2}){1,32}$").match(magic).hasMatch()))
        return QCoreApplication::translate("KomaMimic", "未対応のスクリプトAPI、ID、script名、またはmagicです。");
    reader.magic = QByteArray::fromHex(magic.toLatin1());
    for (const auto &extension : object.value("extensions").toArray()) {
        const auto value = extension.toString();
        if (!QRegularExpression("^[a-z0-9]{1,12}$").match(value).hasMatch()) return QCoreApplication::translate("KomaMimic", "不正なスクリプト拡張子です。");
        if (!reader.extensions.contains(value)) reader.extensions.append(value);
    }
    if (reader.kind=="reader" && reader.extensions.isEmpty()) return QCoreApplication::translate("KomaMimic", "スクリプト拡張子がありません。");
    reader.sourceName = directory + '/' + script;
    if (!directory.startsWith(":/")) {
        const auto root = QDir(directory).canonicalPath();
        const auto path = QFileInfo(reader.sourceName).canonicalFilePath();
        if (root.isEmpty() || QFileInfo(reader.sourceName).isSymLink() || QFileInfo(path).absolutePath() != root)
            return QCoreApplication::translate("KomaMimic", "スクリプトはpackage直下の通常ファイルを指定してください。");
    }
    QFile file(reader.sourceName);
    if (!file.open(QIODevice::ReadOnly)) return file.errorString();
    reader.source = file.read(MaxScriptBytes + 1);
    if (reader.source.isEmpty() || reader.source.size() > MaxScriptBytes) return QCoreApplication::translate("KomaMimic", "スクリプトのサイズが範囲外です。");
    if (QString::fromUtf8(reader.source).toUtf8() != reader.source) return QCoreApplication::translate("KomaMimic", "スクリプトはUTF-8で保存してください。");
    return {};
}
QVector<Reader> bundledReaders() {
    initializeReaderResources();
    QVector<Reader> readers;
    for (const auto *name : {"mag", "pic2", "pic", "pnm", "reading-filter"}) {
        Reader reader;
        const auto error = readPackage(QString(":/komamimic/readers/") + name, reader);
        if (!error.isEmpty()) qFatal("Invalid bundled script reader: %s", qPrintable(error));
        readers.append(reader);
    }
    return readers;
}
struct Registry {
    QReadWriteLock lock;
    QVector<Reader> readers = bundledReaders();
};
Registry &registry() { static Registry value; return value; }
}
QStringList scriptImageExtensions() {
    auto &r = registry(); QReadLocker lock(&r.lock); QStringList result;
    // A non-const Qt container iterator can detach implicitly shared storage.
    // Read locks permit simultaneous readers, so iteration must stay const.
    for (const auto &reader : std::as_const(r.readers))
        for (const auto &extension : reader.extensions) if (!result.contains(extension)) result.append(extension);
    return result;
}
namespace {
QString appendDirectory(const QString &directory, QVector<Reader> &updated) {
    QDir dir(directory);
    if (!dir.exists()) return QCoreApplication::translate("KomaMimic", "スクリプトdirectoryがありません: ") + directory;
    QVector<Reader> pending;
    QStringList packages;
    if (QFileInfo::exists(dir.filePath("manifest.json"))) packages.append(dir.absolutePath());
    else for (const auto &info : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (info.isSymLink()) continue;
        if (QFileInfo::exists(info.absoluteFilePath() + "/manifest.json")) packages.append(info.absoluteFilePath());
    }
    if (packages.isEmpty() || packages.size() > 32) return QCoreApplication::translate("KomaMimic", "スクリプトpackage数が範囲外です（1〜32）。");
    QSet<QString> ids;
    for (const auto &path : packages) {
        Reader reader; const auto error = readPackage(path, reader);
        if (!error.isEmpty()) return path + ": " + error;
        if (ids.contains(reader.id)) return QCoreApplication::translate("KomaMimic", "スクリプトIDが重複しています: ") + reader.id;
        ids.insert(reader.id);
        pending.append(reader);
    }
    for (const auto &reader : pending) {
        updated.erase(std::remove_if(updated.begin(), updated.end(), [&](const Reader &old) { return old.id == reader.id; }), updated.end());
        for (const auto &old : std::as_const(updated)) {
            if (reader.kind!="reader" || old.kind!="reader") continue;
            if (old.magic == reader.magic) return QCoreApplication::translate("KomaMimic", "スクリプト形式が重複しています: ") + reader.id;
            for (const auto &extension : reader.extensions)
                if (old.extensions.contains(extension)) return QCoreApplication::translate("KomaMimic", "スクリプト形式が重複しています: ") + extension;
        }
        updated.prepend(reader);
    }
    if (updated.size() > 32) return QCoreApplication::translate("KomaMimic", "登録スクリプトが32件を超えています。");
    return {};
}
QString configuredReaders(const QStringList &directories, QVector<Reader> &readers) {
    if (directories.size()>32) return QCoreApplication::translate("KomaMimic", "登録フォルダは32件までです。");
    readers=bundledReaders();
    for (const auto &directory : directories) {
        const auto error=appendDirectory(directory, readers);
        if (!error.isEmpty()) return error;
    }
    return {};
}
}
QString registerScriptReaders(const QString &directory) {
    auto &r=registry(); QWriteLocker lock(&r.lock); auto updated=r.readers;
    const auto error=appendDirectory(directory, updated);
    if (!error.isEmpty()) return error;
    r.readers=std::move(updated); return {};
}
QString configureScriptReaders(const QStringList &directories) {
    QVector<Reader> updated;
    const auto error=configuredReaders(directories, updated);
    if (!error.isEmpty()) return error;
    auto &r=registry(); QWriteLocker lock(&r.lock); r.readers=std::move(updated); return {};
}
QString inspectScriptReaders(const QStringList &directories, QVariantList &packages) {
    QVector<Reader> readers;
    const auto error=configuredReaders(directories, readers);
    if (!error.isEmpty()) return error;
    QVariantList result;
    for (const auto &reader : std::as_const(readers))
        result.append(QVariantMap{{"id",reader.id},{"kind",reader.kind}, {"apiVersion",1},
            {"multipage",reader.multipage},{"extensions",reader.extensions},
            {"path",reader.sourceName},{"settings",reader.schema}});
    packages=std::move(result); return {};
}
static ImageResult executeScript(const QByteArray &source, const QString &scriptName,
                          const QByteArray &bytes, QSize thumbnail, int timeoutMs,
                          const std::function<bool()> &cancel, int apiVersion=1, int page=0,
                          const QVariantMap &settings={}, const QString &operation="decode", QSize inputSize={}, int *pageCount=nullptr) {
    if (source.isEmpty() || source.size() > MaxScriptBytes || bytes.size() > (operation=="filter" ? MaxPixels*4 : MaxEncodedBytes)
        || timeoutMs < 1 || timeoutMs > 60000) return {{}, QCoreApplication::translate("KomaMimic", "スクリプト入力のサイズ・期限が範囲外です。")};
    if (cancel && cancel()) return {{}, QCoreApplication::translate("KomaMimic", "スクリプト読込を取り消しました。")};
#ifdef Q_OS_WIN
    const auto worker = QCoreApplication::applicationDirPath() + "/komamimic-script-worker.exe";
#else
    const auto worker = QCoreApplication::applicationDirPath() + "/komamimic-script-worker";
#endif
    if (!QFileInfo(worker).isExecutable()) return {{}, QCoreApplication::translate("KomaMimic", "スクリプトworkerが見つかりません: ") + worker};
    const auto header = QJsonDocument(QJsonObject{{"apiVersion",apiVersion},{"operation",operation},{"pageIndex",page},
        {"settings",QJsonObject::fromVariantMap(settings)},{"width",inputSize.width()},{"height",inputSize.height()},{"source",QString::fromUtf8(source)},
        {"name",scriptName},{"timeoutMs",std::max(1, timeoutMs - 250)}}).toJson(QJsonDocument::Compact);
    if (header.size() > MaxHeaderBytes) return {{}, QCoreApplication::translate("KomaMimic", "スクリプトrequestが大きすぎます。")};
    QByteArray request(8, char(0));
    qToBigEndian<quint32>(quint32(header.size()), request.data());
    qToBigEndian<quint32>(quint32(bytes.size()), request.data()+4);
    request += header; request += bytes;
    QElapsedTimer clock; clock.start();
    QProcess process; process.setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    process.start(worker, QStringList{I18n::currentLanguage()});
    if (!process.waitForStarted(std::min(timeoutMs, 2000))) return {{}, QCoreApplication::translate("KomaMimic", "スクリプトworkerを起動できません: ") + process.errorString()};
    if (process.write(request) != request.size()) { process.kill(); process.waitForFinished(); return {{}, QCoreApplication::translate("KomaMimic", "スクリプト入力を送信できません。")}; }
    process.closeWriteChannel();
    QByteArray output, diagnostic;
    const auto collect = [&] {
        output += process.readAllStandardOutput();
        const auto more = process.readAllStandardError();
        if (diagnostic.size() < 8192) diagnostic += more.left(8192-diagnostic.size());
    };
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(25); collect();
        const bool cancelled = cancel && cancel();
        if (cancelled || clock.elapsed() > timeoutMs || output.size() > MaxPixels * 4 + 65544) {
            process.kill(); process.waitForFinished();
            return {{}, cancelled ? QCoreApplication::translate("KomaMimic", "スクリプト読込を取り消しました。") : QCoreApplication::translate("KomaMimic", "スクリプトの時間・出力上限を超えました。")};
        }
    }
    collect();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {{}, QCoreApplication::translate("KomaMimic", "スクリプトworkerが異常終了しました: ") + QString::fromUtf8(diagnostic)};
    if (output.size() < 8) return {{}, QCoreApplication::translate("KomaMimic", "スクリプトworkerの応答がありません。")};
    const auto headerSize = qFromBigEndian<quint32>(output.constData());
    const auto pixelSize = qFromBigEndian<quint32>(output.constData()+4);
    if (headerSize > 65536 || quint64(headerSize)+pixelSize+8 != quint64(output.size())) return {{}, QCoreApplication::translate("KomaMimic", "スクリプトworkerの応答サイズが不正です。")};
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(output.mid(8,headerSize), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return {{}, QCoreApplication::translate("KomaMimic", "スクリプトworkerの応答が不正です。")};
    const auto object = document.object();
    const auto error = object.value("error").toString();
    if (!error.isEmpty()) return {{}, error};
    if(operation=="inspect") {
      const double count=object.value("pages").toDouble();
      if(!pageCount || pixelSize!=0 || count<1 || count>10000 || count!=int(count)) return {{},QCoreApplication::translate("KomaMimic", "スクリプトのページ数が範囲外です。")};
      *pageCount=int(count); return {};
    }
    const auto width = object.value("width").toDouble(), height = object.value("height").toDouble();
    if (width < 1 || height < 1 || width > MaxPixels || height > MaxPixels
        || width != int(width) || height != int(height) || width*height > MaxPixels
        || width*height*4 != pixelSize) return {{}, QCoreApplication::translate("KomaMimic", "スクリプト画像の寸法・byte数が不正です。")};
    const auto *data = reinterpret_cast<const uchar *>(output.constData()+8+headerSize);
    QImage image(data, int(width), int(height), int(width)*4, QImage::Format_RGBA8888);
    image = image.copy();
    if (image.isNull()) return {{}, QCoreApplication::translate("KomaMimic", "スクリプト画像のメモリを確保できません。")};
    if (thumbnail.isValid()) image = image.scaled(thumbnail, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    image.setDevicePixelRatio(1); return {image,{}};
}

ImageResult runImageScript(const QByteArray &source, const QString &name,const QByteArray &bytes,QSize thumbnail,int timeout,const std::function<bool()> &cancel) {
  return executeScript(source,name,bytes,thumbnail,timeout,cancel);
}
namespace {
QVector<Reader> readersSnapshot() {auto &r=registry();QReadLocker lock(&r.lock);return r.readers;}
QVariantMap validatedSettings(const Reader &r,const QVariantMap &values) {
  QVariantMap result;
  for(auto i=r.schema.cbegin();i!=r.schema.cend();++i) {
    const auto field=i.value().toMap();
    auto v=values.value(i.key(),field.value("default"));
    if(field.value("type")=="boolean") v=v.toBool();
    else {
      double n=v.toDouble(), minimum=field.value("minimum",-1000000).toDouble(),maximum=field.value("maximum",1000000).toDouble();
      if(!std::isfinite(minimum) || !std::isfinite(maximum) || std::abs(minimum)>1000000 || std::abs(maximum)>1000000) {minimum=-1000000;maximum=1000000;}
      if(!std::isfinite(n))n=0;
      n=std::clamp(n,std::min(minimum,maximum),std::max(minimum,maximum));
      v=field.value("type")=="integer" ? QVariant(int(n)) : QVariant(n);
    }
    result[i.key()]=v;
  }
  return result;
}
}
QByteArray scriptReaderFingerprint() {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  const auto readers=readersSnapshot();
  for(const auto &r:readers) {
    for(const auto &part:{r.id.toUtf8(),r.kind.toUtf8(),QByteArray::number(r.apiVersion),QByteArray::number(int(r.multipage)),
        r.extensions.join(',').toUtf8(),r.magic,r.source,QJsonDocument(QJsonObject::fromVariantMap(r.schema)).toJson(QJsonDocument::Compact)}) {
      hash.addData(QByteArray::number(part.size()));hash.addData(":");hash.addData(part);
    }
  }
  return hash.result();
}
ImageResult decodeScriptImage(const QByteArray &bytes,const QString &name,QSize thumbnail,bool &recognized,bool matchSuffix,
                              int page,const QVariantMap &settings,const std::function<bool()> &cancel) {
  const auto readers=readersSnapshot();recognized=false;
  const Reader *selected=nullptr;
  for(const auto &r:readers)if(r.kind=="reader" && bytes.startsWith(r.magic)){selected=&r;break;}
  if(!selected && matchSuffix)for(const auto &r:readers)if(r.kind=="reader" && r.extensions.contains(QFileInfo(name).suffix().toLower())){selected=&r;break;}
  if(!selected)return {};
  recognized=true; const auto &r=*selected;
  if(page<0 || page>=10000 || (page>0 && !r.multipage))return {{},QCoreApplication::translate("KomaMimic", "スクリプトのページが範囲外です。")};
  return executeScript(r.source,r.sourceName,bytes,thumbnail,10000,cancel,r.apiVersion,page,validatedSettings(r,settings.value(r.id).toMap()));
}
int scriptPageCount(const QByteArray &bytes,const QString &,QString &error,const QVariantMap &settings,const std::function<bool()> &cancel) {
  for(const auto &r:readersSnapshot())if(r.kind=="reader" && r.multipage && bytes.startsWith(r.magic)) {
    int count=0;const auto result=executeScript(r.source,r.sourceName,bytes,{},10000,cancel,r.apiVersion,0,validatedSettings(r,settings.value(r.id).toMap()),"inspect",{},&count);
    error=result.error;return error.isEmpty()?count:-1;
  }
  return 0;
}
QVariantMap scriptSettingsSchemas(){QVariantMap result;for(const auto &r:readersSnapshot())if(!r.schema.isEmpty())result[r.id]=r.schema;return result;}
QStringList scriptFilterIds(){QStringList result;for(const auto &r:readersSnapshot())if(r.kind=="filter")result.append(r.id);return result;}
ImageResult applyScriptFilter(const QString &id,const QImage &image,const QVariantMap &settings,const std::function<bool()> &cancel) {
  if(image.isNull() || qint64(image.width())*image.height()>MaxPixels)return {{},QCoreApplication::translate("KomaMimic", "フィルター入力の寸法が不正です。")};
  for(const auto &r:readersSnapshot())if(r.kind=="filter" && r.id==id) {
    const auto rgba=image.convertToFormat(QImage::Format_RGBA8888);
    return executeScript(r.source,r.sourceName,QByteArray(reinterpret_cast<const char*>(rgba.constBits()),rgba.sizeInBytes()),{},10000,cancel,1,0,validatedSettings(r,settings.value(r.id).toMap()),"filter",rgba.size());
  }
  return {{},QCoreApplication::translate("KomaMimic", "スクリプトフィルターが登録されていません：")+id};
}

QStringList scriptMultipageExtensions(){QStringList result;for(const auto &r:readersSnapshot())if(r.multipage)result+=r.extensions;return result;}

QString configuredScriptFilter(const QVariant &value) {
  const auto id=value.toString();
  if(id.isEmpty() || id=="0")return {};
  if(id=="1")return "reading-filter"; // early v0.9 development setting
  return id; // Keep unavailable IDs explicit so processing reports the missing package.
}
