// SPDX-License-Identifier: GPL-3.0-only
#include "documentsettings.h"
#include <QCoreApplication>
#include "mainwindow.h"
#include "pageview.h"
#include "readerfeatures.h"
#include "shortcuts.h"
#include <QAction>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFontComboBox>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>
#include <algorithm>
#include "documentview.h"

void MainWindow::buildDocumentActions() {
  auto *settings = m_actions["documentSettings"];
  settings->setObjectName("action_documentSettings");
  m_actions["documentSettings"] = settings;
  connect(settings, &QAction::triggered, this, &MainWindow::documentSettings);
  for (const auto &id : {"textSizeIncrease", "textSizeDecrease", "textSizeReset", "documentWritingMode", "documentPublisherStyles"}) {
    auto *action = m_actions.value(id);
    if (!action) continue;
    connect(action, &QAction::triggered, this, [this, id = QString(id)] {
      if (!m_book) return;
      const auto o = m_book->openOptions.text;
      if (id == "textSizeReset") setDocumentOption("size", {});
      else if (id == "textSizeIncrease" || id == "textSizeDecrease")
        setDocumentOption("size", std::clamp(o.pointSize + (id == "textSizeIncrease" ? 2 : -2), 8, 120));
      else if (id == "documentPublisherStyles") setDocumentOption("publisherStyles", !o.publisherStyles);
      else if (documentViewActive()) {
        bool vertical = o.writingMode == 2;
        if (m_documentView) vertical = m_documentView->vertical();
        setDocumentOption("writingMode", vertical ? 1 : 2);
      }
      else setDocumentOption("vertical", !o.vertical);
    });
  }
  connect(m_actions["documentBack"], &QAction::triggered, this, [this] { if(documentViewActive() && m_documentView) m_documentView->back(); });
  auto *find = m_actions["findDocument"];
  find->setObjectName("action_findDocument");
  m_actions["findDocument"] = find;
  connect(find, &QAction::triggered, this, [this] {
    bool ok = false;
    const auto text = QInputDialog::getText(
        this, QCoreApplication::translate("KomaMimic", "本文を検索"), QCoreApplication::translate("KomaMimic", "検索する文字列"), QLineEdit::Normal, {}, &ok);
    if (!ok || text.isEmpty() || !m_book)
      return;
    if (documentViewActive()) {
      m_documentView->find(text);
      return;
    }
    const auto p = m_book->documentContent ? m_book->documentContent : readPublication(m_currentPath, m_book->openOptions.text);
    if (!p->error.isEmpty() || p->sections.isEmpty())
      return;
    const auto plain = p->sections.first().plain;
    auto start = currentDocumentLocation().offset + 1;
    auto offset = plain.indexOf(text, start, Qt::CaseInsensitive);
    if (offset < 0)
      offset = plain.indexOf(text, 0, Qt::CaseInsensitive);
    if (offset < 0)
      statusBar()->showMessage(QCoreApplication::translate("KomaMimic", "本文に見つかりません。"), 5000);
    else {
      setMode(ViewMode::Reader);
      jump(resolveDocumentLocation({"text.html", int(offset), text}));
    }
  });
}
void MainWindow::updateDocumentActions() {
  if (!m_actions.contains("documentSettings"))
    return;
  const bool text = isTextDocumentPath(m_currentPath);
  const bool extended = documentViewActive();
  m_actions["autoSpread"]->setEnabled(!extended && !m_loading);
  for (const auto &id : {"rtl", "cover", "thumbnails", "imagePaste"})
    if (auto *a = m_actions.value(id))
      a->setEnabled(!extended || (QString(id) == "thumbnails" && m_mode == ViewMode::Covers));
  if (extended) {
    const QSignalBlocker blocker(m_actions["spread"]);
    m_actions["spread"]->setChecked(m_book->openOptions.text.pageLayout == 1);
  } else {
    const QSignalBlocker blocker(m_actions["spread"]);
    m_actions["spread"]->setChecked(m_options.spread);
  }
  m_actions["spread"]->setEnabled(!m_loading && (!extended || m_renderReady));
  for (const auto &spec : shortcutSpecs())
    if (spec.id.startsWith("scale") || spec.id.startsWith("zoom") ||
        spec.id.startsWith("filter") || spec.id.startsWith("rotate") ||
        spec.id.startsWith("resize") || spec.id.startsWith("sharpen") || spec.id == "noUpscale")
      m_actions[spec.id]->setEnabled(!extended);
  const bool editableText = text && m_book && m_renderReady && !m_loading;
  bool fixed = false;
  if (extended && m_book->publication)
    for (const auto &s : m_book->publication->sections)
      if (s.path == m_book->pages[m_anchor].documentSection) { fixed = s.fixed || s.wide; break; }
  for (const auto &id : {"textSizeIncrease", "textSizeDecrease", "textSizeReset", "documentWritingMode"})
    m_actions[id]->setEnabled(editableText && !fixed &&
        (QString(id) != "documentWritingMode" || extended || !isMarkdownDocument(m_currentPath, m_book->openOptions.text)));
  if (auto *a = m_actions.value("documentPublisherStyles")) a->setEnabled(editableText && extended);
  if (extended)
    for (const auto &id :
         {"splitVertical", "splitHorizontal", "splitReset", "insertBlank",
          "coverBlank",    "editCopy",        "editCut",    "editPaste",
          "editDelete",    "editClear",       "editUndo",   "editRedo",
          "addPages",      "trashPages",      "imageCopy",  "saveSpread",
          "saveImage",     "saveAll",         "saveZip",    "savePdf",
          "saveGif",       "print",           "preloadAll", "registerCover",
          "registerSpine", "saveThumbnail",   "imageInfo"})
      if (auto *a = m_actions.value(id))
        a->setEnabled(false);
  m_actions["documentSettings"]->setEnabled(!m_loading);
  m_actions["documentBack"]->setEnabled(extended && m_renderReady);
  m_actions["findDocument"]->setEnabled(text && m_book && m_renderReady);

}
void MainWindow::setDocumentOption(const QString &key, const QVariant &value) {
  if (!m_book || !isTextDocumentPath(m_currentPath)) return;
  m_pendingDocumentLocation = currentDocumentLocation();
  m_restoreDocumentLocation = true;
  const auto setting = documentBookKey(m_currentPath) + key;
  if (value.isValid()) m_settings->setValue(setting, value); else m_settings->remove(setting);
  m_settings->sync();
  loadBook(m_currentPath, m_mode);
}
DocumentLocation MainWindow::currentDocumentLocation() const {
  if (!m_book || m_anchor < 0 || m_anchor >= pageCount())
    return {};
  const auto &p = m_book->pages[m_anchor];
  QString quote;
  if (m_book->documentContent)
    for (const auto &s : m_book->documentContent->sections)
      if (s.path == p.documentSection) {
        quote = s.plain.mid(std::max(0, p.documentOffset), 40);
        break;
      }
  return {p.documentSection, std::max(0, p.documentOffset), quote};
}
int MainWindow::resolveDocumentLocation(
    const DocumentLocation &location) const {
  int result = 0, offset = location.offset;
  if (m_book && m_book->documentContent && !location.quote.isEmpty())
    for (const auto &s : m_book->documentContent->sections)
      if (s.path == location.section &&
          s.plain.mid(offset, location.quote.size()) != location.quote) {
        const int found =
            s.plain.indexOf(location.quote, std::max(0, offset - 4096));
        if (found >= 0)
          offset = found;
      }
  for (int i = 0; i < pageCount(); ++i) {
    const auto &p = m_book->pages[i];
    if (p.documentSection == location.section && p.documentOffset <= offset)
      result = i;
  }
  return result;
}
void MainWindow::saveDocumentLocation() {
  if (!isTextDocumentPath(m_currentPath) || !m_book || !m_renderReady)
    return;
  m_settings->setValue(documentBookKey(m_currentPath) + "location",
                       currentDocumentLocation().json().toVariantMap());
}
void MainWindow::documentSettings() {
  const auto path = m_currentPath;
  const bool book = isTextDocumentPath(path);
  QDialog dialog(this);
  dialog.setObjectName("documentSettingsDialog");
  dialog.setWindowTitle(QCoreApplication::translate("KomaMimic", "文書の表示設定"));
  dialog.resize(640, 650);
  auto *outer = new QVBoxLayout(&dialog);
  auto *scroll = new QScrollArea(&dialog);
  scroll->setWidgetResizable(true);
  outer->addWidget(scroll);
  auto *content = new QWidget(scroll);
  scroll->setWidget(content);
  auto *layout = new QVBoxLayout(content);
  auto *form = new QFormLayout;
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  layout->addLayout(form);
  auto *scope = new QComboBox(&dialog);
  scope->setObjectName("documentSettingsScope");
  if (book)
    scope->addItems({QCoreApplication::translate("KomaMimic", "この本"), QCoreApplication::translate("KomaMimic", "この形式の既定値"), QCoreApplication::translate("KomaMimic", "すべての文書の既定値")});
  else
    scope->addItem(QCoreApplication::translate("KomaMimic", "すべての文書の既定値"));
  form->addRow(QCoreApplication::translate("KomaMimic", "保存先"), scope);
  QHash<QString, QWidget *> fields;
  const auto choice = [&](QFormLayout *f, const QString &key,
                          const QString &label, const QStringList &items) {
    auto *c = new QComboBox(&dialog);
    c->addItems(items);
    c->setObjectName("document_" + key);
    fields[key] = c;
    f->addRow(label, c);
    return c;
  };
  const auto check = [&](QFormLayout *f, const QString &key,
                         const QString &label) {
    auto *c = new QCheckBox(&dialog);
    c->setObjectName("document_" + key);
    fields[key] = c;
    f->addRow(label, c);
  };
  choice(form, "contentMode", QCoreApplication::translate("KomaMimic", "TXT / Markdownの解釈"),
         {QCoreApplication::translate("KomaMimic", "拡張子に従う"), QCoreApplication::translate("KomaMimic", "プレーンテキスト・青空文庫"), "Markdown"});
  choice(form, "markdownDialect", QCoreApplication::translate("KomaMimic", "Markdownの記法"),
         {"CommonMark", QCoreApplication::translate("KomaMimic", "GitHub形式")});
  check(form, "ruby", QCoreApplication::translate("KomaMimic", "ルビを表示（青空文庫のルビは常に解釈）"));
  check(form, "vertical", QCoreApplication::translate("KomaMimic", "TXTを縦書きにする"));
  auto *font = new QFontComboBox(&dialog);
  font->setObjectName("document_font");
  font->lineEdit()->setPlaceholderText(QCoreApplication::translate("KomaMimic", "未指定：文書・OSの既定フォント"));
  font->setToolTip(QCoreApplication::translate("KomaMimic", "一覧から選ぶかフォント名を入力します。空欄は文書の指定に従い"
                   "ます。固定レイアウトのフォントは変更しません。"));
  fields["font"] = font;
  form->addRow(QCoreApplication::translate("KomaMimic", "フォント名"), font);
  for (const auto &key : QStringList{"encoding", "foreground", "background"}) {
    auto *e = new QLineEdit(&dialog);
    e->setObjectName("document_" + key);
    fields[key] = e;
    form->addRow(
        QMap<QString, QString>{{"encoding", QCoreApplication::translate("KomaMimic", "文字コード（TXT / Markdown）")},
                               {"foreground", QCoreApplication::translate("KomaMimic", "文字色")},
                               {"background", QCoreApplication::translate("KomaMimic", "背景色")}}
            .value(key),
        e);
  }
  for (const auto &key : QStringList{"size", "margin", "spacing"}) {
    auto *s = new QSpinBox(&dialog);
    s->setObjectName("document_" + key);
    s->setRange(key == "size" ? 8 : 0, key == "size"     ? 120
                                       : key == "margin" ? 300
                                                         : 80);
    fields[key] = s;
    form->addRow(key == "size"     ? QCoreApplication::translate("KomaMimic", "文字サイズ（px）")
                 : key == "margin" ? QCoreApplication::translate("KomaMimic", "余白（px）")
                                   : QCoreApplication::translate("KomaMimic", "行間・列間（px）"),
                 s);
  }
  auto *sampleText = new QLineEdit("日本語の見本 AaBb 012345 — The quick brown fox", &dialog);
  sampleText->setObjectName("documentPreviewText"); sampleText->setAccessibleName(QCoreApplication::translate("KomaMimic", "見本の文字列"));
  layout->addWidget(sampleText);
  auto *previewScroll = new QScrollArea(&dialog); previewScroll->setWidgetResizable(true);
  previewScroll->setMinimumHeight(100); previewScroll->setMaximumHeight(160);
  auto *preview = new QLabel; preview->setObjectName("documentFontPreview");
  preview->setTextFormat(Qt::PlainText); preview->setWordWrap(true); preview->setMargin(10);
  previewScroll->setWidget(preview); layout->addWidget(previewScroll);
  const auto updatePreview = [fields, sampleText, preview] {
    auto *family = qobject_cast<QFontComboBox *>(fields.value("font"));
    QFont chosen = family->currentFont();
    chosen.setPixelSize(qobject_cast<QSpinBox *>(fields.value("size"))->value()); preview->setFont(chosen);
    auto colors = preview->palette();
    for (const auto &[key, role] : QVector<QPair<QString, QPalette::ColorRole>>{{"foreground", QPalette::WindowText}, {"background", QPalette::Window}}) {
      QColor color(qobject_cast<QLineEdit *>(fields.value(key))->text());
      if (color.isValid()) colors.setColor(role, color);
    }
    preview->setPalette(colors); preview->setAutoFillBackground(true); preview->setText(sampleText->text());
  };
  connect(font, &QFontComboBox::currentFontChanged, &dialog, updatePreview);
  connect(qobject_cast<QSpinBox *>(fields["size"]), &QSpinBox::valueChanged, &dialog, updatePreview);
  for (const auto &key : {"foreground", "background"})
    connect(qobject_cast<QLineEdit *>(fields[key]), &QLineEdit::textChanged, &dialog, updatePreview);
  connect(sampleText, &QLineEdit::textChanged, &dialog, updatePreview);
  auto *advanced = new QWidget(&dialog);
  auto *af = new QFormLayout(advanced);
  layout->addWidget(advanced);
  {
    choice(af, "pageLayout", QCoreApplication::translate("KomaMimic", "ページ配置"),
           {QCoreApplication::translate("KomaMimic", "単ページ"), QCoreApplication::translate("KomaMimic", "見開き（2ページ）")});
    check(af, "wideImages", QCoreApplication::translate("KomaMimic", "横長の画像ページを全幅表示"));
    check(af, "publisherStyles", QCoreApplication::translate("KomaMimic", "EPUBの出版社CSSを使用"));
    check(af, "embeddedFonts", QCoreApplication::translate("KomaMimic", "EPUBの埋め込みフォントを使用"));
    check(af, "overrideColors", QCoreApplication::translate("KomaMimic", "EPUBの配色を上書き"));
    choice(af, "writingMode", QCoreApplication::translate("KomaMimic", "組方向（固定レイアウトを除く）"),
           {QCoreApplication::translate("KomaMimic", "文書に従う（TXTは上の指定）"), QCoreApplication::translate("KomaMimic", "横書き"), QCoreApplication::translate("KomaMimic", "縦書き")});
  }
  auto *note = new QLabel(
      QCoreApplication::translate("KomaMimic", "固定レイアウトのフォント・文字サイズ・組方向は変更しませ"
      "ん。\n本ごとの指定 → 形式の既定値 → 全体の既定値の順に適用します。"),
      &dialog);
  note->setWordWrap(true);
  layout->addWidget(note);
  QVariantMap values;
  const auto reload = [&] {
    const int s = book ? scope->currentIndex() : 2;
    values = documentOptions(*m_settings, path, s == 0, s <= 1);
    for (auto i = fields.begin(); i != fields.end(); ++i) {
      const auto v = values.value(i.key());
      if (auto *c = qobject_cast<QFontComboBox *>(i.value()))
        c->setEditText(v.toString());
      else if (auto *c = qobject_cast<QComboBox *>(i.value()))
        c->setCurrentIndex(v.toInt());
      else if (auto *c = qobject_cast<QCheckBox *>(i.value()))
        c->setChecked(v.toBool());
      else if (auto *c = qobject_cast<QSpinBox *>(i.value()))
        c->setValue(v.toInt());
      else if (auto *c = qobject_cast<QLineEdit *>(i.value()))
        c->setText(v.toString());
    }
  };
  connect(scope, &QComboBox::currentIndexChanged, &dialog, reload);
  reload();
  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                               QDialogButtonBox::RestoreDefaults,
                           &dialog);
  outer->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  bool reset = false;
  connect(buttons->button(QDialogButtonBox::RestoreDefaults),
          &QPushButton::clicked, &dialog, [&] {
            reset = true;
            dialog.accept();
          });
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    for (const auto &key : QStringList{"foreground", "background"})
      if (!QColor(qobject_cast<QLineEdit *>(fields[key])->text()).isValid()) {
        QMessageBox::warning(&dialog, QCoreApplication::translate("KomaMimic", "配色"), QCoreApplication::translate("KomaMimic", "有効な色を指定してください。"));
        return;
      }
    dialog.accept();
  });
  updatePreview();
  if (dialog.exec() != QDialog::Accepted)
    return;
  const int s = book ? scope->currentIndex() : 2;
  const auto prefix = s == 0   ? documentBookKey(path)
                      : s == 1 ? documentFormatKey(path)
                               : QString("text/");
  const auto inherited = s == 0 ? documentOptions(*m_settings, path, false)
                         : s == 1
                             ? documentOptions(*m_settings, path, false, false)
                             : documentOptionDefaults();
  for (auto i = fields.begin(); i != fields.end(); ++i) {
    if (auto *c = qobject_cast<QFontComboBox *>(i.value()))
      values[i.key()] = c->currentText().trimmed();
    else if (auto *c = qobject_cast<QComboBox *>(i.value()))
      values[i.key()] = c->currentIndex();
    else if (auto *c = qobject_cast<QCheckBox *>(i.value()))
      values[i.key()] = c->isChecked();
    else if (auto *c = qobject_cast<QSpinBox *>(i.value()))
      values[i.key()] = c->value();
    else if (auto *c = qobject_cast<QLineEdit *>(i.value()))
      values[i.key()] = c->text();
  }
  if (reset)
    values = inherited;
  m_pendingDocumentLocation = currentDocumentLocation();
  m_restoreDocumentLocation = book && m_book;
  saveDocumentOptions(*m_settings, prefix, values, inherited);
  if (book)
    loadBook(path, m_mode);
  else
    updateDocumentActions();
}
void MainWindow::openDocumentView() {
  if (!m_documentView) {
    m_documentView = new DocumentView(m_stack);
    for (const auto &spec : shortcutSpecs())
      if (spec.scope == ShortcutScope::Reader) m_documentView->addAction(m_actions[spec.id]);
    m_stack->addWidget(m_documentView);
    connect(m_documentView, &DocumentView::nextRequested, this,
            &MainWindow::nextPage);
    connect(m_documentView, &DocumentView::previousRequested, this,
            &MainWindow::previousPage);
    connect(m_documentView, &DocumentView::laidOut, this,
            &MainWindow::documentLaidOut);
    connect(m_documentView, &DocumentView::positionChanged, this,
            [this](int page, const DocumentLocation &) {
              if (!documentViewActive())
                return;
              m_anchor = page;
              m_renderReady = true;
              saveDocumentLocation();
              if (m_features)
                m_features->rendered(m_features->visiblePagesForView(), {});
              refreshState();
            });
    connect(m_documentView, &DocumentView::failed, this,
            [this](const QString &error) {
              m_lastError = error;
              m_renderReady = false;
              m_view->setMessage(error);
              if (m_mode == ViewMode::Reader)
                m_stack->setCurrentWidget(m_view);
              statusBar()->showMessage(error);
            });
  }
  const auto location = m_restoreDocumentLocation ? m_pendingDocumentLocation
                                                  : currentDocumentLocation();
  m_restoreDocumentLocation = false;
  m_documentView->open(m_book->publication, m_book->openOptions.text, location);
  setMode(m_mode);
}
void MainWindow::documentLaidOut(const QVector<DocumentLocation> &locations) {
  if (!documentViewActive() || locations.isEmpty())
    return;
  const auto old = currentDocumentLocation();
  QVector<BookPage> pages;
  m_documentWidePages.clear(); m_documentSoloPages.clear(); m_documentSides.clear();
  for (const auto &location : locations) {
    if (location.wide) m_documentWidePages.insert(pages.size());
    if (location.solo) m_documentSoloPages.insert(pages.size());
    if (location.side) m_documentSides.insert(pages.size(),location.side);
    BookPage p;
    p.name = m_book->publication->title;
    p.filePath = m_currentPath;
    p.document = true;
    p.documentSection = location.section;
    p.documentOffset = location.offset;
    p.identity =
        "text:" + location.section + ':' + QString::number(location.offset);
    pages.append(p);
  }
  m_book = m_book->withPages(pages);
  m_anchor = resolveDocumentLocation(old);
  m_renderReady = true;
  populatePages();
  if (m_features)
    m_features->refreshDocumentToc();
  refreshState();
}

void MainWindow::clearDocumentView() {
  m_documentWidePages.clear(); m_documentSoloPages.clear(); m_documentSides.clear();
  if (m_documentView) {
    m_documentView->clear();
    m_stack->removeWidget(m_documentView);
    delete m_documentView;
    m_documentView = nullptr;
  }
}
