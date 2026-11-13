#ifndef FOREVERTAS_APP_SYSTEM_FILE_DIALOG_H
#define FOREVERTAS_APP_SYSTEM_FILE_DIALOG_H

#include <QString>

namespace forevertas::app {

enum class SystemFileDialogBackend {
    XdgDesktopPortal,
    WindowsIFileDialog
};

SystemFileDialogBackend ActiveSystemFileDialogBackend();

QString OpenSystemDirectoryDialog(const QString &title,
                                  const QString &initialDirectory);
QString OpenSystemFileDialog(const QString &title,
                             const QString &initialPath);
// Asks where to save a file: initialPath suggests the folder and name;
// extension (without the dot) is offered as filterName.
QString OpenSystemSaveDialog(const QString &title,
                             const QString &initialPath,
                             const QString &filterName,
                             const QString &extension);

}  // namespace forevertas::app

#endif
