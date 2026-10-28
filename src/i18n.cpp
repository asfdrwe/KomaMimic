// SPDX-License-Identifier: GPL-3.0-only
#include "i18n.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLibraryInfo>
#include <QLocale>
#include <QPointer>
#include <QStandardPaths>
#include <QSettings>
#include <QTranslator>

static void initializeCatalogResources() {
    static const bool initialized = [] { Q_INIT_RESOURCE(i18n_catalogs); return true; }();
    Q_UNUSED(initialized);
}
namespace {
QString language = "ja";
QPointer<QTranslator> applicationTranslator, fallbackTranslator, qtTranslator;
}
QVector<I18n::Language> I18n::languages() {
    initializeCatalogResources();
    QFile file(":/i18n/languages.json");
    QVector<Language> result;
    if (file.open(QIODevice::ReadOnly))
        for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
            const auto code = value.toString();
            const QLocale locale(code);
            auto name = locale.nativeLanguageName();
            if (name.isEmpty()) name = code;
            if (code.contains('_') || code.contains('-')) name += " (" + locale.nativeTerritoryName() + ')';
            result.append({code, name});
        }
    return result;
}
QString I18n::resolve(const QString &selection, const QStringList &preferences) {
    const auto supported = languages();
    const auto match = [&](QString code) -> QString {
        code.replace('-', '_');
        for (const auto &entry : supported)
            if (QString(entry.code).replace('-', '_').compare(code, Qt::CaseInsensitive) == 0) return entry.code;
        const auto base = code.section('_', 0, 0);
        for (const auto &entry : supported)
            if (entry.code.compare(base, Qt::CaseInsensitive) == 0) return entry.code;
        for (const auto &entry : supported)
            if (QString(entry.code).replace('-', '_').section('_', 0, 0).compare(base, Qt::CaseInsensitive) == 0) return entry.code;
        return {};
    };
    if (selection != "system") {
        const auto explicitLanguage = match(selection);
        if (!explicitLanguage.isEmpty()) return explicitLanguage;
    }
    for (const auto &preference : preferences) {
        const auto code = match(preference);
        if (!code.isEmpty()) return code;
    }
    return "en";
}
QString I18n::initialize(const QString &selection) {
    delete applicationTranslator; delete fallbackTranslator; delete qtTranslator;
    language = resolve(selection, QLocale::system().uiLanguages());
    if (language != "en") {
        qtTranslator = new QTranslator(QCoreApplication::instance());
        if (qtTranslator->load(QLocale(language), "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
            QCoreApplication::installTranslator(qtTranslator);
    }
    if (language != "ja") {
        if (language != "en") {
            fallbackTranslator = new QTranslator(QCoreApplication::instance());
            if (fallbackTranslator->load(":/i18n/komamimic_en.qm"))
                QCoreApplication::installTranslator(fallbackTranslator);
        }
        applicationTranslator = new QTranslator(QCoreApplication::instance());
        if (applicationTranslator->load(":/i18n/komamimic_" + language + ".qm"))
            QCoreApplication::installTranslator(applicationTranslator);
        else language = fallbackTranslator ? QString("en") : QString("ja");
    }
    return language;
}
QString I18n::savedLanguage(const QSettings &settings) {
    // Avoid Qt INI's reserved General section. Accept early development keys
    // under either spelling without changing the user's other settings.
    return settings.value("ui/language", settings.value("general/language", settings.value("General/language", "system"))).toString();
}
QString I18n::currentLanguage() { return language; }
QString I18n::formatLabel(const QString &stableName) {
    return QCoreApplication::translate("KomaMimic", stableName.toUtf8().constData());
}
QString I18n::startupConfigPath(const QStringList &arguments) {
    QString path;
    for (int i = 1; i < arguments.size(); ++i) {
        const auto &arg = arguments[i];
        if (arg == "--") break;
        if ((arg == "--config" || arg == "-i") && i + 1 < arguments.size()) path = arguments[++i];
        else if (arg.startsWith("--config=")) path = arg.mid(9);
        else if (arg.startsWith("-i=")) path = arg.mid(3);
        else if (arg.startsWith("-i") && !arg.startsWith("--") && arg.size() > 2) path = arg.mid(2);
    }
    return path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/settings.ini"
                          : QFileInfo(path).absoluteFilePath();
}
