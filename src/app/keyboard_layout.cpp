#include "app/keyboard_layout.h"

#include <QFile>
#include <QGuiApplication>
#include <QInputMethod>
#include <QLocale>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QString>

#include <optional>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace forevertas::app {
namespace {

#ifndef Q_OS_WIN
// An XKB layout (and variant) as AZERTY or not: French and Belgian, but not
// their BÉPO and Dvorak variants.
bool AzertyLayout(QString layout, QString variant) {
    layout = layout.trimmed().toLower();
    variant = variant.trimmed().toLower();
    const qsizetype plus = layout.indexOf(QLatin1Char('+'));
    if (plus >= 0) {
        variant = layout.mid(plus + 1);
        layout = layout.left(plus);
    }
    const qsizetype paren = layout.indexOf(QLatin1Char('('));
    if (paren >= 0) {
        variant = layout.mid(paren + 1).remove(QLatin1Char(')'));
        layout = layout.left(paren);
    }
    if (layout != QStringLiteral("fr") && layout != QStringLiteral("be")) {
        return false;
    }
    return !variant.contains(QStringLiteral("bepo")) &&
            !variant.contains(QStringLiteral("dvorak")) &&
            !variant.contains(QStringLiteral("ergol"));
}

QString Run(const QString &program, const QStringList &arguments) {
    if (QStandardPaths::findExecutable(program).isEmpty()) return {};
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForFinished(800)) {
        process.kill();
        process.waitForFinished(200);
        return {};
    }
    return QString::fromUtf8(process.readAllStandardOutput());
}

QString FirstOfList(const QString &list) {
    return list.split(QLatin1Char(','), Qt::SkipEmptyParts).value(0).trimmed();
}

std::optional<bool> FromKde() {
    const QString path = QStandardPaths::locate(
            QStandardPaths::GenericConfigLocation, QStringLiteral("kxkbrc"));
    if (path.isEmpty()) return std::nullopt;
    QSettings settings(path, QSettings::IniFormat);
    // Comma lists read as string lists.
    const auto first = [&settings](const QString &key) {
        const QVariant value = settings.value(key);
        return value.typeId() == QMetaType::QStringList
                ? value.toStringList().value(0).trimmed()
                : FirstOfList(value.toString());
    };
    const QString layout = first(QStringLiteral("Layout/LayoutList"));
    if (layout.isEmpty()) return std::nullopt;
    return AzertyLayout(layout, first(QStringLiteral("Layout/VariantList")));
}

std::optional<bool> FromGnome() {
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    if (!desktop.contains(QStringLiteral("GNOME"), Qt::CaseInsensitive)) return std::nullopt;
    const QString sources = Run(QStringLiteral("gsettings"),
                                {QStringLiteral("get"),
                                 QStringLiteral("org.gnome.desktop.input-sources"),
                                 QStringLiteral("sources")});
    const QRegularExpressionMatch match =
            QRegularExpression(QStringLiteral("\\('xkb',\\s*'([^']+)'\\)")).match(sources);
    if (!match.hasMatch()) return std::nullopt;
    return AzertyLayout(match.captured(1), {});
}

std::optional<bool> FromX11() {
    if (qEnvironmentVariable("XDG_SESSION_TYPE") == QStringLiteral("wayland") ||
        qEnvironmentVariableIsEmpty("DISPLAY")) {
        return std::nullopt;
    }
    const QString query = Run(QStringLiteral("setxkbmap"), {QStringLiteral("-query")});
    const auto field = [&query](const char *name) {
        const QRegularExpressionMatch match =
                QRegularExpression(QStringLiteral("^%1:\\s*(\\S+)").arg(QLatin1String(name)),
                                   QRegularExpression::MultilineOption)
                        .match(query);
        return match.hasMatch() ? match.captured(1) : QString{};
    };
    const QString layout = field("layout");
    if (layout.isEmpty()) return std::nullopt;
    return AzertyLayout(FirstOfList(layout), FirstOfList(field("variant")));
}

std::optional<bool> FromSystemFiles() {
    const auto read = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
    };
    // Debian, Ubuntu: XKBLAYOUT="fr"
    const QString debian = read(QStringLiteral("/etc/default/keyboard"));
    QRegularExpressionMatch match =
            QRegularExpression(QStringLiteral("XKBLAYOUT=\"?([^\"\\n]+)")).match(debian);
    if (match.hasMatch()) {
        const QRegularExpressionMatch variant =
                QRegularExpression(QStringLiteral("XKBVARIANT=\"?([^\"\\n]*)")).match(debian);
        return AzertyLayout(FirstOfList(match.captured(1)),
                            variant.hasMatch() ? FirstOfList(variant.captured(1)) : QString{});
    }
    // systemd-localed: Option "XkbLayout" "fr"
    const QString xorg = read(QStringLiteral("/etc/X11/xorg.conf.d/00-keyboard.conf"));
    match = QRegularExpression(QStringLiteral("\"XkbLayout\"\\s*\"([^\"]+)\"")).match(xorg);
    if (match.hasMatch()) {
        const QRegularExpressionMatch variant =
                QRegularExpression(QStringLiteral("\"XkbVariant\"\\s*\"([^\"]*)\"")).match(xorg);
        return AzertyLayout(FirstOfList(match.captured(1)),
                            variant.hasMatch() ? FirstOfList(variant.captured(1)) : QString{});
    }
    // The console's keymap: KEYMAP=fr-latin9, be-latin1
    match = QRegularExpression(QStringLiteral("KEYMAP=\"?([a-z]+)"))
                    .match(read(QStringLiteral("/etc/vconsole.conf")));
    if (match.hasMatch()) return AzertyLayout(match.captured(1), {});
    return std::nullopt;
}
#endif

}  // namespace

bool KeyboardLettersAreAzerty() {
#ifdef Q_OS_WIN
    // The virtual key of the key in QWERTY's Q place.
    const HKL layout = GetKeyboardLayout(0);
    return MapVirtualKeyExW(0x10, MAPVK_VSC_TO_VK, layout) == 'A';
#else
    for (const auto &source : {FromKde, FromGnome, FromX11, FromSystemFiles}) {
        if (const std::optional<bool> azerty = source()) return *azerty;
    }
    const QString name = QGuiApplication::inputMethod() != nullptr
            ? QGuiApplication::inputMethod()->locale().name()
            : QLocale::system().name();
    return name == QStringLiteral("fr_FR") || name == QStringLiteral("fr_BE") ||
            name == QStringLiteral("fr_LU");
#endif
}

}  // namespace forevertas::app
