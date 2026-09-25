#include "app/update_controller.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>

#include <cstdio>

#if defined(Q_OS_LINUX)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace forevertas::app {
namespace {

const QUrl releaseFeed(QStringLiteral(
        "https://github.com/Skycrafter-dev/ForeverTAS/"
        "releases/latest/download/updates.json"));
constexpr char kPreferredPackageKey[] = "updates/preferredPackageId";

bool successfulResponse(QNetworkReply *reply) {
    const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return reply->error() == QNetworkReply::NoError &&
           status >= 200 && status < 300;
}

} // namespace

UpdateController::UpdateController(QObject *parent)
    : QObject(parent), m_hardware(DetectUpdateHardware()) {
    m_automaticSelection = !QSettings().contains(
            QLatin1String(kPreferredPackageKey));
#if defined(Q_OS_WIN)
    m_supported = QFileInfo::exists(QCoreApplication::applicationDirPath() +
                                   QStringLiteral("/.forevertas-installed"));
#elif defined(Q_OS_LINUX)
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    const QFileInfo image(appImage);
    m_supported = image.isAbsolute() && image.isFile() &&
                  image.isWritable() && QFileInfo(image.absolutePath()).isWritable();
#endif
}

bool UpdateController::supported() const { return m_supported; }
bool UpdateController::updateAvailable() const {
    return selectedRelease() &&
           (m_catalog->version != QCoreApplication::applicationVersion() ||
            m_selectedPackageId != installedPackageId());
}
bool UpdateController::checking() const { return m_checking; }
bool UpdateController::catalogReady() const { return m_catalog.has_value(); }
QString UpdateController::installedPackageId() const {
    return QStringLiteral(FOREVERTAS_PACKAGE_ID);
}
QString UpdateController::selectedPackageId() const {
    return m_selectedPackageId;
}
QString UpdateController::recommendedPackageId() const {
    return m_recommendedPackageId;
}
bool UpdateController::automaticSelection() const {
    return m_automaticSelection;
}
bool UpdateController::selectedPackageCompatible() const {
    return m_selectedPackageId == QStringLiteral("universal") ||
           (m_selectedPackageId == QStringLiteral("amd-rx7000-rx9000") &&
            m_hardware.amdRadeonRx7000Or9000) ||
           (m_hardware.nvidiaComputeCapability >= 50 &&
            m_selectedPackageId == QStringLiteral("nvidia-sm%1")
                                           .arg(m_hardware.nvidiaComputeCapability));
}
QVariantList UpdateController::packages() const {
    QVariantList result;
    if (!m_catalog)
        return result;
    for (const auto &package : m_catalog->packages) {
        QString label;
        if (package.packageId == QStringLiteral("universal")) {
            label = tr("Universal (CPU + Vulkan)");
        } else if (package.packageId ==
                   QStringLiteral("amd-rx7000-rx9000")) {
            label = tr("AMD Radeon RX 7000 / 9000 (HIP + Vulkan)");
        } else {
            label = tr("NVIDIA SM %1 (CUDA + HIP + Vulkan)")
                            .arg(package.packageId.mid(9));
        }
        result.push_back(QVariantMap{
                {QStringLiteral("id"), package.packageId},
                {QStringLiteral("label"), label},
                {QStringLiteral("recommended"),
                 package.packageId == m_recommendedPackageId},
                {QStringLiteral("installed"),
                 package.packageId == installedPackageId()}});
    }
    return result;
}
bool UpdateController::downloading() const { return m_downloading; }
int UpdateController::downloadProgress() const { return m_downloadProgress; }
QString UpdateController::latestVersion() const {
    return m_catalog ? m_catalog->version : QString{};
}
QUrl UpdateController::releaseUrl() const {
    return m_catalog ? m_catalog->releaseUrl : QUrl{};
}
QString UpdateController::errorMessage() const { return m_errorMessage; }

void UpdateController::setError(const QString &message) {
    if (m_errorMessage == message)
        return;
    m_errorMessage = message;
    emit errorMessageChanged();
}

