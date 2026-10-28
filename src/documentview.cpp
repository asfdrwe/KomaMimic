// SPDX-License-Identifier: GPL-3.0-only
#include "documentview.h"
#include <QCoreApplication>
#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineView>
#include <QWheelEvent>

namespace {
const QByteArray Scheme = "koma-document";
const QString Csp =
    "<meta charset=\"utf-8\"><meta http-equiv=\"Content-Security-Policy\" "
    "content=\"default-src 'none'; img-src 'self' data:; style-src 'self' "
    "'unsafe-inline'; font-src 'self'; frame-src 'self'; script-src 'none'; "
    "connect-src 'none'; object-src 'none'; base-uri 'none'; form-action "
    "'none'\">";
class Handler : public QWebEngineUrlSchemeHandler {
public:
  using QWebEngineUrlSchemeHandler::QWebEngineUrlSchemeHandler;
  std::shared_ptr<const Publication> publication;
  QString host;
  bool fonts = true;
  void requestStarted(QWebEngineUrlRequestJob *job) override {
    if (!publication || job->requestUrl().host() != host ||
        job->requestMethod() != "GET") {
      job->fail(QWebEngineUrlRequestJob::RequestDenied);
      return;
    }
    const auto key =
        publicationPath({}, job->requestUrl().path(QUrl::FullyEncoded).mid(1));
    QByteArray data, mime = "text/html";
    if (key == "__reader__.html")
      data =
          ("<!doctype html><html><head>" + Csp + "</head><body></body></html>")
              .toUtf8();
    else {
      for (const auto &section : publication->sections)
        if (key == section.path) {
          auto html = section.html;
          if (section.wide) {
            QUrl image;
            image.setPath('/' + section.imagePath);
            html = "<html><head><style>html,body{margin:0;width:100%;height:100%;overflow:hidden}"
                   "img{width:100%;height:100%;object-fit:contain;display:block}</style></head><body><img src=\"" +
                   image.toString(QUrl::FullyEncoded).toHtmlEscaped() + "\"></body></html>";
          }
          html.remove(QRegularExpression("<\\?xml[^>]*>"));
          const auto head =
              QRegularExpression("<head(?:\\s[^>]*)?>",
                                 QRegularExpression::CaseInsensitiveOption)
                  .match(html);
          auto childCsp = Csp;
          childCsp.replace("frame-src 'self'", "frame-src 'none'");
          if (head.hasMatch())
            html.insert(head.capturedEnd(), childCsp);
          else
            html = "<!doctype html><html><head>" + childCsp + "</head><body>" +
                   html + "</body></html>";
          data = html.toUtf8();
          break;
        }
      if (data.isEmpty()) {
        data = publication->resources.value(key);
        mime = QMimeDatabase()
                   .mimeTypeForFile(key, QMimeDatabase::MatchExtension)
                   .name()
                   .toUtf8();
        if (!fonts && (mime.startsWith("font/") || key.endsWith(".otf") ||
                       key.endsWith(".ttf") || key.endsWith(".woff") ||
                       key.endsWith(".woff2")))
          data.clear();
        if (data.isEmpty() && !publication->localRoot.isEmpty()) {
          const auto image = documentResource(publication->localRoot, QUrl(key))
                                 .value<QImage>();
          if (!image.isNull()) {
            QBuffer buffer(&data);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            mime = "image/png";
          }
        }
      }
    }
    if (data.isEmpty()) {
      job->fail(QWebEngineUrlRequestJob::UrlNotFound);
      return;
    }
    auto *buffer = new QBuffer(job);
    buffer->setData(data);
    buffer->open(QIODevice::ReadOnly);
    job->reply(mime, buffer);
  }
};
class Interceptor : public QWebEngineUrlRequestInterceptor {
public:
  using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;
  void interceptRequest(QWebEngineUrlRequestInfo &info) override {
    const auto url = info.requestUrl();
    // No network or file access, including CSS imports and nested documents.
    if (url.scheme() != Scheme &&
        !(url.scheme() == "data" &&
          info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeImage))
      info.block(true);
  }
};
class Page : public QWebEnginePage {
public:
  using QWebEnginePage::QWebEnginePage;

protected:
  bool acceptNavigationRequest(const QUrl &url, NavigationType,
                               bool main) override {
    return !main ||
           (url.scheme() == Scheme && url.path() == "/__reader__.html");
  }
  QWebEnginePage *createWindow(WebWindowType) override { return nullptr; }
  QStringList chooseFiles(FileSelectionMode, const QStringList &,
                          const QStringList &) override {
    return {};
  }
};
} // namespace
QJsonObject DocumentView::readerMessages() {
  return {
    {"nodeLimit", QCoreApplication::translate("KomaMimic", "本文のテキスト要素数が上限を超えています。")},
    {"rangeLimit", QCoreApplication::translate("KomaMimic", "文書のレイアウト範囲が上限を超えています。")},
    {"pageLimit", QCoreApplication::translate("KomaMimic", "文書のページ数が上限を超えています。")},
    {"back", QCoreApplication::translate("KomaMimic", "リンク元へ戻る")},
    {"notePrevious", QCoreApplication::translate("KomaMimic", "注釈の前頁")},
    {"noteNext", QCoreApplication::translate("KomaMimic", "注釈の次頁")},
    {"loadFailed", QCoreApplication::translate("KomaMimic", "本文を読み込めません。")},
    {"loadTimeout", QCoreApplication::translate("KomaMimic", "本文の読み込みがタイムアウトしました。")},
    {"parseFailed", QCoreApplication::translate("KomaMimic", "本文がHTMLとして解釈できません。")}
  };
}
void DocumentView::registerScheme() {
  if (!QWebEngineUrlScheme::schemeByName(Scheme).name().isEmpty())
    return;
  QWebEngineUrlScheme scheme(Scheme);
  scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
  scheme.setFlags(QWebEngineUrlScheme::SecureScheme);
  QWebEngineUrlScheme::registerScheme(scheme);
}
DocumentView::DocumentView(QWidget *parent) : QWidget(parent) {
  Q_INIT_RESOURCE(documents);
  setObjectName("documentView");
  m_profile =
      new QWebEngineProfile(this); // Off-the-record; no shared browser data.
  m_handler = new Handler(m_profile);
  m_profile->installUrlSchemeHandler(Scheme, static_cast<Handler *>(m_handler));
  m_profile->setUrlRequestInterceptor(new Interceptor(m_profile));
  connect(m_profile, &QWebEngineProfile::downloadRequested, this,
          [](QWebEngineDownloadRequest *request) { request->cancel(); });
  auto *page = new Page(m_profile, this);
  auto *settings = page->settings();
  settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls,
                         false);
  settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls,
                         false);
  settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
  settings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard,
                         false);
  settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
  settings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, false);
  m_web = new QWebEngineView(this);
  m_web->setPage(page);
  setFocusProxy(m_web);
  m_web->setContextMenuPolicy(Qt::NoContextMenu);
  qApp->installEventFilter(this);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(m_web);
  m_timer = new QTimer(this);
  m_timer->setInterval(150);
  connect(m_timer, &QTimer::timeout, this, &DocumentView::poll);
  connect(page, &QWebEnginePage::renderProcessTerminated, this, [this] {
    m_timer->stop();
    emit failed(QCoreApplication::translate("KomaMimic", "文書の描画プロセスが終了しました。本を開き直してください。"));
  });
}
DocumentView::~DocumentView() {
  qApp->removeEventFilter(this);
  delete m_web->page();
  delete m_web;
  delete m_profile;
}
void DocumentView::clear() {
  ++m_generation;
  m_timer->stop();
  m_ready = false;
  m_vertical = false; m_auxiliary=false;
  m_polling = false;
  m_locations.clear();
  m_location = {};
  m_web->stop();
  static_cast<Handler *>(m_handler)->publication.reset();
}
void DocumentView::open(std::shared_ptr<const Publication> publication,
                        const TextOptions &o,
                        const DocumentLocation &location) {
  clear();
  const auto generation = m_generation;
  auto *handler = static_cast<Handler *>(m_handler);
  handler->publication = publication;
  handler->host = QUuid::createUuid().toString(QUuid::WithoutBraces);
  handler->fonts = o.embeddedFonts;
  const auto base = "koma-document://" + handler->host + '/';
  QJsonArray sections;
  for (const auto &s : publication->sections)
    sections.append(QJsonObject{
        {"path", s.path},
        {"plain", s.plain},
        {"side",s.side},{"solo",s.solo},{"auxiliary",s.auxiliary},
        {"fixed", s.fixed || s.wide},
        {"imagePage", s.wide},
        {"wide", s.wide && o.wideImages},
        {"width", s.wide ? (o.wideImages ? 2424 : 1200) : s.viewport.isValid() ? s.viewport.width() : 0},
        {"height", s.wide ? 1600 : s.viewport.isValid() ? s.viewport.height() : 0}});
  QJsonObject options{{"background", o.background.name()},
                      {"markdownStyle", publication->markdown ? markdownReaderStyle() : QString{}},
                      {"foreground", o.foreground.name()},
                      {"family", o.family},
                      {"size", o.pointSize},
                      {"spacing", o.lineSpacing},
                      {"margin", o.margin},
                      {"pageLayout", o.pageLayout},
                      {"writingMode", o.writingMode == 0 && !publication->epub
                                          ? (o.vertical ? 2 : 1)
                                          : o.writingMode},
                      {"ruby", o.ruby},
                      {"publisherStyles", o.publisherStyles},
                      {"overrideColors", o.overrideColors}};
  const auto spec = QString::fromUtf8(
      QJsonDocument(QJsonObject{{"base", base},
                                {"messages", readerMessages()}, {"sections", sections},
                                {"options", options},
                                {"epub", publication->epub},
                                {"rightToLeft", publication->epub ? publication->rightToLeft
                                    : options.value("writingMode").toInt() == 2},
                                {"location", location.json()}})
          .toJson(QJsonDocument::Compact));
  auto connection = std::make_shared<QMetaObject::Connection>();
  *connection = connect(m_web, &QWebEngineView::loadFinished, this,
                        [this, generation, spec, connection, base](bool ok) {
                          if (generation != m_generation) {
                            disconnect(*connection);
                            return;
                          }
                          if (m_web->url().host() != QUrl(base).host())
                            return;
                          disconnect(*connection);
                          if (!ok) {
                            emit failed(QCoreApplication::translate("KomaMimic", "文書ビューを起動できません。"));
                            return;
                          }
                          QFile script(":/documents/layout.js");
                          QFile panels(":/documents/panels.js");
                          if (!script.open(QIODevice::ReadOnly) ||
                              !panels.open(QIODevice::ReadOnly)) {
                            emit failed(QCoreApplication::translate("KomaMimic", "文書描画スクリプトがありません。"));
                            return;
                          }
                          m_web->page()->runJavaScript(
                              QString::fromUtf8(panels.readAll()) + '\n' +
                                  QString::fromUtf8(script.readAll()) +
                                  "\nwindow.komaDocument.start(" + spec + ");",
                              QWebEngineScript::ApplicationWorld);
                          m_timer->start();
                        });
  m_web->load(QUrl(base + "__reader__.html"));
  QTimer::singleShot(60000, this, [this, generation] {
    if (generation == m_generation && !m_ready) {
      m_timer->stop();
      emit failed(QCoreApplication::translate("KomaMimic", "文書のレイアウトがタイムアウトしました。"));
    }
  });
}
void DocumentView::poll() {
  if (m_polling)
    return;
  m_polling = true;
  const auto generation = m_generation;
  QPointer<DocumentView> guard(this);
  m_web->page()->runJavaScript(
      (m_ready ? "window.komaDocument && window.komaDocument.state(false)"
               : "window.komaDocument && window.komaDocument.state(true)"),
      QWebEngineScript::ApplicationWorld,
      [guard, generation](const QVariant &value) {
        if (!guard || generation != guard->m_generation)
          return;
        auto *self = guard.data();
        self->m_polling = false;
        const auto state = QJsonObject::fromVariantMap(value.toMap());
        if (!state.value("error").toString().isEmpty()) {
          self->m_timer->stop();
          emit self->failed(state.value("error").toString());
          return;
        }
        if (!state.value("ready").toBool())
          return;
        self->m_auxiliary=state.value("auxiliary").toBool();
        self->m_vertical = state.value("vertical").toBool();
        if (!self->m_ready) {
          for (const auto &p : state.value("pages").toArray())
            self->m_locations.append(DocumentLocation::fromJson(p.toObject()));
          self->m_ready = true;
          emit self->laidOut(self->m_locations);
        }
        const int page = state.value("current").toInt();
        if (page >= 0 && page < self->m_locations.size() &&
            state.value("changed").toBool()) {
          self->m_location = self->m_locations[page];
          emit self->positionChanged(page, self->m_location);
        }
      });
}
void DocumentView::jump(int page) {
  m_web->page()->runJavaScript(
      QString("window.komaDocument && window.komaDocument.select(%1)")
          .arg(page),
      QWebEngineScript::ApplicationWorld);
}
void DocumentView::find(const QString &text) {
  const auto arg = QString::fromUtf8(
      QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact));
  m_web->page()->runJavaScript(
      "window.komaDocument && window.komaDocument.find(" + arg + "[0])",
      QWebEngineScript::ApplicationWorld);
}

