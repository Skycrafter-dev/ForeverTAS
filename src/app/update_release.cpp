#include "app/update_release.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>

namespace forevertas::app {
namespace {

bool ValidPackageId(const QString &id) {
    static const QRegularExpression nvidia(
            QStringLiteral("^nvidia-sm(50|52|53|60|61|62|70|72|75|80|86|87|89|90|100|101|120)$"));
    return id == QStringLiteral("universal") ||
           id == QStringLiteral("amd-rx7000-rx9000") ||
           nvidia.match(id).hasMatch();
}

} // namespace

std::optional<UpdateCatalog> ParseUpdateCatalog(
        const QByteArray &json,
        const QString &installedVersion,
        const QString &platform,
        const QString &architecture) {
    static const QRegularExpression versionPattern(
            QStringLiteral("^v?([0-9]+\\.[0-9]+\\.[0-9]+)$"));
    static const QRegularExpression digestPattern(
            QStringLiteral("^([0-9a-fA-F]{64})$"));
    if (platform != QStringLiteral("linux") &&
        platform != QStringLiteral("windows")) {
        return std::nullopt;
    }
    if (architecture != QStringLiteral("x86_64") &&
        architecture != QStringLiteral("arm64")) {
        return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject())
        return std::nullopt;
    const QJsonObject release = document.object();
    if (release.value(QStringLiteral("schema")).toInt() != 1) {
        return std::nullopt;
    }
    const auto match = versionPattern.match(
            release.value(QStringLiteral("version")).toString());
    if (!match.hasMatch())
        return std::nullopt;
    UpdateCatalog catalog;
    catalog.version = match.captured(1);
    const QVersionNumber latest = QVersionNumber::fromString(catalog.version);
    const QVersionNumber installed = QVersionNumber::fromString(installedVersion);
    if (installed.isNull() || QVersionNumber::compare(latest, installed) < 0)
        return std::nullopt;

    catalog.releaseUrl = QUrl(
            release.value(QStringLiteral("release_url")).toString());
    if (catalog.releaseUrl.scheme() != QStringLiteral("https") ||
        catalog.releaseUrl.host() != QStringLiteral("github.com") ||
        catalog.releaseUrl.path() != QStringLiteral(
                "/Skycrafter-dev/ForeverTAS/releases/tag/v%1")
                .arg(catalog.version)) {
        return std::nullopt;
    }
    const QString prefix = QStringLiteral("ForeverTAS-%1-%2-")
                                   .arg(catalog.version, platform);
    const QString extension = platform == QStringLiteral("windows")
                                      ? QStringLiteral("-Setup.exe")
                                      : QStringLiteral(".AppImage");
    const QString suffix = QStringLiteral("-%1%2").arg(architecture, extension);
    const QString downloadPrefix = QStringLiteral(
            "/Skycrafter-dev/ForeverTAS/releases/download/v%1/")
                                           .arg(catalog.version);
    for (const QJsonValue &value : release.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        if (!name.startsWith(prefix) || !name.endsWith(suffix))
            continue;
        const QString id = name.mid(prefix.size(),
                                    name.size() - prefix.size() - suffix.size());
        if (!ValidPackageId(id))
            continue;
        const QUrl downloadUrl(QStringLiteral(
                "https://github.com%1%2").arg(downloadPrefix, name));
        const auto digest = digestPattern.match(
                asset.value(QStringLiteral("sha256")).toString());
        if (!digest.hasMatch()) {
            continue;
        }
        bool duplicate = false;
        for (const auto &package : catalog.packages)
            duplicate |= package.packageId == id;
        if (duplicate)
            continue;
        catalog.packages.push_back(UpdateRelease{
                catalog.version, id, name, downloadUrl, catalog.releaseUrl,
                QByteArray::fromHex(digest.captured(1).toLatin1())});
    }
    if (catalog.packages.isEmpty())
        return std::nullopt;
    return catalog;
}

QString RecommendedPackageId(int nvidiaComputeCapability,
                             bool amdRadeonRx7000Or9000,
                             const QStringList &availablePackageIds) {
    const QString nvidia = QStringLiteral("nvidia-sm%1")
                                    .arg(nvidiaComputeCapability);
    if (nvidiaComputeCapability >= 50 &&
        availablePackageIds.contains(nvidia)) {
        return nvidia;
    }
    if (amdRadeonRx7000Or9000 &&
        availablePackageIds.contains(QStringLiteral("amd-rx7000-rx9000"))) {
        return QStringLiteral("amd-rx7000-rx9000");
    }
    if (availablePackageIds.contains(QStringLiteral("universal")))
        return QStringLiteral("universal");
    return {};
}

} // namespace forevertas::app