void UpdateController::checkForUpdates() {
    if (!m_supported || m_checkReply || m_downloading)
        return;
    setError({});
    m_checking = true;
    emit checkingChanged();
    QNetworkRequest request(releaseFeed);
    request.setTransferTimeout(20000);
    request.setRawHeader("User-Agent", "ForeverTAS-updater");
    m_checkReply = m_network.get(request);
    QNetworkReply *const reply = m_checkReply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_checkReply = nullptr;
        m_checking = false;
        emit checkingChanged();
        if (successfulResponse(reply)) {
            const QByteArray data = reply->read(2 * 1024 * 1024 + 1);
            if (data.size() <= 2 * 1024 * 1024) {
                const auto catalog = ParseUpdateCatalog(
                        data,
                        QCoreApplication::applicationVersion(),
                        QStringLiteral(FOREVERTAS_UPDATE_PLATFORM),
                        QStringLiteral(FOREVERTAS_UPDATE_ARCH));
                if (catalog) {
                    m_catalog = catalog;
                    QStringList ids;
                    for (const auto &package : catalog->packages)
                        ids.push_back(package.packageId);
                    m_recommendedPackageId = RecommendedPackageId(
                            m_hardware.nvidiaComputeCapability,
                            m_hardware.amdRadeonRx7000Or9000, ids);
                    const QString preferred = QSettings().value(
                            QLatin1String(kPreferredPackageKey)).toString();
                    m_selectedPackageId = !m_automaticSelection &&
                                                  ids.contains(preferred)
                            ? preferred : m_recommendedPackageId;
                    if (m_selectedPackageId.isEmpty() &&
                        ids.contains(installedPackageId())) {
                        m_selectedPackageId = installedPackageId();
                    }
                    emit catalogChanged();
                    emit selectedPackageChanged();
                    emit updateAvailableChanged();
                } else {
                    setError(tr("No compatible package was found in the latest release."));
                }
            } else {
                setError(tr("The release catalog was too large."));
            }
        } else {
            setError(tr("Could not check for updates: %1")
                             .arg(reply->errorString()));
        }
        reply->deleteLater();
    });
}

const UpdateRelease *UpdateController::selectedRelease() const {
    if (!m_catalog)
        return nullptr;
    for (const auto &package : m_catalog->packages) {
        if (package.packageId == m_selectedPackageId)
            return &package;
    }
    return nullptr;
}

void UpdateController::selectPackage(const QString &packageId) {
    if (m_downloading || !m_catalog)
        return;
    for (const auto &package : m_catalog->packages) {
        if (package.packageId == packageId) {
            m_selectedPackageId = packageId;
            m_automaticSelection = false;
            QSettings().setValue(QLatin1String(kPreferredPackageKey),
                                 packageId);
            emit selectedPackageChanged();
            emit updateAvailableChanged();
            return;
        }
    }
}

void UpdateController::useAutomaticPackage() {
    if (m_downloading)
        return;
    QSettings().remove(QLatin1String(kPreferredPackageKey));
    m_automaticSelection = true;
    m_selectedPackageId = m_recommendedPackageId;
    emit selectedPackageChanged();
    emit updateAvailableChanged();
}

QString UpdateController::downloadPath() const {
    const auto *const release = selectedRelease();
    if (!release)
        return {};
#if defined(Q_OS_LINUX)
    return qEnvironmentVariable("APPIMAGE") + QStringLiteral(".download");
#else
    const QString directory = QStandardPaths::writableLocation(
            QStandardPaths::TempLocation) + QStringLiteral("/ForeverTAS-updates");
    return QDir(directory).filePath(release->assetName);
#endif
}

void UpdateController::downloadAndInstall() {
    if (!m_supported || !updateAvailable() || m_downloading)
        return;
    setError({});
    m_downloadTarget = *selectedRelease();
    const QString destination = downloadPath();
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        setError(tr("Cannot create the update download directory."));
        return;
    }
    m_file = std::make_unique<QSaveFile>(destination);
    if (!m_file->open(QIODevice::WriteOnly)) {
        setError(tr("Cannot write the update beside ForeverTAS."));
        m_file.reset();
        return;
    }
    m_hash.reset();
    m_downloadProgress = 0;
    emit downloadProgressChanged();
    m_downloading = true;
    emit downloadingChanged();

    QNetworkRequest request(m_downloadTarget->downloadUrl);
    request.setTransferTimeout(60000);
    request.setRawHeader("User-Agent", "ForeverTAS-updater");
    m_downloadReply = m_network.get(request);
    QNetworkReply *const reply = m_downloadReply;
    connect(reply, &QIODevice::readyRead, this, [this, reply]() {
        if (!m_downloading || !m_file)
            return;
        const QByteArray data = reply->readAll();
        if (m_file->write(data) != data.size()) {
            setError(tr("The update could not be saved."));
            reply->abort();
            return;
        }
        m_hash.addData(data);
    });
    connect(reply, &QNetworkReply::downloadProgress,
            this, [this](qint64 received, qint64 total) {
        if (total <= 0 || !m_downloading)
            return;
        const int percent = static_cast<int>(100 * received / total);
        if (m_downloadProgress != percent) {
            m_downloadProgress = percent;
            emit downloadProgressChanged();
        }
    });
    connect(reply, &QNetworkReply::finished,
            this, [this, reply]() { finishDownload(reply); });
}

