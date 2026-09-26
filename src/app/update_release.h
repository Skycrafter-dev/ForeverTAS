#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <optional>

namespace forevertas::app {

struct UpdateRelease {
    QString version;
    QString packageId;
    QString assetName;
    QUrl downloadUrl;
    QUrl releaseUrl;
    QByteArray sha256;
};

struct UpdateCatalog {
    QString version;
    QUrl releaseUrl;
    QVector<UpdateRelease> packages;
};

std::optional<UpdateCatalog> ParseUpdateCatalog(
        const QByteArray &json,
        const QString &installedVersion,
        const QString &platform,
        const QString &architecture);

QString RecommendedPackageId(int nvidiaComputeCapability,
                             bool amdRadeonRx7000Or9000,
                             const QStringList &availablePackageIds);
QString ResolveNvidiaMatrixPackageId(const QString &preferredPackageId,
                                    int nvidiaComputeCapability,
                                    const QString &packageFilename);

} // namespace forevertas::app
