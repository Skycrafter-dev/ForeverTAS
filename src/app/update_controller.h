#pragma once

#include "app/update_release.h"
#include "app/update_hardware.h"

#include <QNetworkAccessManager>
#include <QObject>

#include <functional>
#include <memory>
#include <optional>

class QNetworkReply;

namespace forevertas::app {

class VerifiedDownload;

// Updates of the app (one per platform), and its GPU module: the one for this
// computer's graphics card is downloaded and loaded by itself, no restart.
class UpdateController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool updateAvailable READ updateAvailable
                       NOTIFY updateAvailableChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadingChanged)
    Q_PROPERTY(int downloadProgress READ downloadProgress
                       NOTIFY downloadProgressChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion
                       NOTIFY updateAvailableChanged)
    Q_PROPERTY(QUrl releaseUrl READ releaseUrl NOTIFY updateAvailableChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage
                       NOTIFY errorMessageChanged)
    // the GPU module: what is happening ("Installing CUDA for this graphics
    // card... 40%"), empty when nothing is
    Q_PROPERTY(QString computeStatus READ computeStatus NOTIFY computeStatusChanged)

public:
    explicit UpdateController(QObject *parent = nullptr);
    ~UpdateController() override;

    bool supported() const;
    bool updateAvailable() const;
    bool checking() const;
    bool downloading() const;
    int downloadProgress() const;
    QString latestVersion() const;
    QUrl releaseUrl() const;
    QString errorMessage() const;
    QString computeStatus() const;

    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void downloadAndInstall();
    Q_INVOKABLE void cancelDownload();
    // Installs this computer's GPU module if it has none (at start).
    Q_INVOKABLE void installComputeModule();

signals:
    void updateAvailableChanged();
    void checkingChanged();
    void downloadingChanged();
    void downloadProgressChanged();
    void errorMessageChanged();
    void computeStatusChanged();
    // a GPU module was installed and loaded
    void computeModulesChanged();

private:
    void setError(const QString &message);
    void setComputeStatus(const QString &status);
    void fetchCatalog(const QUrl &url, std::function<void(std::optional<UpdateCatalog>)> done);
    void installModule(const ComputeModuleRelease &module);
    void installNextModule();

    QNetworkAccessManager m_network;
    std::unique_ptr<VerifiedDownload> m_appDownload;
    std::unique_ptr<VerifiedDownload> m_moduleDownload;
    // modules still to install after the one downloading (one per graphics card)
    QVector<ComputeModuleRelease> m_pendingModules;
    std::optional<UpdateCatalog> m_catalog;
    UpdateHardware m_hardware;
    QString m_errorMessage;
    QString m_computeStatus;
    bool m_supported = false;
    bool m_checking = false;
    int m_downloadProgress = 0;
};

int ApplyAppImageUpdate(const QString &downloadedAppImage,
                        const QString &installedAppImage,
                        qint64 previousPid);

} // namespace forevertas::app
