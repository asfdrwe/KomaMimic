// SPDX-License-Identifier: GPL-3.0-only
#include "shortcuts.h"
#include <QCoreApplication>
#include "readerfeatures.h"
#include <QDialogButtonBox>
#include <QComboBox>
#include <QCheckBox>
#include <QSettings>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
QKeySequence defaultFullscreenShortcut() {
#ifdef Q_OS_MACOS
    // Qt Ctrl maps to Command and Meta maps to physical Control on macOS.
    return QKeySequence("Ctrl+Meta+F");
#else
    return QKeySequence("F11");
#endif
}
QVector<ShortcutSpec> shortcutSpecs() {
    return QVector<ShortcutSpec>{
        {"open", QCoreApplication::translate("KomaMimic", "画像・書庫を開く"), QKeySequence("Ctrl+O")},
        {"folder", QCoreApplication::translate("KomaMimic", "フォルダを選択"), QKeySequence("Ctrl+Shift+O")},
        {"next", QCoreApplication::translate("KomaMimic", "次のページ（見開き単位）"), QKeySequence("Right"), ShortcutScope::Reader},
        {"previous", QCoreApplication::translate("KomaMimic", "前のページ（見開き単位）"), QKeySequence("Left"), ShortcutScope::Reader},
        {"nextOne", QCoreApplication::translate("KomaMimic", "1ページだけ進む"), QKeySequence("Down"), ShortcutScope::Reader},
        {"previousOne", QCoreApplication::translate("KomaMimic", "1ページだけ戻る"), QKeySequence("Up"), ShortcutScope::Reader},
        {"first", QCoreApplication::translate("KomaMimic", "先頭ページ"), QKeySequence("Home"), ShortcutScope::Reader},
        {"last", QCoreApplication::translate("KomaMimic", "最終ページ"), QKeySequence("End"), ShortcutScope::Reader},
        {"nextBook", QCoreApplication::translate("KomaMimic", "次の書庫"), QKeySequence("Shift+Right")},
        {"previousBook", QCoreApplication::translate("KomaMimic", "前の書庫"), QKeySequence("Shift+Left")},
        {"boundaryStop", QCoreApplication::translate("KomaMimic", "非リピート"), QKeySequence()},
        {"boundaryRepeat", QCoreApplication::translate("KomaMimic", "リピート"), QKeySequence()},
        {"boundaryAdvance", QCoreApplication::translate("KomaMimic", "フォルダ移動自動式"), QKeySequence()},
        {"exitFullscreen", QCoreApplication::translate("KomaMimic", "全画面を解除（読書画面）"), QKeySequence("Escape"), ShortcutScope::Reader},
        {"spread", QCoreApplication::translate("KomaMimic", "単ページ／見開き"), QKeySequence(), ShortcutScope::Reader},
        {"rtl", QCoreApplication::translate("KomaMimic", "右開き／左開き"), QKeySequence("\\"), ShortcutScope::Reader},
        {"cover", QCoreApplication::translate("KomaMimic", "表紙を単ページにする"), QKeySequence(), ShortcutScope::Reader},
        {"library", QCoreApplication::translate("KomaMimic", "表紙サムネイル／読書画面"), QKeySequence("Shift+Down")},
        {"shortcuts", QCoreApplication::translate("KomaMimic", "ショートカット設定"), QKeySequence("Ctrl+K")},
        {"archiveNames", QCoreApplication::translate("KomaMimic", "書庫内の名前の文字コード"), QKeySequence()},
        {"mediaPause", QCoreApplication::translate("KomaMimic", "アニメーションを一時停止／再生"), QKeySequence(), ShortcutScope::Reader},
        {"mediaStop", QCoreApplication::translate("KomaMimic", "アニメーションを先頭で停止"), QKeySequence(), ShortcutScope::Reader},
        {"mediaRestart", QCoreApplication::translate("KomaMimic", "アニメーションを先頭から再生"), QKeySequence(), ShortcutScope::Reader},
        {"mediaRepeat", QCoreApplication::translate("KomaMimic", "アニメーションを常に繰り返す"), QKeySequence(), ShortcutScope::Reader},
        {"tree", QCoreApplication::translate("KomaMimic", "フォルダツリーの表示切替"), QKeySequence("Ctrl+T")},
        {"focusTree", QCoreApplication::translate("KomaMimic", "フォルダツリーへフォーカス"), QKeySequence()},
        {"focusThumbnails", QCoreApplication::translate("KomaMimic", "サムネイルへフォーカス"), QKeySequence()},
        {"parentFolder", QCoreApplication::translate("KomaMimic", "上のフォルダへ"), QKeySequence("Shift+Up")},
        {"folderBack", QCoreApplication::translate("KomaMimic", "前に開いたフォルダへ戻る"), QKeySequence()},
        {"folderForward", QCoreApplication::translate("KomaMimic", "フォルダの移動をやり直す"), QKeySequence()},
        {"previousFolder", QCoreApplication::translate("KomaMimic", "前の同階層フォルダへ"), QKeySequence("Ctrl+PgUp")},
        {"nextFolder", QCoreApplication::translate("KomaMimic", "次の同階層フォルダへ"), QKeySequence("Ctrl+PgDown")},
        {"readFolder", QCoreApplication::translate("KomaMimic", "現在のフォルダの画像を読む"), QKeySequence("Ctrl+Return")},
        {"thumbnails", QCoreApplication::translate("KomaMimic", "画像・書庫内のページサムネイル"), QKeySequence("Ctrl+E")},
        {"fullscreenGlobal", QCoreApplication::translate("KomaMimic", "全画面切替"), defaultFullscreenShortcut()},
        {"thumbAccept", QCoreApplication::translate("KomaMimic", "サムネイル決定・選択項目を開く"), QKeySequence("Return"), ShortcutScope::Thumbnails},
        {"thumbCancel", QCoreApplication::translate("KomaMimic", "サムネイルを閉じる・読書に戻る"), QKeySequence("Escape"), ShortcutScope::Thumbnails},
        {"thumbLeft", QCoreApplication::translate("KomaMimic", "サムネイル選択：左"), QKeySequence("Left"), ShortcutScope::Thumbnails},
        {"thumbRight", QCoreApplication::translate("KomaMimic", "サムネイル選択：右"), QKeySequence("Right"), ShortcutScope::Thumbnails},
        {"thumbUp", QCoreApplication::translate("KomaMimic", "サムネイル選択：上"), QKeySequence("Up"), ShortcutScope::Thumbnails},
        {"thumbDown", QCoreApplication::translate("KomaMimic", "サムネイル選択：下"), QKeySequence("Down"), ShortcutScope::Thumbnails},
        {"thumbFirst", QCoreApplication::translate("KomaMimic", "サムネイル選択：先頭"), QKeySequence("Home"), ShortcutScope::Thumbnails},
        {"thumbLast", QCoreApplication::translate("KomaMimic", "サムネイル選択：末尾"), QKeySequence("End"), ShortcutScope::Thumbnails},
        {"thumbPageUp", QCoreApplication::translate("KomaMimic", "サムネイル選択：1画面上"), QKeySequence("PgUp"), ShortcutScope::Thumbnails},
        {"thumbPageDown", QCoreApplication::translate("KomaMimic", "サムネイル選択：1画面下"), QKeySequence("PgDown"), ShortcutScope::Thumbnails},
        {"thumbRefresh", QCoreApplication::translate("KomaMimic", "サムネイルを再取得"), QKeySequence("F6"), ShortcutScope::Thumbnails},
        {"thumbFit", QCoreApplication::translate("KomaMimic", "サムネイル：1画面"), QKeySequence("A")},
        {"thumbEight", QCoreApplication::translate("KomaMimic", "サムネイル：8列"), QKeySequence("H")},
        {"thumbCustom", QCoreApplication::translate("KomaMimic", "サムネイル：列数を指定"), QKeySequence("T")},
        {"treeUp", QCoreApplication::translate("KomaMimic", "ツリー選択：上"), QKeySequence("Up"), ShortcutScope::Tree},
        {"treeDown", QCoreApplication::translate("KomaMimic", "ツリー選択：下"), QKeySequence("Down"), ShortcutScope::Tree},
        {"treeLeft", QCoreApplication::translate("KomaMimic", "ツリーを閉じる・親を選択"), QKeySequence("Left"), ShortcutScope::Tree},
        {"treeRight", QCoreApplication::translate("KomaMimic", "ツリーを開く・子を選択"), QKeySequence("Right"), ShortcutScope::Tree},
        {"treeAccept", QCoreApplication::translate("KomaMimic", "ツリーのフォルダを開く"), QKeySequence("Return"), ShortcutScope::Tree},
        {"treeCancel", QCoreApplication::translate("KomaMimic", "ツリーからサムネイルに戻る"), QKeySequence("Escape"), ShortcutScope::Tree},
        {"scaleFit", QCoreApplication::translate("KomaMimic", "ウィンドウに合わせる"), QKeySequence("X"), ShortcutScope::Reader},
        {"scaleWidth", QCoreApplication::translate("KomaMimic", "幅に合わせる"), QKeySequence(), ShortcutScope::Reader},
        {"scaleHeight", QCoreApplication::translate("KomaMimic", "高さに合わせる"), QKeySequence(), ShortcutScope::Reader},
        {"scaleActual", QCoreApplication::translate("KomaMimic", "原寸（100%）"), QKeySequence("C"), ShortcutScope::Reader},
        {"zoomIn", QCoreApplication::translate("KomaMimic", "拡大"), QKeySequence("Num++"), ShortcutScope::Reader},
        {"zoomOut", QCoreApplication::translate("KomaMimic", "縮小"), QKeySequence("Num+-"), ShortcutScope::Reader},
        {"noUpscale", QCoreApplication::translate("KomaMimic", "縮小はするが拡大はしない"), QKeySequence(), ShortcutScope::Reader},
        {"resizeSmooth", QCoreApplication::translate("KomaMimic", "HALFTONE相当（近似）"), QKeySequence(), ShortcutScope::Reader},
        {"resizeArea", QCoreApplication::translate("KomaMimic", "平均画素法"), QKeySequence(), ShortcutScope::Reader},
        {"resizeLanczos", "Lanczos 3", QKeySequence(), ShortcutScope::Reader},
        {"resizeBicubic", QCoreApplication::translate("KomaMimic", "バイキュービック"), QKeySequence(), ShortcutScope::Reader},
        {"resizeBilinear", QCoreApplication::translate("KomaMimic", "線形補間"), QKeySequence(), ShortcutScope::Reader},
        {"resizeAreaWeak", QCoreApplication::translate("KomaMimic", "平均画素法＋シャープ弱"), QKeySequence(), ShortcutScope::Reader},
        {"resizeAreaStrong", QCoreApplication::translate("KomaMimic", "平均画素法＋シャープ強"), QKeySequence(), ShortcutScope::Reader},
        {"resizeNearest", QCoreApplication::translate("KomaMimic", "最近傍（補間なし）"), QKeySequence(), ShortcutScope::Reader},
        {"filterEnabled", QCoreApplication::translate("KomaMimic", "画像加工を有効"), QKeySequence("F"), ShortcutScope::Reader},
        {"filterSettings", QCoreApplication::translate("KomaMimic", "画像加工の設定"), QKeySequence("G"), ShortcutScope::Reader},
        {"filterReset", QCoreApplication::translate("KomaMimic", "画像加工をすべてリセット"), QKeySequence(), ShortcutScope::Reader},
        {"rotateRight", QCoreApplication::translate("KomaMimic", "右に90度回転"), QKeySequence("]"), ShortcutScope::Reader},
        {"rotateLeft", QCoreApplication::translate("KomaMimic", "左に90度回転"), QKeySequence("["), ShortcutScope::Reader},
        {"rotateHalf", QCoreApplication::translate("KomaMimic", "180度回転"), QKeySequence(), ShortcutScope::Reader},
        {"filterFlipH", QCoreApplication::translate("KomaMimic", "画像を左右反転"), QKeySequence(), ShortcutScope::Reader},
        {"filterFlipV", QCoreApplication::translate("KomaMimic", "画像を上下反転"), QKeySequence(), ShortcutScope::Reader},
        {"filterGray", QCoreApplication::translate("KomaMimic", "グレースケール"), QKeySequence(), ShortcutScope::Reader},
        {"filterInvert", QCoreApplication::translate("KomaMimic", "色反転"), QKeySequence(), ShortcutScope::Reader},
        {"filterBinary", QCoreApplication::translate("KomaMimic", "2値化"), QKeySequence(), ShortcutScope::Reader},
        {"filterAutoCrop", QCoreApplication::translate("KomaMimic", "白い余白を自動除去"), QKeySequence(), ShortcutScope::Reader},
        {"sharpenNone", QCoreApplication::translate("KomaMimic", "シャープ化：なし"), QKeySequence(), ShortcutScope::Reader},
        {"sharpenWeak", QCoreApplication::translate("KomaMimic", "シャープ化：弱"), QKeySequence(), ShortcutScope::Reader},
        {"sharpenStrong", QCoreApplication::translate("KomaMimic", "シャープ化：強"), QKeySequence(), ShortcutScope::Reader},
        {"quit", QCoreApplication::translate("KomaMimic", "終了"), QKeySequence("Ctrl+Q")}
    } + featureShortcutSpecs();
}
QString commandCategory(const QString &id) {
    if (id.startsWith("document") || id.startsWith("textSize") || id == "findDocument") return "documents";
    if (id.startsWith("thumb") || id.startsWith("tree") || id.startsWith("focus")) return "panes";
    if (id.startsWith("scale") || id.startsWith("resize") || id.startsWith("zoom") || id.startsWith("filter") || id.startsWith("rotate") || id.startsWith("sharpen") || id.startsWith("profile") || id.startsWith("saveProfile") || id.startsWith("loupe")) return "image";
    if (id.startsWith("edit") || id.startsWith("select") || id.startsWith("save") || id.startsWith("quickSave") || id == "insertBlank" || id == "addPages" || id.startsWith("imageC") || id == "imagePaste" || id == "trashPages") return "editing";
    if (id.contains("Playlist") || id.contains("Favorite") || id.contains("Shelf") || id.startsWith("playlist") || id.startsWith("favorite") || id.startsWith("history") || id.startsWith("list") || id.startsWith("book") || id.startsWith("marker") || id.startsWith("register") || id.contains("Toc") || id == "toc") return "lists";
    if (id.startsWith("next") || id.startsWith("previous") || id.startsWith("jump") || id.startsWith("scroll") || id.startsWith("wheel") || id.startsWith("boundary") || id == "first" || id == "last" || id == "goPage" || id.startsWith("slide") || id.startsWith("media") || id == "timedList") return "navigation";
    return "general";
}
QString commandCategoryName(const QString &id) {
    return QHash<QString, QString>{{"general", QCoreApplication::translate("KomaMimic", "ファイル・一般")}, {"navigation", QCoreApplication::translate("KomaMimic", "移動・再生")}, {"panes", QCoreApplication::translate("KomaMimic", "一覧・ペイン")}, {"image", QCoreApplication::translate("KomaMimic", "画像・倍率")}, {"editing", QCoreApplication::translate("KomaMimic", "編集・保存")}, {"lists", QCoreApplication::translate("KomaMimic", "ブックリスト・目次")}, {"documents", QCoreApplication::translate("KomaMimic", "文書")}}.value(id, id);
}
QString shortcutScopeName(ShortcutScope scope) {
    switch (scope) {
    case ShortcutScope::Global: return QCoreApplication::translate("KomaMimic", "共通");
    case ShortcutScope::Reader: return QCoreApplication::translate("KomaMimic", "読書画面");
    case ShortcutScope::Thumbnails: return QCoreApplication::translate("KomaMimic", "サムネイル");
    case ShortcutScope::Tree: return QCoreApplication::translate("KomaMimic", "フォルダツリー");
    }
    return {};
}
QString shortcutConflict(const QHash<QString, QKeySequence> &bindings) {
    const auto specs = shortcutSpecs();
    for (int i = 0; i < specs.size(); ++i) {
        const auto key = bindings.value(specs[i].id);
        if (key.isEmpty()) continue;
        if (key.count() > 1) return QCoreApplication::translate("KomaMimic", "操作ごとに1組のキーを設定してください。");
        for (int j = i + 1; j < specs.size(); ++j) {
            if (specs[i].scope != specs[j].scope && specs[i].scope != ShortcutScope::Global && specs[j].scope != ShortcutScope::Global) continue;
            const auto other = bindings.value(specs[j].id);
            // Keep explicitly saved shared Return bindings from older profiles readable.
            const bool acceptPair=(specs[i].id=="fullscreenGlobal" && (specs[j].id=="thumbAccept" || specs[j].id=="treeAccept"))
                || (specs[j].id=="fullscreenGlobal" && (specs[i].id=="thumbAccept" || specs[i].id=="treeAccept"));
            if (acceptPair && key==QKeySequence("Return") && other==key) continue;
            if (!other.isEmpty() && key.matches(other) != QKeySequence::NoMatch)
                return QCoreApplication::translate("KomaMimic", "「%1」と「%2」のキーが重複しています。").arg(specs[i].label, specs[j].label);
        }
    }
    return {};
}
QHash<QString, QKeySequence> documentKeyBindings(const QSettings &settings) {
    QHash<QString, QKeySequence> result;
    for (const auto &spec : shortcutSpecs())
        if (spec.scope == ShortcutScope::Reader && settings.contains("documentKeys/" + spec.id))
            result[spec.id] = QKeySequence::fromString(settings.value("documentKeys/" + spec.id).toString(), QKeySequence::PortableText);
    return result;
}
QString documentShortcutConflict(const QHash<QString, QKeySequence> &base,
                                 const QHash<QString, QKeySequence> &overrides) {
    auto effective = base;
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        bool allowed = false;
        for (const auto &spec : shortcutSpecs()) if (spec.id == it.key() && spec.scope == ShortcutScope::Reader) allowed = true;
        if (!allowed) return QCoreApplication::translate("KomaMimic", "文書用の上書きは読書画面の操作に指定してください。");
        effective[it.key()] = it.value();
    }
    return shortcutConflict(effective);
}
ShortcutDialog::ShortcutDialog(const QHash<QString, QKeySequence> &bindings, QWidget *parent,
                                const QHash<QString, QKeySequence> &document)
    : QDialog(parent) {
    setObjectName("shortcutDialog");
    setWindowTitle(QCoreApplication::translate("KomaMimic", "キーボードショートカット"));
    resize(1000, 650);
    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(QCoreApplication::translate("KomaMimic", "キー欄を選んでキーを押してください。空欄は割り当てなしです。\n読書・サムネイル・ツリーで同じキーを別の操作に使えます。共通のキーはすべての画面で有効です。"));
    description->setWordWrap(true);
    layout->addWidget(description);
    const auto specs = shortcutSpecs();
    auto *table = new QTableWidget(specs.size(), 5, this);
    table->setHorizontalHeaderLabels({QCoreApplication::translate("KomaMimic", "操作"), QCoreApplication::translate("KomaMimic", "共通キー"), QCoreApplication::translate("KomaMimic", "対象"), QCoreApplication::translate("KomaMimic", "文書表示での上書き"), QCoreApplication::translate("KomaMimic", "文書用キー")});
    auto *showDocument = new QCheckBox(QCoreApplication::translate("KomaMimic", "文書用のキー設定を表示"), this);
    showDocument->setObjectName("showDocumentKeys");
    showDocument->setChecked(!document.isEmpty());
    layout->addWidget(showDocument);
    table->setColumnHidden(3, !showDocument->isChecked());
    table->setColumnHidden(4, !showDocument->isChecked());
    connect(showDocument, &QCheckBox::toggled, this, [table](bool on) {table->setColumnHidden(3,!on);table->setColumnHidden(4,!on);});
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (int row = 0; row < specs.size(); ++row) {
        const auto &spec = specs[row];
        table->setItem(row, 0, new QTableWidgetItem(spec.label));
        table->setItem(row, 2, new QTableWidgetItem(shortcutScopeName(spec.scope)));
        auto *editor = new QKeySequenceEdit(bindings.value(spec.id), table);
        editor->setObjectName("key_" + spec.id);
        editor->setMaximumSequenceLength(1);
        editor->setClearButtonEnabled(true);
        editor->setFinishingKeyCombinations({});
        editor->setAccessibleName(spec.label);
        table->setCellWidget(row, 1, editor);
        m_editors.insert(spec.id, editor);
        connect(editor, &QKeySequenceEdit::keySequenceChanged, this, [this] { validate(); });
        if (spec.scope == ShortcutScope::Reader) {
            auto *mode = new QComboBox(table);
            mode->setObjectName("documentKeyMode_" + spec.id);
            mode->addItems({QCoreApplication::translate("KomaMimic", "共通設定を使う"), QCoreApplication::translate("KomaMimic", "別のキーを指定"), QCoreApplication::translate("KomaMimic", "割り当てなし")});
            auto *doc = new QKeySequenceEdit(document.value(spec.id), table);
            doc->setObjectName("documentKey_" + spec.id);
            doc->setMaximumSequenceLength(1);
            doc->setClearButtonEnabled(true);
            doc->setFinishingKeyCombinations({});
            mode->setCurrentIndex(!document.contains(spec.id) ? 0 : document.value(spec.id).isEmpty() ? 2 : 1);
            doc->setEnabled(mode->currentIndex() == 1);
            table->setCellWidget(row, 3, mode); table->setCellWidget(row, 4, doc);
            m_documentEditors[spec.id] = doc; m_documentModes[spec.id] = mode;
            connect(mode, &QComboBox::currentIndexChanged, this, [this, doc](int index) {doc->setEnabled(index == 1); validate();});
            connect(doc, &QKeySequenceEdit::keySequenceChanged, this, [this] {validate();});
        }
    }
    auto *category = new QComboBox(this); category->setObjectName("shortcutCategory");
    category->addItem(QCoreApplication::translate("KomaMimic", "すべての操作"), "all");
    for (const auto &id : {"general", "navigation", "panes", "image", "editing", "lists", "documents"}) category->addItem(commandCategoryName(id), id);
    layout->addWidget(category);
    auto *search = new QLineEdit(this); search->setObjectName("shortcutSearch");
    search->setPlaceholderText(QCoreApplication::translate("KomaMimic", "操作名・対象で絞り込む")); search->setClearButtonEnabled(true); layout->addWidget(search);
    const auto filter = [table, specs, search, category] {
        for (int row = 0; row < specs.size(); ++row) {
            const auto &spec = specs[row]; const auto query = search->text();
            const bool matches = spec.label.contains(query, Qt::CaseInsensitive) || spec.id.contains(query, Qt::CaseInsensitive) || shortcutScopeName(spec.scope).contains(query, Qt::CaseInsensitive);
            table->setRowHidden(row, !matches || (category->currentData() != "all" && category->currentData().toString() != commandCategory(spec.id)));
        }
    };
    connect(search, &QLineEdit::textChanged, this, filter);
    connect(category, &QComboBox::currentIndexChanged, this, filter);
    layout->addWidget(table);
    m_error = new QLabel(this);
    m_error->setObjectName("shortcutError");
    m_error->setWordWrap(true);
    m_error->setStyleSheet("color: #bf3636;");
    layout->addWidget(m_error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    m_save = buttons->button(QDialogButtonBox::Save);
    m_save->setText(QCoreApplication::translate("KomaMimic", "保存"));
    m_save->setObjectName("saveShortcuts");
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("KomaMimic", "キャンセル"));
    auto *reset = buttons->addButton(QCoreApplication::translate("KomaMimic", "すべて初期値に戻す"), QDialogButtonBox::ResetRole);
    auto *resetVisible = buttons->addButton(QCoreApplication::translate("KomaMimic", "表示中の操作を初期値に戻す"), QDialogButtonBox::ResetRole);
    resetVisible->setObjectName("resetVisibleShortcuts");
    connect(resetVisible, &QPushButton::clicked, this, [this, table, specs] {
        for (int row = 0; row < specs.size(); ++row) if (!table->isRowHidden(row)) {
            m_editors.value(specs[row].id)->setKeySequence(specs[row].defaultKey);
            if (auto *mode = m_documentModes.value(specs[row].id)) mode->setCurrentIndex(0);
        }
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        for (const auto &spec : shortcutSpecs()) m_editors.value(spec.id)->setKeySequence(spec.defaultKey);
        for (auto *mode : m_documentModes) mode->setCurrentIndex(0);
    });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { validate(); if (m_save->isEnabled()) accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    validate();
}
QHash<QString, QKeySequence> ShortcutDialog::bindings() const {
    QHash<QString, QKeySequence> result;
    for (auto it = m_editors.cbegin(); it != m_editors.cend(); ++it) result.insert(it.key(), it.value()->keySequence());
    return result;
}
void ShortcutDialog::validate() {
    if (!m_error) return;
    QString error = shortcutConflict(bindings());
    if (error.isEmpty()) error = documentShortcutConflict(bindings(), documentBindings());
    for (auto it = m_documentModes.cbegin(); it != m_documentModes.cend(); ++it)
        if (it.value()->currentIndex() == 1 && m_documentEditors[it.key()]->keySequence().isEmpty())
            error = QCoreApplication::translate("KomaMimic", "文書用キーを指定するか「割り当てなし」を選んでください。");
    m_error->setText(error);
    m_save->setEnabled(error.isEmpty());
}
QHash<QString, QKeySequence> ShortcutDialog::documentBindings() const {
    QHash<QString, QKeySequence> result;
    for (auto it = m_documentModes.cbegin(); it != m_documentModes.cend(); ++it) {
        if (it.value()->currentIndex() == 1) result[it.key()] = m_documentEditors[it.key()]->keySequence();
        else if (it.value()->currentIndex() == 2) result[it.key()] = {};
    }
    return result;
}

QKeySequence legacyKeySequence(int value) {
    if(value==0)return {};
    if(value<0 || (value & ~0x7ff))return {};
    const int vk=value & 255;
    int key=0;
    if((vk>='A' && vk<='Z') || (vk>='0' && vk<='9'))key=vk;
    else if(vk>=0x70 && vk<=0x87)key=Qt::Key_F1+vk-0x70;
    else if(vk>=0x60 && vk<=0x69)key=Qt::Key_0+vk-0x60;
    else key=QHash<int,int>{{8,Qt::Key_Backspace},{9,Qt::Key_Tab},{13,Qt::Key_Return},{27,Qt::Key_Escape},
        {32,Qt::Key_Space},{33,Qt::Key_PageUp},{34,Qt::Key_PageDown},{35,Qt::Key_End},{36,Qt::Key_Home},
        {37,Qt::Key_Left},{38,Qt::Key_Up},{39,Qt::Key_Right},{40,Qt::Key_Down},{45,Qt::Key_Insert},{46,Qt::Key_Delete},
        {106,Qt::Key_Asterisk},{107,Qt::Key_Plus},{109,Qt::Key_Minus},{110,Qt::Key_Period},{111,Qt::Key_Slash}}.value(vk);
    // OEM punctuation is keyboard-layout dependent: preserve it in the report.
    if(!key)return {};
    Qt::KeyboardModifiers mods;
    if(value&0x100)mods|=Qt::ShiftModifier;
    if(value&0x200)mods|=Qt::ControlModifier;
    if(value&0x400)mods|=Qt::AltModifier;
    if(vk>=0x60 && vk<=0x6f)mods|=Qt::KeypadModifier;
    return QKeySequence(QKeyCombination(mods,Qt::Key(key)));
}
