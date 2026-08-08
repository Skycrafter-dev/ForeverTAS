#include "app/script_file_store.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace forevertas::app {
namespace {

constexpr qint64 kMaximumScriptBytes = 1024 * 1024;

QVariantMap Failure(const QString &message, bool exists = false) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("exists"), exists},
            {QStringLiteral("message"), message}};
}

bool IsSupportedKind(const QString &kind) {
    return kind == QStringLiteral("conditions") ||
           kind == QStringLiteral("inputs");
}

QString DefaultRootDirectory() {
    return QDir(QStandardPaths::writableLocation(
                        QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("script-library"));
}

} // namespace

ScriptFileStore::ScriptFileStore(QObject *parent)
    : ScriptFileStore(DefaultRootDirectory(), parent) {}

ScriptFileStore::ScriptFileStore(const QString &rootDirectory, QObject *parent)
    : QObject(parent), rootDirectory_(QDir::cleanPath(rootDirectory)) {}

QString ScriptFileStore::rootDirectory() const { return rootDirectory_; }

QString ScriptFileStore::directoryForKind(const QString &kind) const {
    if (!IsSupportedKind(kind))
        return {};
    return QDir(rootDirectory_).filePath(kind);
}

QVariantMap ScriptFileStore::resolvedFile(const QString &kind,
                                          const QString &name) const {
    const QString directory = directoryForKind(kind);
    if (directory.isEmpty()) {
        return Failure(tr("Unknown script library."));
    }

    QString fileName = name.trimmed();
    if (fileName.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive)) {
        fileName.chop(4);
        fileName = fileName.trimmed();
    }
    static const QRegularExpression invalidCharacters(
            QStringLiteral("[\\x00-\\x1f<>:\"/\\\\|?*]"));
    static const QRegularExpression reservedWindowsName(
            QStringLiteral("^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$"),
            QRegularExpression::CaseInsensitiveOption);
    if (fileName.isEmpty()) {
        return Failure(tr("Enter a file name."));
    }
    if (fileName == QStringLiteral(".") || fileName == QStringLiteral("..") ||
        fileName.size() > 80 || fileName.endsWith(QLatin1Char('.')) ||
        fileName.endsWith(QLatin1Char(' ')) ||
        invalidCharacters.match(fileName).hasMatch() ||
        reservedWindowsName.match(fileName).hasMatch()) {
        return Failure(
                tr("Use a short file name without / \\ : * ? \" < > |."));
    }

    fileName += QStringLiteral(".txt");
    const QString path = QDir(directory).filePath(fileName);
    if (QFileInfo(path).absolutePath() !=
        QFileInfo(directory).absoluteFilePath()) {
        return Failure(tr("That file name is not allowed."));
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("name"), fileName},
            {QStringLiteral("path"), path},
            {QStringLiteral("directory"), directory}};
}

QVariantList ScriptFileStore::files(const QString &kind) const {
    const QString directory = directoryForKind(kind);
    if (directory.isEmpty())
        return {};

    const QFileInfoList entries = QDir(directory).entryInfoList(
            {QStringLiteral("*.txt")}, QDir::Files | QDir::Readable,
            QDir::Name | QDir::IgnoreCase);
    QVariantList result;
    result.reserve(entries.size());
    for (const QFileInfo &entry : entries) {
        result.push_back(
                QVariantMap{{QStringLiteral("name"), entry.fileName()},
                            {QStringLiteral("size"), entry.size()},
                            {QStringLiteral("modified"),
                             entry.lastModified().toString(Qt::ISODate)}});
    }
    return result;
}

QVariantMap ScriptFileStore::save(const QString &kind, const QString &name,
                                  const QString &content,
                                  bool overwrite) const {
    const QVariantMap resolved = resolvedFile(kind, name);
    if (!resolved.value(QStringLiteral("ok")).toBool())
        return resolved;

    const QByteArray bytes = content.toUtf8();
    if (bytes.size() > kMaximumScriptBytes) {
        return Failure(tr("The script is larger than 1 MB."));
    }
    const QString path = resolved.value(QStringLiteral("path")).toString();
    if (QFileInfo::exists(path) && !overwrite) {
        return Failure(
                tr("%1 already exists. Choose Replace to overwrite it.")
                        .arg(resolved.value(QStringLiteral("name")).toString()),
                true);
    }
    if (!QDir().mkpath(
                resolved.value(QStringLiteral("directory")).toString())) {
        return Failure(tr("Could not create the script library folder."));
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        !file.commit()) {
        return Failure(tr("Could not save %1.")
                               .arg(resolved.value(QStringLiteral("name"))
                                            .toString()));
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("exists"), false},
            {QStringLiteral("name"), resolved.value(QStringLiteral("name"))},
            {QStringLiteral("message"), tr("Saved.")}};
}

QVariantMap ScriptFileStore::load(const QString &kind,
                                  const QString &name) const {
    const QVariantMap resolved = resolvedFile(kind, name);
    if (!resolved.value(QStringLiteral("ok")).toBool())
        return resolved;

    QFile file(resolved.value(QStringLiteral("path")).toString());
    if (!file.open(QIODevice::ReadOnly)) {
        return Failure(tr("Could not open %1.")
                               .arg(resolved.value(QStringLiteral("name"))
                                            .toString()));
    }
    if (file.size() > kMaximumScriptBytes) {
        return Failure(tr("The script is larger than 1 MB."));
    }
    const QByteArray bytes = file.readAll();
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("name"), resolved.value(QStringLiteral("name"))},
            {QStringLiteral("content"), QString::fromUtf8(bytes)},
            {QStringLiteral("message"), tr("Loaded.")}};
}

} // namespace forevertas::app
