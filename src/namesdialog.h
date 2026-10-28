// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "book.h"
#include <QDialog>
class QComboBox;
class QCheckBox;
class QTableWidget;
class NamesDialog : public QDialog {
    Q_OBJECT
public:
    explicit NamesDialog(std::shared_ptr<const Book> book, QWidget *parent = nullptr);
    NamePolicy policy() const;
private:
    void preview();
    std::shared_ptr<const Book> m_book;
    QComboBox *m_encoding;
    QCheckBox *m_force;
    QTableWidget *m_table;
};
