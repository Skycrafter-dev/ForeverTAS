#include "app/update_controller.h"
#include "app/compute_archive.h"
#include "app/compute_modules.h"
#include "compute/compute_local.h"
#include "diagnostics/diagnostics_log.h"
#include "forevertas_compute_abi.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QVersionNumber>

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

bool successfulResponse(QNetworkReply *reply) {
    const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return reply->error() == QNetworkReply::NoError &&
           status >= 200 && status < 300;
}

QNetworkRequest Request(const QUrl &url, int timeoutMs) {
    QNetworkRequest request(url);
    request.setTransferTimeout(timeoutMs);
    request.setRawHeader("User-Agent", "ForeverTAS-updater");
    return request;
}

} // namespace

// A file downloaded to a path and checked against its SHA-256 before it is
// kept; done() gets an empty error on success.
class VerifiedDownload {
public:
    VerifiedDownload(QNetworkAccessManager &network, const QUrl &url, const QString &path,
                     QByteArray sha256, std::function<void(int)> progress,
                     std::function<void(const QString &error)> done)
        : m_file(path), m_sha256(std::move(sha256)), m_done(std::move(done)) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath()) ||
            !m_file.open(QIODevice::WriteOnly)) {
            m_failed = QObject::tr("cannot write %1").arg(path);
            return;
        }
        m_reply = network.get(Request(url, 60000));
        QObject::connect(m_reply, &QIODevice::readyRead, m_reply, [this]() { Take(m_reply); });
        QObject::connect(m_reply, &QNetworkReply::downloadProgress, m_reply,
                         [progress](qint64 received, qint64 total) {
                             if (total > 0) progress(static_cast<int>(100 * received / total));
                         });
        QObject::connect(m_reply, &QNetworkReply::finished, m_reply, [this]() { Finish(); });
    }
    ~VerifiedDownload() {
        if (m_reply) {
            m_reply->disconnect();
            m_reply->abort();
            m_reply->deleteLater();
        }
    }
    // a failure before the download could start (reported by the owner)
    QString startError() const { return m_failed; }
    void abort() {
        if (m_reply) m_reply->abort();
    }

private:
    void Take(QNetworkReply *reply) {
        const QByteArray data = reply->readAll();
        if (m_failed.isEmpty() && m_file.write(data) != data.size()) {
            m_failed = QObject::tr("the download could not be saved");
            reply->abort();
            return;
        }
        m_hash.addData(data);
    }
    void Finish() {
        QNetworkReply *const reply = std::exchange(m_reply, nullptr);
        if (successfulResponse(reply)) Take(reply);
        QString error = m_failed;
        if (error.isEmpty() && !successfulResponse(reply)) {
            error = reply->error() == QNetworkReply::OperationCanceledError
                    ? QObject::tr("cancelled") : reply->errorString();
        }
        reply->deleteLater();
        if (error.isEmpty() && m_hash.result() != m_sha256) error = QObject::tr("the checksum did not match");
        if (error.isEmpty() && !m_file.commit()) error = QObject::tr("the download could not be saved");
        if (!error.isEmpty()) m_file.cancelWriting();
        const auto done = m_done;   // may delete this
        done(error);
    }

    QSaveFile m_file;
    QByteArray m_sha256;
    std::function<void(const QString &)> m_done;
    QNetworkReply *m_reply = nullptr;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QString m_failed;
};

