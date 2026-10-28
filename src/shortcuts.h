// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QDialog>
#include <QHash>
#include <QKeySequence>
#include <QVector>
class QKeySequenceEdit;
class QLabel;
class QPushButton;
class QComboBox;
class QSettings;
enum class ShortcutScope { Global, Reader, Thumbnails, Tree };
struct ShortcutSpec { QString id, label; QKeySequence defaultKey; ShortcutScope scope = ShortcutScope::Global; };
QString commandCategory(const QString &id);
QString commandCategoryName(const QString &id);
QString shortcutScopeName(ShortcutScope scope);
QVector<ShortcutSpec> shortcutSpecs();
QKeySequence defaultFullscreenShortcut();
QString shortcutConflict(const QHash<QString, QKeySequence> &bindings);
QHash<QString, QKeySequence> documentKeyBindings(const QSettings &settings);
QString documentShortcutConflict(const QHash<QString, QKeySequence> &base,
                                 const QHash<QString, QKeySequence> &overrides);
class ShortcutDialog : public QDialog {
    Q_OBJECT
public:
    explicit ShortcutDialog(const QHash<QString, QKeySequence> &bindings, QWidget *parent = nullptr,
                            const QHash<QString, QKeySequence> &document = {});
    QHash<QString, QKeySequence> bindings() const;
    QHash<QString, QKeySequence> documentBindings() const;
private:
    void validate();
    QHash<QString, QKeySequenceEdit *> m_editors;
    QHash<QString, QKeySequenceEdit *> m_documentEditors;
    QHash<QString, QComboBox *> m_documentModes;
    QLabel *m_error = nullptr;
    QPushButton *m_save = nullptr;
};

// MangaMeeya VK + Shift(0x100), Ctrl(0x200), Alt(0x400). Empty if unsupported.
QKeySequence legacyKeySequence(int value);
