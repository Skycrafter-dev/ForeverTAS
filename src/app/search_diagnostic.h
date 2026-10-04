#ifndef FOREVERTAS_APP_SEARCH_DIAGNOSTIC_H
#define FOREVERTAS_APP_SEARCH_DIAGNOSTIC_H

#include <QString>
#include <QVariantMap>

namespace forevertas::app {

inline QVariantMap SearchDiagnostic(const QString &stage, const QString &details) {
    QString category = QStringLiteral("internal");
    QString reason = QStringLiteral("The search could not complete.");
    QString guidance = QStringLiteral("Keep your input script and copy these details when reporting the failure. No settings have been reset.");
    // Match known diagnostics, not incidental words in paths or user scripts.
    if (details.startsWith("Could not save ")) {
        category = QStringLiteral("storage");
        reason = QStringLiteral("The search result could not be saved.");
        guidance = QStringLiteral("Check free disk space and write permissions for the history location in the details. Copy the current best inputs before closing the app.");
    } else if (details.startsWith("opening pack directory") ||
        details.startsWith("opening cached pack directory") ||
        details.startsWith("opening shared CPU pack directory")) {
        category = QStringLiteral("assets");
        reason = QStringLiteral("The installed game assets could not be opened.");
        guidance = QStringLiteral("Check the selected Packs directory and read permissions, then retry.");
    } else if (details.contains("sandbox state is incompatible:") ||
               details.contains("sandbox state cursor is incompatible") ||
               details.contains("sandbox state has no runtime clone")) {
        category = QStringLiteral("state-compatibility");
        reason = QStringLiteral("A saved simulation state does not match this simulation.");
        guidance = QStringLiteral("Start a fresh search for the selected map and backend. If it recurs, report the mismatched field in the technical details.");
    } else if (details.startsWith("decoding ") ||
               details.startsWith("reading replay ") ||
               details.startsWith("reading cached replay ") ||
               details.startsWith("loading replay ") ||
               details.startsWith("loading scenario ")) {
        category = QStringLiteral("input");
        reason = QStringLiteral("The scenario or replay could not be read.");
        guidance = QStringLiteral("Check that the selected file is a readable, supported TrackMania replay or map. Keep the original file for a bug report if it opens in the game.");
    } else if (details.startsWith("unsupported ", Qt::CaseInsensitive) ||
               details.contains("backend is not available") ||
               details.contains("backend is not supported")) {
        category = QStringLiteral("capability");
        reason = QStringLiteral("The requested capability is unavailable.");
        guidance = QStringLiteral("Choose a supported target or backend. For GPU-only failures, try Optimized CPU and include the GPU and driver version in your report.");
    } else if (details.startsWith("invalid ", Qt::CaseInsensitive) ||
               details.startsWith("configuration ", Qt::CaseInsensitive)) {
        category = QStringLiteral("configuration");
        reason = QStringLiteral("The search configuration was rejected.");
        guidance = QStringLiteral("Correct the setting named in the technical details, then retry. Other settings can be kept.");
    }
    return {{"stage", stage}, {"category", category}, {"reason", reason},
            {"guidance", guidance}, {"details", details}};
}

inline QString FormatSearchDiagnostic(const QVariantMap &diagnostic) {
    return QStringLiteral("Stage: %1\nCategory: %2\n%3\n%4\n\nTechnical details:\n%5")
            .arg(diagnostic.value("stage").toString(),
                 diagnostic.value("category").toString(),
                 diagnostic.value("reason").toString(),
                 diagnostic.value("guidance").toString(),
                 diagnostic.value("details").toString());
}

}  // namespace forevertas::app
#endif
