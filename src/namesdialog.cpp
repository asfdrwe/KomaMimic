// SPDX-License-Identifier: GPL-3.0-only
#include "namesdialog.h"
#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QSet>
NamesDialog::NamesDialog(std::shared_ptr<const Book> book, QWidget *parent) : QDialog(parent), m_book(std::move(book)) {
    setObjectName("archiveNamesDialog"); setWindowTitle(QCoreApplication::translate("KomaMimic", "書庫内の名前の文字コード")); resize(940, 560);
    auto *layout = new QVBoxLayout(this);
    auto *help = new QLabel(QCoreApplication::translate("KomaMimic", "宣言のない名前は既定でCP932と仮定します。正常なUnicode情報を優先します。\n変更はこの書庫に記憶し、元ファイルを書き換えません。プレビューは先頭200項目です。"), this);
    help->setWordWrap(true); layout->addWidget(help);
    m_encoding = new QComboBox(this); m_encoding->setObjectName("archiveEncoding");
    m_encoding->addItems(legacyEncodings()); m_encoding->setCurrentText(m_book->namePolicy.legacyEncoding);
    layout->addWidget(m_encoding);
    m_force = new QCheckBox(QCoreApplication::translate("KomaMimic", "Unicode・charsetの宣言も無視し、指定した文字コードで強制再解釈する"), this);
    m_force->setObjectName("forceArchiveEncoding"); m_force->setChecked(m_book->namePolicy.force); layout->addWidget(m_force);
    m_table = new QTableWidget(this); m_table->setObjectName("archiveNamesPreview");
    m_table->setColumnCount(4); m_table->setHorizontalHeaderLabels({QCoreApplication::translate("KomaMimic", "表示名"), QCoreApplication::translate("KomaMimic", "解釈の根拠"), QCoreApplication::translate("KomaMimic", "文字コード"), QCoreApplication::translate("KomaMimic", "元の名前byte（16進）")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(m_table);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Save)->setObjectName("saveArchiveEncoding");
    buttons->button(QDialogButtonBox::Save)->setText(QCoreApplication::translate("KomaMimic", "適用して記憶"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("KomaMimic", "キャンセル"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject); layout->addWidget(buttons);
    connect(m_encoding, &QComboBox::currentTextChanged, this, &NamesDialog::preview);
    connect(m_force, &QCheckBox::toggled, this, &NamesDialog::preview);
    preview();
}
NamePolicy NamesDialog::policy() const { return {m_encoding->currentText(), m_force->isChecked()}; }
void NamesDialog::preview() {
    m_table->setRowCount(0); QSet<qint64> seen;
    for (const auto &page : m_book->pages) {
        if (seen.contains(page.archiveIndex)) continue;
        seen.insert(page.archiveIndex);
        const auto name = decodeName(page.sourceName, policy());
        const int row = m_table->rowCount(); m_table->insertRow(row);
        const QStringList columns = {name.text, name.origin, name.encoding, QString::fromLatin1(page.sourceName.rawName.left(80).toHex(' '))};
        for (int c = 0; c < columns.size(); ++c) {
            auto *item = new QTableWidgetItem(columns[c]);
            item->setToolTip(name.warning.isEmpty() ? name.origin : name.warning);
            m_table->setItem(row, c, item);
        }
        if (m_table->rowCount() >= 200) break;
    }
}
