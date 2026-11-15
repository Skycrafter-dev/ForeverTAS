#ifndef FOREVERTAS_COMPUTE_COMPUTE_MODULE_API_H
#define FOREVERTAS_COMPUTE_COMPUTE_MODULE_API_H

// A GPU module: the search core built with CUDA or HIP, as a library the app
// loads when the computer has the GPU for it. The app itself has no GPU code.
// App and module pass C++ types to each other, so a module is only used by an
// app built from the same headers with the same compiler: kComputeAbi, a hash
// of both, must match.

#include "searches/search_runner.h"

#include <cstdio>
#include <optional>
#include <string>

namespace forevertas::compute {

struct GpuStatus {
    bool ready = false;
    std::string deviceName;
    int computeMajor = 0;
    int computeMinor = 0;
    // why not ready (the driver's words)
    std::string diagnostic;
    // the twin (Approximate physics) runs on this GPU
    bool approximate = false;
};

struct SearchOutcome {
    enum class Kind { Done, NoEligibleEvaluation, Cancelled, Failed };
    Kind kind = Kind::Failed;
    std::optional<SearchResult> result;
    std::string message;
};

struct ModuleApi {
    const char *abi;
    // "cuda" or "hip"
    const char *backend;
    void (*useLog)(std::FILE *stream);
    void (*status)(GpuStatus *out);
    void (*runSearch)(const SearchRequest &request,
                      const SearchRunControl *control,
                      SearchOutcome *out);
};

using ModuleEntry = const ModuleApi *(*)();
inline constexpr char kModuleEntryName[] = "ForeverTasComputeModule";

}  // namespace forevertas::compute

#endif
