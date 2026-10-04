#ifndef FOREVERTAS_APP_SEARCH_DIAGNOSTIC_H
#define FOREVERTAS_APP_SEARCH_DIAGNOSTIC_H

#include <QString>
#include <QVariantMap>

namespace forevertas::app {

// Explains a failure message in plain words. The map holds "stage",
// "category", "reason" (what went wrong), "guidance" (what to do), the exact
// "details" and "text", the copyable report built by FormatSearchDiagnostic.
QVariantMap SearchDiagnostic(const QString &stage, const QString &details);

QString FormatSearchDiagnostic(const QVariantMap &diagnostic);

}  // namespace forevertas::app
#endif
