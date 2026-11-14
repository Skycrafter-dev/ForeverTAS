#pragma once

#include <QString>

namespace forevertas::app {

// Unpacks a GPU module (.tar.zst: plain files, no folders) into directory.
bool ExtractComputeArchive(const QString &archive, const QString &directory, QString *error);

}  // namespace forevertas::app