UpdateController::UpdateController(QObject *parent)
    : QObject(parent), m_hardware(DetectUpdateHardware()) {
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

UpdateController::~UpdateController() = default;

bool UpdateController::supported() const { return m_supported; }
bool UpdateController::updateAvailable() const {
    return m_catalog && m_catalog->app &&
           QVersionNumber::compare(QVersionNumber::fromString(m_catalog->version),
                                   QVersionNumber::fromString(QCoreApplication::applicationVersion())) > 0;
}
bool UpdateController::checking() const { return m_checking; }
bool UpdateController::downloading() const { return m_appDownload != nullptr; }
int UpdateController::downloadProgress() const { return m_downloadProgress; }
QString UpdateController::latestVersion() const {
    return m_catalog ? m_catalog->version : QString{};
}
QUrl UpdateController::releaseUrl() const {
    return m_catalog ? m_catalog->releaseUrl : QUrl{};
}
QString UpdateController::errorMessage() const { return m_errorMessage; }
QString UpdateController::computeStatus() const { return m_computeStatus; }

void UpdateController::setError(const QString &message) {
    if (m_errorMessage == message)
        return;
    m_errorMessage = message;
    if (!message.isEmpty()) diagnostics::Log("update: %s", qUtf8Printable(message));
    emit errorMessageChanged();
}

void UpdateController::setComputeStatus(const QString &status) {
    if (m_computeStatus == status) return;
    m_computeStatus = status;
    emit computeStatusChanged();
}

void UpdateController::fetchCatalog(const QUrl &url,
                                    std::function<void(std::optional<UpdateCatalog>)> done) {
    QNetworkReply *const reply = m_network.get(Request(url, 20000));
    connect(reply, &QNetworkReply::finished, this, [reply, done]() {
        std::optional<UpdateCatalog> catalog;
        if (successfulResponse(reply)) {
            const QByteArray data = reply->read(2 * 1024 * 1024 + 1);
            if (data.size() <= 2 * 1024 * 1024) {
                catalog = ParseUpdateCatalog(data, QStringLiteral(FOREVERTAS_UPDATE_PLATFORM),
                                             QStringLiteral(FOREVERTAS_UPDATE_ARCH));
            }
        } else {
            diagnostics::Log("update: %s: %s", qUtf8Printable(reply->url().toString()),
                             qUtf8Printable(reply->errorString()));
        }
        reply->deleteLater();
        done(catalog);
    });
}

void UpdateController::checkForUpdates() {
    if (!m_supported || m_checking || m_appDownload)
        return;
    setError({});
    m_checking = true;
    emit checkingChanged();
    fetchCatalog(releaseFeed, [this](std::optional<UpdateCatalog> catalog) {
        m_checking = false;
        emit checkingChanged();
        if (!catalog || !catalog->app) {
            setError(tr("Could not check for updates."));
            return;
        }
        m_catalog = catalog;
        emit updateAvailableChanged();
    });
}

void UpdateController::installComputeModule() {
    // not for developer builds with CUDA/HIP built in, or modules from elsewhere
    if (m_moduleDownload || compute::LocalHas(PhysicsBackend::Cuda) ||
        compute::LocalHas(PhysicsBackend::Hip) ||
        !qEnvironmentVariable("FOREVERTAS_COMPUTE_DIR").isEmpty() ||
        (m_hardware.nvidiaComputeCapability < 50 && !m_hardware.amdRadeonRx7000Or9000)) {
        return;
    }
    // this version's catalog: its modules are made for this exact app
    const QUrl url(QStringLiteral("https://github.com/Skycrafter-dev/ForeverTAS/releases/download/v%1/updates.json")
                           .arg(QCoreApplication::applicationVersion()));
    diagnostics::Log("GPU module: looking for one (NVIDIA compute capability %d, AMD RX 7000/9000 %s)",
                     m_hardware.nvidiaComputeCapability, m_hardware.amdRadeonRx7000Or9000 ? "yes" : "no");
    fetchCatalog(url, [this](std::optional<UpdateCatalog> catalog) {
        const QVector<ComputeModuleRelease> modules = catalog
                ? ChooseComputeModules(catalog->modules, QLatin1String(kComputeAbi),
                                       m_hardware.nvidiaComputeCapability,
                                       m_hardware.amdRadeonRx7000Or9000)
                : QVector<ComputeModuleRelease>{};
        if (modules.isEmpty()) {
            diagnostics::Log("GPU module: none for this computer in %s", catalog ? "the catalog" : "no catalog");
            return;
        }
        // one per graphics card; the right one installed (and loaded) already
        // stays, another one (a new graphics card) is replaced
        m_pendingModules.clear();
        for (const ComputeModuleRelease &module : modules) {
            const PhysicsBackend backend = module.backend == QStringLiteral("cuda") ? PhysicsBackend::Cuda
                                                                                  : PhysicsBackend::Hip;
            if (ComputeModules::Instance().InstalledId(backend) != module.id) m_pendingModules.push_back(module);
        }
        installNextModule();
    });
}

void UpdateController::installNextModule() {
    if (m_moduleDownload || m_pendingModules.isEmpty()) return;
    const ComputeModuleRelease module = m_pendingModules.takeFirst();
    installModule(module);
}

void UpdateController::installModule(const ComputeModuleRelease &module) {
    const QString name = module.backend == QStringLiteral("cuda") ? QStringLiteral("CUDA") : QStringLiteral("HIP");
    const QString root = ComputeModules::InstallDirectory();
    const QString archive = QDir(root).filePath(module.id + QStringLiteral(".tar.zst"));
    diagnostics::Log("GPU module: installing %s from %s", qUtf8Printable(module.id),
                     qUtf8Printable(module.downloadUrl.toString()));
    setComputeStatus(tr("Installing %1 for this graphics card...").arg(name));
    m_moduleDownload = std::make_unique<VerifiedDownload>(
            m_network, module.downloadUrl, archive, module.sha256,
            [this, name](int percent) {
                setComputeStatus(tr("Installing %1 for this graphics card... %2%").arg(name).arg(percent));
            },
            [this, module, name, root, archive](const QString &failure) {
                QString error = failure;
                const QString folder = QDir(root).filePath(module.id);
                const QString staging = folder + QStringLiteral(".new");
                if (error.isEmpty()) {
                    QDir(staging).removeRecursively();
                    if (ExtractComputeArchive(archive, staging, &error)) {
                        QDir(folder).removeRecursively();
                        if (!QDir().rename(staging, folder)) {
                            error = tr("cannot install it in %1").arg(folder);
                        } else {
                            // its backend's module to load from now on (the others go when
                            // they can: Windows keeps a loaded one's files)
                            QSaveFile active(QDir(root).filePath(QStringLiteral("active-") + module.backend));
                            if (!active.open(QIODevice::WriteOnly) || active.write(module.id.toUtf8()) < 0 ||
                                !active.commit()) {
                                error = tr("cannot write %1").arg(active.fileName());
                            }
                        }
                    }
                }
                QFile::remove(archive);
                QMetaObject::invokeMethod(
                        this,
                        [this]() {
                            m_moduleDownload.reset();
                            installNextModule();
                        },
                        Qt::QueuedConnection);
                if (!error.isEmpty()) {
                    diagnostics::Log("GPU module: %s could not be installed: %s", qUtf8Printable(module.id),
                                     qUtf8Printable(error));
                    setComputeStatus(tr("%1 could not be installed: %2").arg(name, error));
                    return;
                }
                ComputeModules &modules = ComputeModules::Instance();
                const PhysicsBackend backend = module.backend == QStringLiteral("cuda") ? PhysicsBackend::Cuda
                                                                                      : PhysicsBackend::Hip;
                const bool another = !modules.InstalledId(backend).isEmpty();
                modules.Reload();
                // a loaded module cannot be swapped: the new one is used next time
                setComputeStatus(another ? tr("%1 for this graphics card is installed. Restart ForeverTAS to use it.")
                                                   .arg(name)
                                         : QString());
                emit computeModulesChanged();
            });
    if (!m_moduleDownload->startError().isEmpty()) {
        setComputeStatus(tr("%1 could not be installed: %2").arg(name, m_moduleDownload->startError()));
        m_moduleDownload.reset();
        installNextModule();
    }
}

void UpdateController::downloadAndInstall() {
    if (!m_supported || !updateAvailable() || m_appDownload)
        return;
    setError({});
    const UpdateRelease release = *m_catalog->app;
#if defined(Q_OS_LINUX)
    const QString destination = qEnvironmentVariable("APPIMAGE") + QStringLiteral(".download");
#else
    const QString destination = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                                     QStringLiteral("/ForeverTAS-updates")).filePath(release.assetName);
#endif
    m_downloadProgress = 0;
    emit downloadProgressChanged();
    m_appDownload = std::make_unique<VerifiedDownload>(
            m_network, release.downloadUrl, destination, release.sha256,
            [this](int percent) {
                if (m_downloadProgress == percent) return;
                m_downloadProgress = percent;
                emit downloadProgressChanged();
            },
            [this, destination](const QString &error) {
                QMetaObject::invokeMethod(this, [this]() {
                    m_appDownload.reset();
                    emit downloadingChanged();
                }, Qt::QueuedConnection);
                if (!error.isEmpty()) {
                    setError(tr("The update download failed: %1").arg(error));
                    return;
                }
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
            });
    if (!m_appDownload->startError().isEmpty()) {
        setError(tr("The update download failed: %1").arg(m_appDownload->startError()));
        m_appDownload.reset();
        return;
    }
    emit downloadingChanged();
}

void UpdateController::cancelDownload() {
    if (m_appDownload) m_appDownload->abort();
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
