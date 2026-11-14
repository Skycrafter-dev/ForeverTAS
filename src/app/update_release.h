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
    QString assetName;
    QUrl downloadUrl;
    QUrl releaseUrl;
    QByteArray sha256;
};

// A GPU module of a release (src/compute): the search core built with CUDA
// for one GPU generation, or with HIP for AMD's.
struct ComputeModuleRelease {
    QString id;        // "cuda-sm86", "hip"
    QString backend;   // "cuda" or "hip"
    int sm = 0;        // CUDA: the generation it carries native code for
    bool ptx = false;  // CUDA: plus code later GPUs can compile at load
    QString abi;
    QUrl downloadUrl;
    QByteArray sha256;
};

struct UpdateCatalog {
    QString version;
    QUrl releaseUrl;
    // the app for this platform
    std::optional<UpdateRelease> app;
    QVector<ComputeModuleRelease> modules;
};

// updates.json: schema 1 (what installed apps up to 0.2.7 read: the app is
// its "universal" asset) plus "compute", the GPU modules per platform.
std::optional<UpdateCatalog> ParseUpdateCatalog(
        const QByteArray &json,
        const QString &platform,
        const QString &architecture);

// The modules for this computer's GPUs: for an NVIDIA card, native code for
// its generation (CUDA runs code for x.y on x.z, z >= y), else the newest with
// PTX; AMD's for an RX 7000/9000. Both with both cards; none when none fits.
QVector<ComputeModuleRelease> ChooseComputeModules(const QVector<ComputeModuleRelease> &modules,
                                                   const QString &abi,
                                                   int nvidiaComputeCapability,
                                                   bool amdRadeonRx7000Or9000);

} // namespace forevertas::app
