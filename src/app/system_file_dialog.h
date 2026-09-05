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
QString SaveSystemFileDialog(const QString &title,
                            const QString &initialPath);

}  // namespace forevertas::app

#endif
