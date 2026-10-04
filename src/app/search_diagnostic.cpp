#include "app/search_diagnostic.h"

#include <QRegularExpression>

#include <initializer_list>

namespace forevertas::app {
namespace {

bool ContainsAny(const QString &text,
                 std::initializer_list<const char *> needles,
                 Qt::CaseSensitivity sensitivity = Qt::CaseSensitive) {
    for (const char *needle : needles) {
        if (text.contains(QLatin1String(needle), sensitivity)) return true;
    }
    return false;
}

bool StartsWithAny(const QString &text,
                   std::initializer_list<const char *> prefixes,
                   Qt::CaseSensitivity sensitivity = Qt::CaseSensitive) {
    for (const char *prefix : prefixes) {
        if (text.startsWith(QLatin1String(prefix), sensitivity)) return true;
    }
    return false;
}

// Vulkan result codes, matched whole: -1 must not match -13 or -1000069000.
bool HasVkResult(const QString &text, int code) {
    const QRegularExpression pattern(
            QStringLiteral("VkResult %1(?![0-9])").arg(code));
    return pattern.match(text).hasMatch();
}

struct Explanation {
    QString category;
    QString reason;
    QString guidance;
};

// Rules match phrases from ForeverTAS and ForeverValidator diagnostics, not
// incidental words in paths or user scripts. Messages are wrapped in step
// prefixes ("<step> failed: <cause>"), so most rules look for the cause
// anywhere in the text. The first matching rule wins, so specific causes come
// before the generic step prefixes.
Explanation Explain(const QString &details) {
    if (ContainsAny(details, {"no iteration satisfied the selected evaluation target",
                              "no attempt satisfied the custom target"})) {
        return {QStringLiteral("no-result"),
                QStringLiteral("No attempt reached the target."),
                QStringLiteral("None of the tried inputs met the target and its conditions. Use \"Evaluate base\" to see what the base inputs reach. Then loosen the conditions or widen the target's time window.")};
    }
    if (ContainsAny(details, {"Could not save ", "Could not create session directory",
                              "Restart result already exists"})) {
        return {QStringLiteral("storage"),
                QStringLiteral("The result could not be saved."),
                QStringLiteral("Check that the disk has free space and that ForeverTAS is allowed to write to the folder in the details. Copy your best inputs before you close the app.")};
    }
    if (details.contains(QLatin1String("parity error"))) {
        return {QStringLiteral("gpu-parity"),
                QStringLiteral("The GPU and the CPU got different results for the same inputs."),
                QStringLiteral("This is a bug in the GPU physics, so the result was not used. Choose Optimized CPU as the physics backend for this search, and please report the problem with the details.")};
    }
    if (ContainsAny(details, {"kernel watchdog limit", "display-device budget",
                              "cudaErrorLaunchTimeout", "hipErrorLaunchTimeOut",
                              "launch timed out"})) {
        return {QStringLiteral("gpu-time-limit"),
                QStringLiteral("The graphics card took too long on one step."),
                QStringLiteral("The graphics card also draws your screen, so the system stops GPU work that runs too long. Turn on \"Calibrate for maximum throughput\", or lower \"Parallel samples at a time\". If it still happens, choose Optimized CPU.")};
    }
    const bool gpuText = ContainsAny(details, {"CUDA", "HIP", "Vulkan", "GPU"});
    if (ContainsAny(details, {"memory headroom", "CUDA free memory", "GPU memory limits",
                              "cudaErrorMemoryAllocation", "hipErrorOutOfMemory"}) ||
        HasVkResult(details, -2) ||
        (gpuText && details.contains(QLatin1String("allocation failed")) &&
         !details.contains(QLatin1String("host allocation failed")))) {
        return {QStringLiteral("gpu-memory"),
                QStringLiteral("The graphics card does not have enough free memory."),
                QStringLiteral("Close other programs that use the graphics card, such as games, browsers or video players. You can also lower \"Parallel samples at a time\", or choose Optimized CPU.")};
    }
    if (ContainsAny(details, {"bad_alloc", "allocation failed", "could not allocate"},
                    Qt::CaseInsensitive) ||
        HasVkResult(details, -1)) {
        return {QStringLiteral("memory"),
                QStringLiteral("The computer ran out of memory."),
                QStringLiteral("Close other programs and try again. Lowering \"Parallel samples at a time\" or \"Worker threads\" also uses less memory.")};
    }
    if (details.contains(QLatin1String("unsupported_physics_transition"))) {
        return {QStringLiteral("gpu-physics"),
                QStringLiteral("The run reached physics that the GPU does not support yet."),
                QStringLiteral("This can happen with water, dirt slides, burnouts, donuts or unusual collisions. Choose Optimized CPU as the physics backend for this search.")};
    }
    if (details.contains(QLatin1String("capacity_exceeded"))) {
        return {QStringLiteral("gpu-capacity"),
                QStringLiteral("An attempt needed more room than the GPU search has."),
                QStringLiteral("Lower the input density or the insertion counts, or choose Optimized CPU as the physics backend.")};
    }
    if (ContainsAny(details, {"fast CUDA", "fast mode", "specialized CUDA",
                              "session specialization"},
                    Qt::CaseInsensitive)) {
        return {QStringLiteral("gpu-fast-kernel"),
                QStringLiteral("The faster CUDA kernel could not be used."),
                QStringLiteral("Turn off \"Use the faster CUDA kernel\" in the CUDA settings. Regular CUDA still works.")};
    }
    if (ContainsAny(details, {"simulation scope is unsupported",
                              "supports only certified map and vehicle"})) {
        return {QStringLiteral("gpu-map"),
                QStringLiteral("GPU physics cannot simulate this map."),
                QStringLiteral("GPU physics supports Stadium maps, and Desert maps with the Desert car. Choose Optimized CPU as the physics backend for other maps.")};
    }
    if (ContainsAny(details, {"search prerequisites are not ready", "support is not compiled",
                              "unavailable in a CPU-only build", "selected GPU backend is not compiled",
                              "-capable devices", "no Vulkan physical device",
                              "no Vulkan device supports", "compute capability",
                              "lacks a required Vulkan", "has no compute queue",
                              "VK_KHR_shader_float_controls2", "VK_EXT_memory_budget",
                              "Vulkan 1.2 loader", "sandbox simulation could not start"})) {
        return {QStringLiteral("gpu-unavailable"),
                QStringLiteral("The graphics card backend could not start."),
                QStringLiteral("Update the graphics driver and restart ForeverTAS, or choose Optimized CPU as the physics backend.")};
    }
    if (ContainsAny(details, {"cudaError", "CUDA error", "hipError", "HIP error", "VkResult",
                              "device_failure", "search batch status: unknown"})) {
        return {QStringLiteral("gpu-error"),
                QStringLiteral("The graphics card reported an error."),
                QStringLiteral("Restart ForeverTAS and try again. If it happens again, update the graphics driver or choose Optimized CPU, and report the problem with the details.")};
    }
    if (StartsWithAny(details, {"unsupported "}, Qt::CaseInsensitive) ||
        ContainsAny(details, {"backend is not available", "backend is not supported",
                              "does not support evaluator", "does not support modifier",
                              "does not support search algorithm", "not available on Vulkan",
                              "GPU custom targets support at most", "scripted GPU target",
                              "condition program must contain",
                              "smooth-steering weight table is too large"})) {
        return {QStringLiteral("capability"),
                QStringLiteral("The selected physics backend cannot run this setup."),
                QStringLiteral("Choose Optimized CPU as the physics backend, or change the target or modifier named in the details.")};
    }
    if (ContainsAny(details, {"-event limit", "-command limit", "128 MiB limit"})) {
        return {QStringLiteral("limits"),
                QStringLiteral("The search has more inputs than it can handle."),
                QStringLiteral("Use fewer inputs: lower the input density or the insertion counts, or use a smaller deformation radius or count.")};
    }
    static const QRegularExpression scriptLine(
            QStringLiteral("(?:^|: )(Line [0-9]+: [^\\n]*)"));
    if (const auto line = scriptLine.match(details); line.hasMatch()) {
        QString message = line.captured(1).trimmed();
        if (!message.endsWith(QLatin1Char('.'))) message += QLatin1Char('.');
        return {QStringLiteral("script"),
                QStringLiteral("The base input script has a mistake."),
                QStringLiteral("%1 Fix that line, then try again.").arg(message)};
    }
    if (ContainsAny(details, {"Simulation horizon must be", "does not fit the Simulation horizon",
                              "exceeds the Simulation horizon", "not aligned to the search tick",
                              "timeline is out of range", "search times are invalid",
                              "modifier window does not intersect", "invalid observation plan",
                              "must begin on or after the first whole tick",
                              "cannot be partitioned at the mutable boundary"})) {
        return {QStringLiteral("time-window"),
                QStringLiteral("The search times do not fit together."),
                QStringLiteral("Check that the modifier and target times fall inside the run, and that \"Search end\" comes after them. Then try again.")};
    }
    if (ContainsAny(details, {"sandbox state is incompatible:",
                              "sandbox state cursor is incompatible",
                              "sandbox state has no runtime clone"})) {
        return {QStringLiteral("state-compatibility"),
                QStringLiteral("Saved simulation data does not match the current settings."),
                QStringLiteral("Start a new search. If this keeps happening, report the problem with the details.")};
    }
    if (ContainsAny(details, {"pack directory", "packs directory",
                              "sandbox assets could not be prepared", "installed-pack asset"},
                    Qt::CaseInsensitive)) {
        return {QStringLiteral("assets"),
                QStringLiteral("The game files could not be read."),
                QStringLiteral("Check that \"Packs directory\" points to the Packs folder of TrackMania United Forever, which holds files such as Stadium.pak, and that the game is fully installed.")};
    }
    if (details.contains(QLatin1String("scenario route is unsupported"))) {
        return {QStringLiteral("unsupported-map"),
                QStringLiteral("ForeverTAS does not support this map's environment, car or game mode."),
                QStringLiteral("Choose a different replay or map.")};
    }
    if (ContainsAny(details, {"sandbox map could not be loaded",
                              "vehicle definition could not be built",
                              "visual scene could not be built", "map collision scene"})) {
        return {QStringLiteral("map"),
                QStringLiteral("The map could not be loaded."),
                QStringLiteral("Check that the map opens in the game and that \"Packs directory\" belongs to the same game. If a graphics card backend is selected, try Optimized CPU.")};
    }
    if (!details.contains(QLatin1String("deterministic execution mode is unavailable")) &&
        (StartsWithAny(details, {"decoding ", "reading replay", "reading cached replay",
                                 "reading shared CPU replay", "reading scenario",
                                 "loading replay ", "loading scenario ",
                                 "loading shared CPU scenario"}) ||
         ContainsAny(details, {"replay file", "replay path is empty",
                               "scenario could not be decoded", "replay has no playable input",
                               "replay input time is out of range"}))) {
        return {QStringLiteral("input"),
                QStringLiteral("The replay or map file could not be read."),
                QStringLiteral("Check that the file still exists and plays in the game. If it does, keep the file and report the problem with the details.")};
    }
    if (StartsWithAny(details, {"invalid ", "configuration "}, Qt::CaseInsensitive) ||
        ContainsAny(details, {"unknown search algorithm", "unknown evaluation target",
                              "unknown modifier", "modifier pipeline must contain",
                              "worker count must be between"})) {
        return {QStringLiteral("configuration"),
                QStringLiteral("A search setting is not valid."),
                QStringLiteral("Correct the setting named in the details, then try again. Your other settings are kept.")};
    }
    return {QStringLiteral("internal"),
            QStringLiteral("ForeverTAS ran into an unexpected problem."),
            QStringLiteral("Your inputs and settings are kept. Try again. If it happens again, copy the details and report the problem.")};
}

}  // namespace

QVariantMap SearchDiagnostic(const QString &stage, const QString &details) {
    const Explanation explanation = Explain(details);
    QVariantMap diagnostic{{"stage", stage},
                           {"category", explanation.category},
                           {"reason", explanation.reason},
                           {"guidance", explanation.guidance},
                           {"details", details}};
    diagnostic.insert(QStringLiteral("text"), FormatSearchDiagnostic(diagnostic));
    return diagnostic;
}

QString FormatSearchDiagnostic(const QVariantMap &diagnostic) {
    return QStringLiteral("%1\n%2\n\nTechnical details\nStage: %3\nCategory: %4\n%5")
            .arg(diagnostic.value("reason").toString(),
                 diagnostic.value("guidance").toString(),
                 diagnostic.value("stage").toString(),
                 diagnostic.value("category").toString(),
                 diagnostic.value("details").toString());
}

}  // namespace forevertas::app
