// SPDX-License-Identifier: GPL-3.0-only
#include "mainwindow.h"
#include <QCoreApplication>
#include "i18n.h"
#include "shortcuts.h"
#include "scriptreader.h"
#include "documentview.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QSettings>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTextStream>
#include <QThreadPool>
#include <QThread>
#include <QTimer>
#include <cstring>
#include <memory>
#include <tuple>
#include <array>
#include <algorithm>
#ifdef Q_OS_WIN
#include <windows.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif

namespace {
#ifdef Q_OS_WIN
void connectParentConsole() {
  // A GUI executable never allocates a console. Keep inherited pipe/file
  // handles for CLI automation; otherwise use a terminal that already exists.
  const auto valid = [](HANDLE h) { return h && h!=INVALID_HANDLE_VALUE; };
  struct Channel { DWORD id; FILE *stream; HANDLE inherited=nullptr; };
  std::array<Channel,2> channels{{{STD_OUTPUT_HANDLE,stdout},{STD_ERROR_HANDLE,stderr}}};
  bool missing=false;
  for(auto &channel:channels) {
    const auto handle=GetStdHandle(channel.id);
    if(valid(handle))DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&channel.inherited,0,FALSE,DUPLICATE_SAME_ACCESS);
    else missing=true;
  }
  if(missing)AttachConsole(ATTACH_PARENT_PROCESS);
  for(auto &channel:channels) {
    HANDLE copy=channel.inherited;
    if(!copy) {
      const auto handle=GetStdHandle(channel.id);if(!valid(handle))continue;
      if(!DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&copy,0,FALSE,DUPLICATE_SAME_ACCESS))continue;
    }
    const int descriptor=_open_osfhandle(reinterpret_cast<intptr_t>(copy),_O_WRONLY|_O_BINARY);
    if(descriptor<0){CloseHandle(copy);continue;}
    if(_fileno(channel.stream)<0) {
#ifdef _MSC_VER
      FILE *reopened=nullptr;
      const bool failed=freopen_s(&reopened,"NUL","wb",channel.stream)!=0;
#else
      const bool failed=std::freopen("NUL","wb",channel.stream)==nullptr;
#endif
      if(failed){_close(descriptor);continue;}
    }
    const int target=_fileno(channel.stream);
    if(descriptor!=target){_dup2(descriptor,target);_close(descriptor);}
    SetStdHandle(channel.id,reinterpret_cast<HANDLE>(_get_osfhandle(target)));
    clearerr(channel.stream);
  }
}
#endif
QString legacySettingsPath() {
  // Ask Qt for the old platform-specific path before creating any windows.
  const auto organization = QCoreApplication::organizationName();
  const auto application = QCoreApplication::applicationName();
  QCoreApplication::setOrganizationName("KomaMimic");
  QCoreApplication::setApplicationName("KomaMimic");
  const auto path =
      QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
      "/settings.ini";
  QCoreApplication::setOrganizationName(organization);
  QCoreApplication::setApplicationName(application);
  return path;
}
} // namespace

