#include "app/compute_modules.h"

#include "compute/compute_local.h"
#include "diagnostics/diagnostics_log.h"
#include "forevertas_compute_abi.h"
#include "searches/search_algorithm.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QStandardPaths>

#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace forevertas::app {

struct ComputeModules::Loaded {
    QLibrary library;
    const compute::ModuleApi *api = nullptr;
    QString id;
    compute::GpuStatus status;
};

namespace {

QStringList SearchDirectories() {
    QStringList directories;
    const QString override = qEnvironmentVariable("FOREVERTAS_COMPUTE_DIR");
    if (!override.isEmpty()) directories << override;
    directories << ComputeModules::InstallDirectory()
                << QCoreApplication::applicationDirPath() + QStringLiteral("/compute");
    return directories;
}

}  // namespace

ComputeModules &ComputeModules::Instance() {
    static ComputeModules modules;
    return modules;
}

ComputeModules::ComputeModules() {
    Reload();
}

QString ComputeModules::InstallDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
            QStringLiteral("/compute");
}

void ComputeModules::Reload() {
    for (const QString &root : SearchDirectories()) {
        const QDir directory(root);
        // installed modules: only each backend's active one (UpdateController
        // installs a new one beside the old, then names it here); the others go
        const auto activeFor = [&](const QString &backend) {
            QFile activeFile(directory.filePath(QStringLiteral("active-") + backend));
            return activeFile.open(QIODevice::ReadOnly) ? QString::fromUtf8(activeFile.readAll()).trimmed()
                                                        : QString();
        };
        for (const QString &name : directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString folder = directory.filePath(name);
            QFile file(folder + QStringLiteral("/module.json"));
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QJsonObject manifest = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
            const QString backend = manifest.value(QStringLiteral("backend")).toString();
            const QString id = manifest.value(QStringLiteral("id")).toString();
            const QString active = activeFor(backend);
            if (!active.isEmpty() && name != active) {
                if (root == InstallDirectory()) QDir(folder).removeRecursively();
                continue;
            }
            std::unique_ptr<Loaded> &slot = backend == QStringLiteral("cuda") ? cuda_ : hip_;
            if ((backend != QStringLiteral("cuda") && backend != QStringLiteral("hip")) || slot ||
                compute::LocalHas(backend == QStringLiteral("cuda") ? PhysicsBackend::Cuda : PhysicsBackend::Hip)) {
                continue;
            }
            if (manifest.value(QStringLiteral("abi")).toString() != QLatin1String(kComputeAbi)) {
                diagnostics::Log("GPU module %s in %s is for another version of ForeverTAS (%s, this one %s)",
                                 qUtf8Printable(id), qUtf8Printable(folder),
                                 qUtf8Printable(manifest.value(QStringLiteral("abi")).toString()), kComputeAbi);
                continue;
            }
            auto loaded = std::make_unique<Loaded>();
            loaded->id = id;
            loaded->library.setFileName(folder + QLatin1Char('/') +
                                        manifest.value(QStringLiteral("file")).toString());
#if defined(_WIN32)
            // its runtime DLLs (cudart, the HIP runtime) sit next to it
            SetDllDirectoryW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(folder).utf16()));
#endif
            const bool opened = loaded->library.load();
#if defined(_WIN32)
            SetDllDirectoryW(nullptr);
#endif
            const auto entry = opened ? reinterpret_cast<compute::ModuleEntry>(
                                                loaded->library.resolve(compute::kModuleEntryName))
                                      : nullptr;
            loaded->api = entry != nullptr ? entry() : nullptr;
            if (loaded->api == nullptr || QLatin1String(loaded->api->abi) != QLatin1String(kComputeAbi) ||
                QLatin1String(loaded->api->backend) != backend) {
                diagnostics::Log("GPU module %s could not be loaded: %s", qUtf8Printable(id),
                                 qUtf8Printable(opened ? QStringLiteral("not a matching module")
                                                       : loaded->library.errorString()));
                continue;
            }
            loaded->api->useLog(diagnostics::LogFile());
            loaded->api->status(&loaded->status);
            diagnostics::Log("GPU module %s: %s%s", qUtf8Printable(id),
                             loaded->status.ready ? "ready on " : "not ready: ",
                             loaded->status.ready ? loaded->status.deviceName.c_str()
                                                  : loaded->status.diagnostic.c_str());
            slot = std::move(loaded);
        }
    }
}

const ComputeModules::Loaded *ComputeModules::ModuleFor(PhysicsBackend backend) const {
    if (backend == PhysicsBackend::Cuda) return cuda_.get();
    if (backend == PhysicsBackend::Hip) return hip_.get();
    return nullptr;
}

bool ComputeModules::Has(PhysicsBackend backend) const {
    return compute::LocalHas(backend) || ModuleFor(backend) != nullptr;
}

compute::GpuStatus ComputeModules::Status(PhysicsBackend backend) const {
    if (const Loaded *module = ModuleFor(backend)) return module->status;
    compute::GpuStatus status;
    compute::LocalStatus(backend, &status);
    return status;
}

QString ComputeModules::InstalledId(PhysicsBackend backend) const {
    const Loaded *module = ModuleFor(backend);
    return module != nullptr ? module->id : QString();
}

SearchResult ComputeModules::RunSearch(const SearchRequest &request,
                                       const SearchRunControl *control) const {
    const Loaded *module = compute::LocalHas(request.backend) ? nullptr : ModuleFor(request.backend);
    if (module == nullptr) return forevertas::RunSearch(request, control);
    compute::SearchOutcome outcome;
    module->api->runSearch(request, control, &outcome);
    switch (outcome.kind) {
    case compute::SearchOutcome::Kind::Done:
        return std::move(*outcome.result);
    case compute::SearchOutcome::Kind::NoEligibleEvaluation:
        throw NoEligibleEvaluation();
    case compute::SearchOutcome::Kind::Cancelled:
        throw SearchCancelled();
    case compute::SearchOutcome::Kind::Failed:
        break;
    }
    throw std::runtime_error(outcome.message);
}

SearchResult ComputeModules::RunSearch(const SearchRequest &request,
                                       const std::vector<SearchDevice> &devices,
                                       const SearchRunControl *control) const {
    if (devices.size() < 2u && (devices.empty() || devices.front().remote.empty())) return RunSearch(request, control);
    return RunSearchOnDevices(
            request, devices,
            [this](const SearchRequest &deviceRequest, const SearchRunControl *deviceControl) {
                if (!deviceRequest.remoteDevice.empty()) {
                    if (!remoteRunner_) throw std::runtime_error("remote searching is not available");
                    return remoteRunner_(deviceRequest, deviceControl);
                }
                return RunSearch(deviceRequest, deviceControl);
            },
            control);
}

}  // namespace forevertas::app