void DocumentView::navigate(const QString &section, const QString &fragment,
                            int offset) {
  const auto args =
      QString::fromUtf8(QJsonDocument(QJsonObject{{"section", section},
                                                  {"fragment", fragment},
                                                  {"offset", offset}})
                            .toJson(QJsonDocument::Compact));
  m_web->page()->runJavaScript(
      "window.komaDocument && window.komaDocument.navigate(" + args + ")",
      QWebEngineScript::ApplicationWorld);
}

bool DocumentView::eventFilter(QObject *object, QEvent *event) {
  const auto *widget = qobject_cast<QWidget *>(object);
  if (event->type() == QEvent::Wheel && widget &&
      (widget == this || isAncestorOf(widget))) {
    const auto *wheel = static_cast<QWheelEvent *>(event);
    m_wheel += wheel->angleDelta().y();
    if (std::abs(m_wheel) >= 120) {
      if (m_wheel < 0)
        emit nextRequested();
      else
        emit previousRequested();
      m_wheel = 0;
    }
    event->accept();
    return true;
  }
  return QWidget::eventFilter(object, event);
}

void DocumentView::back() { m_web->page()->runJavaScript("window.komaDocument && window.komaDocument.back()", QWebEngineScript::ApplicationWorld); }

void DocumentView::stepNote(int direction) { m_web->page()->runJavaScript(QString("window.komaDocument && window.komaDocument.stepNote(%1)").arg(direction>0?1:-1), QWebEngineScript::ApplicationWorld); }
