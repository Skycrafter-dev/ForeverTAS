#pragma once

#include "compute/compute_module_api.h"
#include "physics_backend.h"
#include "searches/search_runner.h"

#include <QString>

#include <functional>
#include <memory>
#include <vector>

namespace forevertas::app {

// The GPU backends this app can run: compiled in (a developer build with CUDA
// or HIP), or a GPU module installed for this computer (releases: the app has
// no GPU code; it downloads the module for its GPU, see UpdateController).
class ComputeModules final {
public:
    static ComputeModules &Instance();

    // Loads the installed modules again (after one was installed).
    void Reload();
    bool Has(PhysicsBackend backend) const;
    compute::GpuStatus Status(PhysicsBackend backend) const;
    // A search on any backend: GPU ones in the module, the rest here. Throws
    // what RunSearch throws.
    SearchResult RunSearch(const SearchRequest &request, const SearchRunControl *control) const;
    // The same search on several GPUs at once (two or more devices; with one,
    // the request's backend alone).
    SearchResult RunSearch(const SearchRequest &request, const std::vector<SearchDevice> &devices,
                           const SearchRunControl *control) const;

    // Runs a remote device (SearchDevice::remote) of a search on several
    // devices: the app's remote hub (docs/REMOTE.md).
    using RemoteRunner = std::function<SearchResult(const SearchRequest &, const SearchRunControl *)>;
    void SetRemoteRunner(RemoteRunner runner) { remoteRunner_ = std::move(runner); }

    // Where modules are installed: one folder per module, with module.json.
    static QString InstallDirectory();
    // The installed module's id for a backend ("cuda-sm86"), empty if none.
    QString InstalledId(PhysicsBackend backend) const;

private:
    ComputeModules();
    struct Loaded;
    std::unique_ptr<Loaded> cuda_;
    std::unique_ptr<Loaded> hip_;
    RemoteRunner remoteRunner_;
    const Loaded *ModuleFor(PhysicsBackend backend) const;
};

}  // namespace forevertas::app