void UpdateController::cancelDownload() {
    if (m_downloadReply)
        m_downloadReply->abort();
}

void UpdateController::finishDownload(QNetworkReply *reply) {
    m_downloadReply = nullptr;
    if (!m_downloading) {
        reply->deleteLater();
        return;
    }
    if (successfulResponse(reply)) {
        const QByteArray remainder = reply->readAll();
        if (m_file->write(remainder) == remainder.size())
            m_hash.addData(remainder);
        else
            setError(tr("The update could not be saved."));
    } else if (reply->error() == QNetworkReply::OperationCanceledError) {
        setError(tr("Update cancelled."));
    } else if (m_errorMessage.isEmpty()) {
        setError(tr("The update download failed: %1").arg(reply->errorString()));
    }
    reply->deleteLater();
    if (m_errorMessage.isEmpty() && m_hash.result() != m_downloadTarget->sha256)
        setError(tr("The update checksum did not match."));
    if (!m_errorMessage.isEmpty() || !m_file->commit()) {
        if (m_errorMessage.isEmpty())
            setError(tr("The update could not be saved."));
        m_file->cancelWriting();
        m_file.reset();
        m_downloading = false;
        emit downloadingChanged();
        return;
    }
    m_file.reset();
    m_downloading = false;
    emit downloadingChanged();

    const QString destination = downloadPath();
#if defined(Q_OS_LINUX)
    const QFile::Permissions permissions = QFile::ReadOwner |
            QFile::WriteOwner | QFile::ExeOwner |
            QFile::ReadGroup | QFile::ExeGroup |
            QFile::ReadOther | QFile::ExeOther;
    if (!QFile::setPermissions(destination, permissions)) {
        setError(tr("Cannot make the downloaded AppImage executable."));
        return;
    }
    const bool started = QProcess::startDetached(
            destination,
            {QStringLiteral("--apply-update"),
             qEnvironmentVariable("APPIMAGE"),
             QString::number(QCoreApplication::applicationPid())});
#else
    const bool started = QProcess::startDetached(
            destination,
            {QStringLiteral("/SILENT"), QStringLiteral("/NORESTART"),
             QStringLiteral("/CLOSEAPPLICATIONS")});
#endif
    if (!started) {
        setError(tr("The downloaded update could not be started."));
        return;
    }
    QCoreApplication::quit();
}

int ApplyAppImageUpdate(const QString &downloadedAppImage,
                        const QString &installedAppImage,
                        qint64 previousPid) {
#if defined(Q_OS_LINUX)
    if (previousPid <= 0 || !QFileInfo(downloadedAppImage).isFile() ||
        !QFileInfo(installedAppImage).isFile() ||
        downloadedAppImage == installedAppImage) {
        return 2;
    }
    for (int attempt = 0; attempt < 300; ++attempt) {
        if (kill(static_cast<pid_t>(previousPid), 0) != 0 && errno == ESRCH)
            break;
        if (attempt == 299) {
            std::fprintf(stderr, "ForeverTAS did not exit for the update.\n");
            return 3;
        }
        QThread::msleep(100);
    }
    const QByteArray installedPath = QFile::encodeName(installedAppImage);
    const QByteArray downloadedPath = QFile::encodeName(downloadedAppImage);
    const auto exchange = [&]() {
        return syscall(SYS_renameat2, AT_FDCWD, installedPath.constData(),
                       AT_FDCWD, downloadedPath.constData(),
                       RENAME_EXCHANGE) == 0;
    };
    if (!exchange()) {
        std::fprintf(stderr, "Could not atomically replace the AppImage.\n");
        return 4;
    }
    const QString backup = installedAppImage + QStringLiteral(".previous");
    if ((QFileInfo::exists(backup) && !QFile::remove(backup)) ||
        !QFile::rename(downloadedAppImage, backup)) {
        exchange();
        std::fprintf(stderr, "Could not back up the previous AppImage.\n");
        return 5;
    }
    if (!QProcess::startDetached(installedAppImage, {})) {
        if (QFile::rename(backup, downloadedAppImage))
            exchange();
        std::fprintf(stderr, "Could not restart ForeverTAS.\n");
        return 6;
    }
    return 0;
#else
    Q_UNUSED(downloadedAppImage)
    Q_UNUSED(installedAppImage)
    Q_UNUSED(previousPid)
    return 2;
#endif
}

} // namespace forevertas::app
