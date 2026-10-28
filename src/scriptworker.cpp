// SPDX-License-Identifier: GPL-3.0-only
#include "media.h"
#include "i18n.h"
#include "imageprocessing.h"
#include <QJsonArray>
#include <cmath>
#include <limits>
#include <QCoreApplication>
#include <QFile>
#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
#include <sys/resource.h>
#endif

namespace {
// The watchdog never touches JS values. QJSEngine explicitly permits calling
// setInterrupted() from another thread. Join before destroying the engine.
class Deadline {
    std::mutex mutex;
    std::condition_variable wake;
    bool done = false;
    std::thread thread;
public:
    Deadline(QJSEngine &engine, int milliseconds) : thread([&, milliseconds] {
        std::unique_lock lock(mutex);
        if (!wake.wait_for(lock, std::chrono::milliseconds(milliseconds), [&]{ return done; }))
            engine.setInterrupted(true);
    }) {}
    ~Deadline() {
        { std::lock_guard lock(mutex); done = true; }
        wake.notify_one(); thread.join();
    }
};
struct Reply { QJsonObject header; QByteArray pixels; };
Reply failure(const QString &message) { return {{{"error", message.left(8192)}}, {}}; }
QByteArray readExact(QFile &file, qsizetype length) {
    QByteArray data; data.reserve(length);
    while (data.size() < length) {
        const auto part = file.read(length - data.size());
        if (part.isEmpty()) break;
        data += part;
    }
    return data;
}
Reply execute(const QJsonObject &request, const QByteArray &bytes) {
    const auto source = request.value("source").toString();
    const int timeout = request.value("timeoutMs").toInt();
    const int api=request.value("apiVersion").toInt();
    const auto operation=request.value("operation").toString("decode");
    if (request.value("apiVersion")!=QJsonValue(1) || !QStringList{"decode","inspect","filter"}.contains(operation) || source.isEmpty()
        || source.toUtf8().size() > 256*1024 || timeout < 1 || timeout > 60000)
        return failure(QCoreApplication::translate("KomaMimic", "不正なスクリプトrequestです。"));
    QJSEngine engine;
    Deadline deadline(engine, timeout);
    // Neither QML imports nor QObject/native capabilities are exposed.
    const auto entry = engine.evaluate("(function(){'use strict';\n" + source +
        "\nreturn typeof " + operation + "==='function' ? " + operation + " : null;})()", request.value("name").toString(), 0);
    if (entry.isError()) return failure(entry.toString() + '\n' + entry.property("stack").toString());
    if (!entry.isCallable()) return failure(QCoreApplication::translate("KomaMimic", "decode(input, context) 関数がありません。"));
    auto context = engine.newObject();
    context.setProperty("apiVersion", api);
    context.setProperty("pageIndex",request.value("pageIndex").toInt());
    context.setProperty("settings",engine.toScriptValue(request.value("settings").toObject().toVariantMap()));
    context.setProperty("maxPages",10000);
    context.setProperty("maxPixels", double(MaxPixels));
    context.setProperty("maxBytes", double(MaxEncodedBytes));
    // Catch even primitive exceptions (throw 123), which isError() cannot identify.
    auto invoke = engine.evaluate("(function(fn,input,ctx){try{return {ok:true,value:fn(input,ctx)};}"
                                  "catch(e){return {ok:false,error:String(e),stack:e&&e.stack};}})");
    QJSValue input=engine.toScriptValue(bytes);
    if(operation=="filter") {
      input=engine.newObject();
      input.setProperty("width",request.value("width").toInt());input.setProperty("height",request.value("height").toInt());
    }
    auto result = invoke.call({entry, input, context});
    if (engine.isInterrupted()) return failure(QCoreApplication::translate("KomaMimic", "スクリプトの実行期限を超えました。"));
    if (result.isError()) return failure(result.toString());
    if (!result.property("ok").toBool()) return failure(result.property("error").toString() + '\n' + result.property("stack").toString());
    if(operation=="inspect") {
      auto validate=engine.evaluate("(function(v){try{if(!v||!Number.isInteger(v.pages)||v.pages<1||v.pages>10000)throw 'Invalid page count';return {ok:true,pages:v.pages};}catch(e){return {ok:false,error:String(e)}}})");
      result=validate.call({result.property("value")});
      if(engine.isInterrupted())return failure(QCoreApplication::translate("KomaMimic", "スクリプトの実行期限を超えました。"));
      if(!result.property("ok").toBool())return failure(result.property("error").toString());
      return {{{"pages",result.property("pages").toInt()}},{}};
    }
    if(operation=="filter") {
      const int width=request.value("width").toInt(), height=request.value("height").toInt();
      if(width<1 || height<1 || qint64(width)*height>MaxPixels || bytes.size()!=qint64(width)*height*4)return failure(QCoreApplication::translate("KomaMimic", "フィルター入力が不正です。"));
      auto validate=engine.evaluate("(function(v){try{if(!Array.isArray(v)||v.length>32)throw 'At most 32 operations';const s=JSON.stringify(v);if(s.length>16384)throw 'Plan too large';return {ok:true,json:s};}catch(e){return {ok:false,error:String(e)}}})");
      result=validate.call({result.property("value")});
      if(engine.isInterrupted())return failure(QCoreApplication::translate("KomaMimic", "スクリプトの実行期限を超えました。"));
      if(!result.property("ok").toBool())return failure(result.property("error").toString());
      const auto operations=QJsonDocument::fromJson(result.property("json").toString().toUtf8()).array();
      QImage image(reinterpret_cast<const uchar*>(bytes.constData()),width,height,width*4,QImage::Format_RGBA8888);image=image.copy();
      for(const auto &value:operations) {
        if(!value.isObject())return failure(QCoreApplication::translate("KomaMimic", "操作はobjectで指定してください。"));
        const auto object=value.toObject();const auto op=object.value("op").toString();
        ImageOptions options;
        const auto numeric=object.value("value").toDouble(std::numeric_limits<double>::quiet_NaN());
        if(op=="grayscale")options.gray=true;
        else if(op=="invert")options.invert=true;
        else if(op=="flipH")options.flipH=true;
        else if(op=="flipV")options.flipV=true;
        else if(op=="gamma" && std::isfinite(numeric) && numeric>=0.1 && numeric<=10)options.gamma=numeric;
        else if(op=="brightness" && std::isfinite(numeric) && numeric>=-100 && numeric<=100)options.brightness=int(numeric);
        else if(op=="contrast" && std::isfinite(numeric) && numeric>=-100 && numeric<=100)options.contrast=int(numeric);
        else if(op=="rotate" && std::isfinite(numeric) && std::abs(numeric)<=270 && int(numeric)%90==0 && numeric==int(numeric))options.rotation=int(numeric);
        else if(op=="resize" && object.value("width").isDouble() && object.value("height").isDouble()) {
          options.resize={object.value("width").toInt(),object.value("height").toInt()};
          if(options.resize.width()<1 || options.resize.height()<1 || options.resize.width()>16384 || options.resize.height()>16384 || qint64(options.resize.width())*options.resize.height()>MaxPixels)return failure(QCoreApplication::translate("KomaMimic", "resize寸法が上限外です。"));
        } else return failure(QCoreApplication::translate("KomaMimic", "未対応・不正なフィルター操作：")+op);
        image=processImage(image,options,[&]{return engine.isInterrupted();});
        if(image.isNull() || engine.isInterrupted())return failure(QCoreApplication::translate("KomaMimic", "フィルターを取り消したか、出力上限を超えました。"));
      }
      image=image.convertToFormat(QImage::Format_RGBA8888);
      return {{{"width",image.width()},{"height",image.height()}},QByteArray(reinterpret_cast<const char*>(image.constBits()),image.sizeInBytes())};
    }
    // Inspect and copy inside JS as well: getters/proxies can throw or loop.
    auto validate = engine.evaluate(
        "(function(v,max){try {"
        "if(!v || !Number.isInteger(v.width) || !Number.isInteger(v.height) ||"
        "v.width<1 || v.height<1 || v.width>max || v.height>max || v.width*v.height>max)"
        "throw new Error('Invalid image dimensions');"
        "if(!(v.pixels instanceof ArrayBuffer) || v.pixels.byteLength!==v.width*v.height*4)"
        "throw new Error('pixels must be a tightly packed RGBA8 ArrayBuffer');"
        "return {ok:true,width:v.width,height:v.height,pixels:v.pixels};"
        "}catch(e){return {ok:false,error:String(e)};}})");
    result = validate.call({result.property("value"), QJSValue(double(MaxPixels))});
    if (engine.isInterrupted()) return failure(QCoreApplication::translate("KomaMimic", "スクリプトの実行期限を超えました。"));
    if (result.isError()) return failure(result.toString());
    if (!result.property("ok").toBool()) return failure(result.property("error").toString());
    const int width = result.property("width").toInt(), height = result.property("height").toInt();
    const auto pixels = result.property("pixels").toVariant().toByteArray();
    if (width < 1 || height < 1 || qint64(width)*height > MaxPixels || pixels.size() != qint64(width)*height*4)
        return failure(QCoreApplication::translate("KomaMimic", "スクリプト画像のbyte数が不正です。"));
    return {{{"width", width}, {"height", height}}, pixels};
}
}
// Limit only the disposable script worker, never the GUI or other applications.
static bool limitWorkerMemory() {
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
    struct rlimit current{}; if(getrlimit(RLIMIT_AS,&current)!=0)return false;
    const rlim_t budget=rlim_t(2)*1024*1024*1024;
    current.rlim_cur=std::min(current.rlim_cur,budget);
    return setrlimit(RLIMIT_AS,&current)==0;
#elif defined(Q_OS_WIN)
    static HANDLE job=CreateJobObjectW(nullptr,nullptr);
    if(!job)return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    info.ProcessMemoryLimit=SIZE_T(2)*1024*1024*1024;
    return SetInformationJobObject(job,JobObjectExtendedLimitInformation,&info,sizeof info) && AssignProcessToJobObject(job,GetCurrentProcess());
#else
    // macOS has no equivalent reliable address-space ceiling for this runtime.
    // Input/output limits and the parent deadline/kill remain active.
    return true;
#endif
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    I18n::initialize(argc > 1 ? QString::fromUtf8(argv[1]) : QStringLiteral("ja"));
    if(!limitWorkerMemory()){fputs("Cannot apply script worker memory limit\n",stderr);return 1;}
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);
#endif
    QFile input, output;
    if (!input.open(stdin, QIODevice::ReadOnly) || !output.open(stdout, QIODevice::WriteOnly)) return 1;
    Reply reply = failure(QCoreApplication::translate("KomaMimic", "スクリプト入力が壊れています。"));
    const auto frame = readExact(input, 8);
    if (frame.size() == 8) {
        const auto headerSize = qFromBigEndian<quint32>(frame.constData());
        const auto byteSize = qFromBigEndian<quint32>(frame.constData()+4);
        if (headerSize <= 1024*1024 && byteSize <= MaxPixels*4) {
            const auto header = readExact(input, headerSize), bytes = readExact(input, byteSize);
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(header, &parse);
            if (header.size() == headerSize && bytes.size() == byteSize && input.read(1).isEmpty()
                && parse.error == QJsonParseError::NoError && document.isObject())
                reply = execute(document.object(), bytes);
        }
    }
    const auto header = QJsonDocument(reply.header).toJson(QJsonDocument::Compact);
    QByteArray response(8, char(0));
    qToBigEndian<quint32>(quint32(header.size()), response.data());
    qToBigEndian<quint32>(quint32(reply.pixels.size()), response.data()+4);
    response += header;
    if (output.write(response) != response.size() || output.write(reply.pixels) != reply.pixels.size() || !output.flush()) return 1;
    return 0;
}
