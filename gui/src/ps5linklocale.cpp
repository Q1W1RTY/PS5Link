// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <ps5linklocale.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTranslator>

namespace {
bool russian = false;

class RussianTranslator final : public QTranslator
{
public:
    RussianTranslator(QJsonObject entries, QObject *parent)
        : QTranslator(parent), entries(std::move(entries)) {}

    bool isEmpty() const override { return entries.isEmpty(); }

    QString translate(const char *, const char *source, const char *, int) const override
    {
        return entries.value(QString::fromUtf8(source)).toString();
    }

private:
    const QJsonObject entries;
};
}

void installPs5LinkLanguage(QApplication &app)
{
    QString preference;
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (!localAppData.isEmpty())
    {
        QFile file(QDir(localAppData).filePath(QStringLiteral("PS5Link/ui-language.txt")));
        if (file.open(QIODevice::ReadOnly))
            preference = QString::fromUtf8(file.read(16)).trimmed();
    }
    const auto systemLanguages = QLocale::system().uiLanguages();
    const bool systemRussian = !systemLanguages.isEmpty() &&
        systemLanguages.first().section(QLatin1Char('-'), 0, 0) == QStringLiteral("ru");
    russian = preference == QStringLiteral("ru") ||
        (preference != QStringLiteral("en") && systemRussian);
    if (!russian)
        return;

    QFile catalog(QStringLiteral(":/ps5link/ru.json"));
    if (catalog.open(QIODevice::ReadOnly))
        app.installTranslator(new RussianTranslator(QJsonDocument::fromJson(catalog.readAll()).object(), &app));
}

bool ps5LinkUsesRussian()
{
    return russian;
}
