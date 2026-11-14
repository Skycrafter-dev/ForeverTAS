#include "app/update_release.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace forevertas::app {
namespace {

QByteArray Digest(const QJsonObject &object) {
    static const QRegularExpression digestPattern(QStringLiteral("^([0-9a-fA-F]{64})$"));
    const auto digest = digestPattern.match(object.value(QStringLiteral("sha256")).toString());
    return digest.hasMatch() ? QByteArray::fromHex(digest.captured(1).toLatin1()) : QByteArray();
}

// Downloads come from this repository's releases only.
bool ReleaseAssetUrl(const QUrl &url) {
    static const QRegularExpression path(
            QStringLiteral("^/Skycrafter-dev/ForeverTAS/releases/download/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$"));
    return url.scheme() == QStringLiteral("https") && url.host() == QStringLiteral("github.com") &&
           path.match(url.path()).hasMatch();
}

} // namespace

std::optional<UpdateCatalog> ParseUpdateCatalog(
        const QByteArray &json,
        const QString &platform,
        const QString &architecture) {
    static const QRegularExpression versionPattern(
            QStringLiteral("^v?([0-9]+\\.[0-9]+\\.[0-9]+)$"));
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
    catalog.releaseUrl = QUrl(
            release.value(QStringLiteral("release_url")).toString());
    if (catalog.releaseUrl.scheme() != QStringLiteral("https") ||
        catalog.releaseUrl.host() != QStringLiteral("github.com") ||
        catalog.releaseUrl.path() != QStringLiteral(
                "/Skycrafter-dev/ForeverTAS/releases/tag/v%1")
                .arg(catalog.version)) {
        return std::nullopt;
    }

    const QString appName = QStringLiteral("ForeverTAS-%1-%2-universal-%3%4")
            .arg(catalog.version, platform, architecture,
                 platform == QStringLiteral("windows") ? QStringLiteral("-Setup.exe")
                                                       : QStringLiteral(".AppImage"));
    for (const QJsonValue &value : release.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject asset = value.toObject();
        if (asset.value(QStringLiteral("name")).toString() != appName) continue;
        const QByteArray sha256 = Digest(asset);
        if (sha256.isEmpty()) break;
        catalog.app = UpdateRelease{
                catalog.version, appName,
                QUrl(QStringLiteral("https://github.com/Skycrafter-dev/ForeverTAS/releases/download/v%1/%2")
                             .arg(catalog.version, appName)),
                catalog.releaseUrl, sha256};
        break;
    }

    const QJsonObject compute = release.value(QStringLiteral("compute")).toObject();
    for (const QJsonValue &value : compute.value(platform).toArray()) {
        const QJsonObject entry = value.toObject();
        ComputeModuleRelease module;
        module.id = entry.value(QStringLiteral("id")).toString();
        module.backend = entry.value(QStringLiteral("backend")).toString();
        module.sm = entry.value(QStringLiteral("sm")).toInt();
        module.ptx = entry.value(QStringLiteral("ptx")).toBool();
        module.abi = entry.value(QStringLiteral("abi")).toString();
        module.downloadUrl = QUrl(entry.value(QStringLiteral("url")).toString());
        module.sha256 = Digest(entry);
        if (module.id.isEmpty() || module.abi.isEmpty() || module.sha256.isEmpty() ||
            (module.backend != QStringLiteral("cuda") && module.backend != QStringLiteral("hip")) ||
            (module.backend == QStringLiteral("cuda") && module.sm < 50) ||
            !ReleaseAssetUrl(module.downloadUrl)) {
            continue;
        }
        catalog.modules.push_back(module);
    }
    if (!catalog.app && catalog.modules.isEmpty())
        return std::nullopt;
    return catalog;
}

QVector<ComputeModuleRelease> ChooseComputeModules(const QVector<ComputeModuleRelease> &modules,
                                                   const QString &abi,
                                                   int nvidiaComputeCapability,
                                                   bool amdRadeonRx7000Or9000) {
    QVector<ComputeModuleRelease> chosen;
    const ComputeModuleRelease *native = nullptr;
    const ComputeModuleRelease *newest = nullptr;
    const int major = nvidiaComputeCapability / 10;
    for (const ComputeModuleRelease &module : modules) {
        if (module.abi != abi || module.backend != QStringLiteral("cuda") ||
            nvidiaComputeCapability < 50) {
            continue;
        }
        if (module.sm / 10 == major && module.sm <= nvidiaComputeCapability &&
            (native == nullptr || module.sm > native->sm)) {
            native = &module;
        }
        if (module.ptx && module.sm <= nvidiaComputeCapability &&
            (newest == nullptr || module.sm > newest->sm)) {
            newest = &module;
        }
    }
    if (native != nullptr) {
        chosen.push_back(*native);
    } else if (newest != nullptr) {
        chosen.push_back(*newest);
    }
    if (amdRadeonRx7000Or9000) {
        for (const ComputeModuleRelease &module : modules) {
            if (module.abi == abi && module.backend == QStringLiteral("hip")) {
                chosen.push_back(module);
                break;
            }
        }
    }
    return chosen;
}

} // namespace forevertas::app
