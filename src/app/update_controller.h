#pragma once

#include "app/update_release.h"
#include "app/update_hardware.h"

#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSaveFile>
#include <QVariantList>

#include <memory>
#include <optional>

class QNetworkReply;

namespace forevertas::app {

class UpdateController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool updateAvailable READ updateAvailable
                       NOTIFY updateAvailableChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)
    Q_PROPERTY(bool catalogReady READ catalogReady NOTIFY catalogChanged)
    Q_PROPERTY(QVariantList packages READ packages NOTIFY catalogChanged)
    Q_PROPERTY(QString installedPackageId READ installedPackageId CONSTANT)
    Q_PROPERTY(QString selectedPackageId READ selectedPackageId
                       NOTIFY selectedPackageChanged)
    Q_PROPERTY(QString recommendedPackageId READ recommendedPackageId
                       NOTIFY catalogChanged)
    Q_PROPERTY(bool automaticSelection READ automaticSelection
                       NOTIFY selectedPackageChanged)
    Q_PROPERTY(bool selectedPackageCompatible READ selectedPackageCompatible
                       NOTIFY selectedPackageChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadingChanged)
    Q_PROPERTY(int downloadProgress READ downloadProgress
                       NOTIFY downloadProgressChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion
                       NOTIFY updateAvailableChanged)
    Q_PROPERTY(QUrl releaseUrl READ releaseUrl NOTIFY updateAvailableChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage
                       NOTIFY errorMessageChanged)

public:
    explicit UpdateController(QObject *parent = nullptr);

    bool supported() const;
    bool updateAvailable() const;
    bool checking() const;
    bool catalogReady() const;
    QVariantList packages() const;
    QString installedPackageId() const;
    QString selectedPackageId() const;
    QString recommendedPackageId() const;
    bool automaticSelection() const;
    bool selectedPackageCompatible() const;
    bool downloading() const;
    int downloadProgress() const;
    QString latestVersion() const;
    QUrl releaseUrl() const;
    QString errorMessage() const;

    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void downloadAndInstall();
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE void selectPackage(const QString &packageId);
    Q_INVOKABLE void useAutomaticPackage();

signals:
    void updateAvailableChanged();
    void checkingChanged();
    void catalogChanged();
    void selectedPackageChanged();
    void downloadingChanged();
    void downloadProgressChanged();
    void errorMessageChanged();

private:
    void setError(const QString &message);
    void finishDownload(QNetworkReply *reply);
    QString downloadPath() const;
    const UpdateRelease *selectedRelease() const;

    QNetworkAccessManager m_network;
    QNetworkReply *m_checkReply = nullptr;
    QNetworkReply *m_downloadReply = nullptr;
    std::optional<UpdateCatalog> m_catalog;
    std::optional<UpdateRelease> m_downloadTarget;
    UpdateHardware m_hardware;
    QString m_selectedPackageId;
    QString m_recommendedPackageId;
    bool m_automaticSelection = true;
    std::unique_ptr<QSaveFile> m_file;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QString m_errorMessage;
    bool m_supported = false;
    bool m_checking = false;
    bool m_downloading = false;
    int m_downloadProgress = 0;
};

int ApplyAppImageUpdate(const QString &downloadedAppImage,
                        const QString &installedAppImage,
                        qint64 previousPid);

} // namespace forevertas::app