int main(int argc, char **argv) {
#ifdef Q_OS_WIN
  qputenv("QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES","1");
  connectParentConsole();
#endif
  DocumentView::registerScheme();
  bool textOnly = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--") == 0)
      break;
    if (std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0 ||
        std::strcmp(argv[i], "--help-all") == 0 ||
        std::strcmp(argv[i], "--version") == 0 ||
        std::strcmp(argv[i], "-v") == 0 ||
        std::strcmp(argv[i], "--formats") == 0 ||
        std::strcmp(argv[i], "--commands") == 0)
      textOnly = true;
  }
  std::unique_ptr<QCoreApplication> app;
  if (textOnly)
    app = std::make_unique<QCoreApplication>(argc, argv);
  else
    app = std::make_unique<QApplication>(argc, argv);
  QCoreApplication::setOrganizationName("KomaMimic");
  QCoreApplication::setApplicationName("KomaMimic");
  if (!textOnly) {
    QApplication::setApplicationDisplayName("KomaMimic");
    QApplication::setDesktopFileName("komamimic");
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256, 512})
      icon.addFile(QString(":/icons/komamimic-%1.png").arg(size), QSize(size, size));
    QApplication::setWindowIcon(icon);
  }
  QCoreApplication::setApplicationVersion(KOMAMIMIC_VERSION);
  { QSettings languageSettings(I18n::startupConfigPath(QCoreApplication::arguments()), QSettings::IniFormat);
    I18n::initialize(I18n::savedLanguage(languageSettings)); }
  QCommandLineParser parser;
  parser.setApplicationDescription(
      QCoreApplication::translate("KomaMimic", "KomaMimic — 画像・書庫を読むクロスプラットフォームビューア (GPLv3)"));
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addOption({{"config", "i"}, QCoreApplication::translate("KomaMimic", "設定ファイルを指定"), "file"});
  parser.addOption({{"fullscreen", "f"}, QCoreApplication::translate("KomaMimic", "全画面で起動")});
  parser.addOption({"formats", QCoreApplication::translate("KomaMimic", "利用できる画像・書庫形式を表示して終了")});
  parser.addOption({"commands", QCoreApplication::translate("KomaMimic", "コマンドIDの一覧を表示して終了")});
  parser.addOption({"new-window", QCoreApplication::translate("KomaMimic", "単一起動設定を使わず別プロセスで起動")});
  parser.addOption({"script-readers", QCoreApplication::translate("KomaMimic", "追加・置換する画像読込スクリプトのフォルダ（複数指定可、別プロセス起動）"), "directory"});
  parser.addOption({{"last", "l"}, QCoreApplication::translate("KomaMimic", "最後に開いた本を復元")});
  parser.addOption(
      {{"page-mode", "p"}, QCoreApplication::translate("KomaMimic", "0:自動見開き / 1:単頁 / 2:見開き"), "mode"});
  parser.addOption({{"entry", "e"},
                    QCoreApplication::translate("KomaMimic", "0:しおり / 1:先頭 / 2:マーカー / 3:画像一覧 / 4:表紙一覧"),
                    "mode"});
  parser.addOption({{"add", "a"}, QCoreApplication::translate("KomaMimic", "プレイリストへ追加"), "path"});
  parser.addOption({"page", QCoreApplication::translate("KomaMimic", "開始ページ（1始まり）"), "number"});
  parser.addOption({"execute",
                    QCoreApplication::translate("KomaMimic", "読込後にコマンドIDまたは旧番号1〜175を実行（複数指定可）"),
                    "id"});
  parser.addPositionalArgument("path", QCoreApplication::translate("KomaMimic", "画像・書庫または表示するフォルダ"),
                               "[path]");
  parser.process(*app);
  {
    // Saved registrations are also available to --formats and --commands.
    QSettings saved(I18n::startupConfigPath(QCoreApplication::arguments()), QSettings::IniFormat);
    const auto error=configureScriptReaders(saved.value("scripts/directories").toStringList());
    if (!error.isEmpty()) {
      if (textOnly) { QTextStream(stderr) << error << '\n'; return 2; }
      // Recover through Preferences when a package was removed or is invalid.
      // The complete saved registry is rejected, leaving bundled readers active.
      app->setProperty("scriptRegistrationError",error);
    }
  }
  app->setProperty("scriptCommandLineDirectories",parser.values("script-readers"));
  for (const auto &directory : parser.values("script-readers")) {
    const auto error = registerScriptReaders(directory);
    if (!error.isEmpty()) { QTextStream(stderr) << error << '\n'; return 2; }
  }
  if (parser.positionalArguments().size() > 1)
    parser.showHelp(2);
  for (const auto &id : parser.values("execute")) {
    if (QStringList{"documentInherited", "documentStandard", "documentExtended"}.contains(id)) {
      QTextStream(stderr) << QCoreApplication::translate("KomaMimic", "実行できないコマンド: ") << id << '\n';
      return 2;
    }
  }
  if (parser.isSet("commands")) {
    QTextStream out(stdout);
    for (const auto &spec : shortcutSpecs())
      out << spec.id << "\t" << spec.label << "\t"
          << spec.defaultKey.toString(QKeySequence::PortableText) << '\n';
    return 0;
  }
  for (const auto &[option, low, high] : QVector<std::tuple<QString, int, int>>{
           {"page-mode", 0, 2}, {"entry", 0, 4}, {"page", 1, 100000}}) {
    if (parser.isSet(option)) {
      bool ok = false;
      const int value = parser.value(option).toInt(&ok);
      if (!ok || value < low || value > high) {
        QTextStream(stderr) << QCoreApplication::translate("KomaMimic", "不正な引数 --") << option << '\n';
        return 2;
      }
    }
  }
  if (parser.isSet("formats")) {
    QTextStream out(stdout);
    out << QCoreApplication::translate("KomaMimic", "画像: ") << imageExtensions().join(", ") << '\n'
        << QCoreApplication::translate("KomaMimic", "書庫: ZIP/CBZ, RAR4/5/CBR, 7z/CB7, LHA/LZH/CBL, CAB, tar/CBT, "
           "tar.gz/bz2/xz/zst\n文書: PDF（組み込みPoppler）, "
           "TXT（UTF-8/CP932等）, Markdown（.md/.markdown）, EPUB\n")
        << QCoreApplication::translate("KomaMimic", "文書表示: WebEngine（TXT・Markdown・EPUB共通）\n")
        << QCoreApplication::translate("KomaMimic", "未宣言のZIP/LHA名: "
           "CP932既定。ツールの文字コード設定で変更。パスワードと書庫内書庫は環境設定。分割/"
           "SFXは保証外。\n");
    return 0;
  }
  const QString config =
      parser.isSet("config")
          ? QFileInfo(parser.value("config")).absoluteFilePath()
          : QStandardPaths::writableLocation(
                QStandardPaths::AppConfigLocation) +
                "/settings.ini";
  if (!QDir().mkpath(QFileInfo(config).absolutePath())) {
    QTextStream(stderr) << QCoreApplication::translate("KomaMimic", "設定フォルダを作成できません: ")
                        << QFileInfo(config).absolutePath() << '\n';
    return 1;
  }
  if (!parser.isSet("config") && !QFileInfo::exists(config)) {
    const auto legacy = legacySettingsPath();
    if (QFileInfo::exists(legacy) && !QFile::copy(legacy, config)) {
      QTextStream(stderr) << QCoreApplication::translate("KomaMimic", "以前の設定を引き継げません: ") << legacy << '\n';
      return 1;
    }
  }
  QImageReader::setAllocationLimit(256);
  QThreadPool::globalInstance()->setMaxThreadCount(3);
  QSettings settings(config, QSettings::IniFormat);
  QString path =
      parser.positionalArguments().isEmpty()
          ? QString{}
          : QFileInfo(parser.positionalArguments().first()).absoluteFilePath();
  if (parser.isSet("last"))
    path = settings.value("lastBook").toString();
  QJsonObject request{{"path", path},
                      {"fullscreen", parser.isSet("fullscreen")}};
  if (parser.isSet("page-mode"))
    request["pageMode"] = parser.value("page-mode").toInt();
  if (parser.isSet("entry"))
    request["entry"] = parser.value("entry").toInt();
  else if (parser.isSet("last"))
    request["entry"] = 0;
  if (parser.isSet("page"))
    request["page"] = parser.value("page").toInt();
  if (parser.isSet("add"))
    request["add"] = QFileInfo(parser.value("add")).absoluteFilePath();
  QJsonArray commands;
  for (const auto &id : parser.values("execute"))
    commands.append(id);
  request["commands"] = commands;
  const auto instanceKey = QFileInfo(config).absoluteDir().canonicalPath() +
                           "/" + QFileInfo(config).fileName();
  const QString instanceName =
      "komamimic-" +
      QString::fromLatin1(QCryptographicHash::hash(instanceKey.toUtf8(),
                                                   QCryptographicHash::Sha256)
                              .toHex()
                              .left(32));
  std::unique_ptr<QLockFile> instanceLock;
  QLocalServer server;
  if (!parser.isSet("new-window") && !parser.isSet("script-readers") &&
      settings.value("General/singleInstance", true).toBool()) {
    instanceLock = std::make_unique<QLockFile>(config + ".instance.lock");
    instanceLock->setStaleLockTime(0);
    if (!instanceLock->tryLock(0)) {
      QLocalSocket socket;
      QElapsedTimer connecting;
      connecting.start();
      bool connected = false;
      do {
        socket.abort();
        socket.connectToServer(instanceName);
        connected = socket.state() == QLocalSocket::ConnectedState ||
                    socket.waitForConnected(std::max(1, 2000 - int(connecting.elapsed())));
        if (connected || (socket.error() != QLocalSocket::ServerNotFoundError &&
                          socket.error() != QLocalSocket::ConnectionRefusedError))
          break;
        // The lock is acquired before the primary publishes its server.
        // Windows waitForConnected() only checks the current state, so retry
        // transient failures explicitly within the same startup deadline.
        if (connecting.elapsed() >= 2000) break;
        QThread::msleep(25);
      } while (connecting.elapsed() < 2000);
      if (!connected) {
        QTextStream(stderr) << QCoreApplication::translate("KomaMimic", "既存ウィンドウへ接続できません。--new-window "
                               "で別起動できます。\n") << socket.errorString() << '\n';
        return 1;
      }
      const auto message =
          QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
      socket.write(message);
      if (socket.bytesToWrite())
        socket.waitForBytesWritten(2000);
      if (!socket.bytesAvailable())
        socket.waitForReadyRead(2000);
      if (socket.readAll().trimmed() != "ok") {
        QTextStream(stderr) << QCoreApplication::translate("KomaMimic", "既存ウィンドウへ要求を送信できませんでした。\n");
        return 1;
      }
      return 0;
    }
    QLocalServer::removeServer(instanceName);
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(instanceName)) {
      QTextStream(stderr)
          << QCoreApplication::translate("KomaMimic", "単一起動のIPCを初期化できないため、単独で起動します: ")
          << server.errorString() << '\n';
      instanceLock.reset();
    }
  }
  MainWindow window(config);
  window.show();
  const auto scriptError=app->property("scriptRegistrationError").toString();
  if(!scriptError.isEmpty())window.statusBar()->showMessage(scriptError,15000);
  QObject::connect(&server, &QLocalServer::newConnection, &window, [&] {
    while (server.hasPendingConnections()) {
      auto *socket = server.nextPendingConnection();
      auto bytes = std::make_shared<QByteArray>();
      QObject::connect(socket, &QLocalSocket::disconnected, socket,
                       &QObject::deleteLater);
      QObject::connect(socket, &QLocalSocket::readyRead, &window,
                       [&, socket, bytes] {
                         *bytes += socket->readAll();
                         if (bytes->size() > 65536) {
                           socket->disconnectFromServer();
                           return;
                         }
                         if (!bytes->contains('\n'))
                           return;
                         QJsonParseError error;
                         const auto document = QJsonDocument::fromJson(
                             bytes->left(bytes->indexOf('\n')), &error);
                         if (error.error != QJsonParseError::NoError ||
                             !document.isObject()) {
                           socket->write("error\n");
                           socket->disconnectFromServer();
                           return;
                         }
                         socket->write("ok\n");
                         socket->flush();
                         socket->disconnectFromServer();
                         window.show();
                         window.raise();
                         window.activateWindow();
                         window.runRequest(document.object());
                       });
      QTimer::singleShot(5000, socket, &QLocalSocket::disconnectFromServer);
    }
  });
  if (path.isEmpty() && !parser.isSet("execute") && !parser.isSet("add"))
    request["path"] =
        settings.value("lastDirectory", QDir::homePath()).toString();
  window.runRequest(request);
  const int result = app->exec();
  QThreadPool::globalInstance()->waitForDone();
  return result;
}
