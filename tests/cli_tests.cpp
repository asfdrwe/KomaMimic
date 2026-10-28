// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "librarystore.h"
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTimer>
#include <QUuid>

class CliTests : public QObject {
    Q_OBJECT
    struct Result { int code; QByteArray out, error; };
    Result run(const QStringList &args) {
        QProcess p; auto env=QProcessEnvironment::systemEnvironment(); env.insert("QT_QPA_PLATFORM","offscreen");
        env.remove("DISPLAY"); env.remove("WAYLAND_DISPLAY"); p.setProcessEnvironment(env);
        p.start(QString::fromUtf8(KOMAMIMIC_EXECUTABLE),args);
        if(!p.waitForStarted(5000)) return {-999,{},p.errorString().toUtf8()};
        if(!p.waitForFinished(15000)) { p.kill(); p.waitForFinished(); return {-998,p.readAllStandardOutput(),p.readAllStandardError()}; }
        return {p.exitStatus()==QProcess::NormalExit?p.exitCode():-997,p.readAllStandardOutput(),p.readAllStandardError()};
    }
private slots:
    void informationalCommands() {
        auto v=run({"--version"}); QCOMPARE(v.code,0); QVERIFY(v.out.contains(KOMAMIMIC_VERSION));
        auto help=run({"--help"}); QCOMPARE(help.code,0); QVERIFY(help.out.contains("--config"));
        auto formats=run({"--formats"}); QCOMPARE(formats.code,0); QVERIFY(formats.out.contains("PDF")); QVERIFY(formats.out.contains("CP932"));
        QVERIFY(formats.out.contains("mag")); QVERIFY(formats.out.contains("pic2"));
        // QTextStream writes CRLF through stdout on Windows.
        formats.out.replace("\r\n", "\n");
        QVERIFY2(formats.out.contains(", pic\n"), formats.out.constData());
        auto commands=run({"--commands"}); QCOMPARE(commands.code,0); QSet<QByteArray> ids;
        for(const auto &line:commands.out.split('\n')) if(!line.isEmpty()) {
            auto id=line.split('\t').first(); QVERIFY(!id.isEmpty()); QVERIFY(!ids.contains(id)); ids.insert(id);
        }
        QVERIFY(ids.contains("quit")); QVERIFY(ids.contains("fullscreenGlobal"));
        for (const auto &id : {"documentInherited", "documentStandard", "documentExtended"})
            QVERIFY(!ids.contains(id));
    }
    void invalidArguments_data() {
        QTest::addColumn<QStringList>("args");
        QTest::newRow("zero-page") << QStringList{"--page","0"};
        QTest::newRow("non-numeric-page") << QStringList{"--page","x"};
        QTest::newRow("page-mode") << QStringList{"--page-mode","3"};
        QTest::newRow("entry-mode") << QStringList{"--entry","5"};
        QTest::newRow("extra-path") << QStringList{"a","b"};
        QTest::newRow("unknown-option") << QStringList{"--not-an-option"};
        QTest::newRow("missing-readers") << QStringList{"--script-readers","/nonexistent-komamimic-readers","--formats"};
        QTest::newRow("retired-simple") << QStringList{"--execute","documentStandard"};
        QTest::newRow("retired-standard") << QStringList{"--execute","documentExtended"};
        QTest::newRow("retired-inherited") << QStringList{"--execute","documentInherited"};
    }
    void invalidArguments() { QFETCH(QStringList,args); auto r=run(args); QVERIFY2(r.code>0,qPrintable(QString::fromUtf8(r.error))); }
    void customReaderMetadataOnly() {
        QTemporaryDir d;
        QVERIFY(TestData::write(d.filePath("manifest.json"),
            R"({"apiVersion":1,"id":"test.cli","extensions":["toy"],"magic":"544f59","script":"decode.js"})"));
        QVERIFY(TestData::write(d.filePath("decode.js"),"while(true){} function decode(){}"));
        const auto formats=run({"--script-readers",d.path(),"--formats"});
        QCOMPARE(formats.code,0); QVERIFY(formats.out.contains("toy"));
    }
    void openJapanesePathAndPage() {
        QTemporaryDir d; const auto images=d.filePath("日本語の画像"); QVERIFY(TestData::imageFolder(images,4));
        const auto config=d.filePath("設定.ini");
        auto r=run({"--config",config,"--new-window",images+"/1.png","-p","1","-e","1","--page","3","--execute","quit"});
        QCOMPARE(r.code,0); QSettings settings(config,QSettings::IniFormat);
        QCOMPARE(settings.value("lastBook").toString(),images+"/1.png");
        LibraryStore db(d.filePath("library.sqlite3")); auto items=db.items("history"); QCOMPARE(items.size(),1);
        QCOMPARE(items.first().state.value("anchor").toInt(),2);
    }
    void ipcOrRestrictedFallback() {
        QTemporaryDir d; const auto config=d.filePath("settings.ini");
        QLocalServer probe;
        probe.setSocketOptions(QLocalServer::UserAccessOption);
        const auto name="komamimic-test-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
        if(!probe.listen(name)) {
            const auto fallback=run({"--config",config,"--execute","quit"});
            QCOMPARE(fallback.code,0); QVERIFY2(fallback.error.contains("IPC"),fallback.error.constData());
            QSKIP("IPC sockets unavailable: standalone fallback verified; IPC round trip not exercised");
        }
        probe.close(); QLocalServer::removeServer(name);
        QProcess first; first.start(QString::fromUtf8(KOMAMIMIC_EXECUTABLE),{"--config",config,"--execute","scaleFit"}); QVERIFY(first.waitForStarted());
        QElapsedTimer timer; timer.start();
        while(!QFile::exists(config+".instance.lock") && timer.elapsed()<5000 && first.state()!=QProcess::NotRunning) QTest::qWait(20);
        const bool locked=QFile::exists(config+".instance.lock");
        Result reply{-999,{},"No instance lock"};
        if(locked) reply=run({"--config",config,"--execute","quit"});
        const bool exited=first.waitForFinished(5000);
        if(!exited) { first.kill(); first.waitForFinished(); }
        const auto firstError=first.readAllStandardError();
        const auto diagnostic=QString("primary lock=%1; forward exit=%2; primary exited=%3; primary exit=%4\n"
                                      "forward stdout: %5\nforward stderr: %6\nprimary stderr: %7")
            .arg(locked).arg(reply.code).arg(exited).arg(first.exitCode())
            .arg(QString::fromUtf8(reply.out),QString::fromUtf8(reply.error),QString::fromUtf8(firstError)).toUtf8();
        QVERIFY2(locked,diagnostic.constData());
        QVERIFY2(reply.code==0,diagnostic.constData());
        QVERIFY2(exited,diagnostic.constData()); QCOMPARE(first.exitCode(),0);
    }
    void secondaryWaitsForServerPublication() {
        QTemporaryDir d; const auto config=d.filePath("settings.ini");
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        const auto probeName="komamimic-test-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
        if(!server.listen(probeName))QSKIP("IPC sockets unavailable; delayed server publication not exercised");
        server.close();QLocalServer::removeServer(probeName);
        const auto key=QFileInfo(config).absoluteDir().canonicalPath()+"/"+QFileInfo(config).fileName();
        const auto name="komamimic-"+QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(),QCryptographicHash::Sha256).toHex().left(32));
        QLockFile lock(config+".instance.lock");QVERIFY(lock.tryLock(0));
        QByteArray received;bool listening=false;
        connect(&server,&QLocalServer::newConnection,this,[&] {
            auto *socket=server.nextPendingConnection();if(!socket)return;
            const auto answer=[&,socket] {
                received+=socket->readAll();
                if(received.contains('\n')){socket->write("ok\n");socket->flush();socket->disconnectFromServer();}
            };
            connect(socket,&QLocalSocket::readyRead,socket,answer);
            connect(socket,&QLocalSocket::disconnected,socket,&QObject::deleteLater);
            if(socket->bytesAvailable())answer();
        });
        QProcess process;
        const auto cleanup=qScopeGuard([&]{if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished();}});
        process.start(QString::fromUtf8(KOMAMIMIC_EXECUTABLE),{"--config",config,"--execute","quit"});
        QVERIFY(process.waitForStarted(5000));
        // Hold the same lock as a primary which has not published its pipe yet.
        QTimer::singleShot(750,&server,[&]{listening=server.listen(name);});
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,10000);
        QVERIFY2(listening,qPrintable(server.errorString()));
        QCOMPARE(process.exitStatus(),QProcess::NormalExit);
        QCOMPARE(process.exitCode(),0);
        QVERIFY2(received.contains("quit"),process.readAllStandardError().constData());
        QVERIFY(QJsonDocument::fromJson(received.trimmed()).isObject());
    }
};
ISOLATED_TEST_MAIN(CliTests)
#include "cli_tests.moc"
