#include "app/system_file_dialog.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QWindow>

#if defined(Q_OS_LINUX)
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QEventLoop>
#include <QUrl>
#include <QVariantMap>

#include <atomic>

namespace forevertas::app {
namespace {

constexpr char kPortalService[] = "org.freedesktop.portal.Desktop";
constexpr char kPortalPath[] = "/org/freedesktop/portal/desktop";
constexpr char kFileChooserInterface[] =
        "org.freedesktop.portal.FileChooser";
constexpr char kRequestInterface[] = "org.freedesktop.portal.Request";

class PortalResponse final : public QObject {
    Q_OBJECT

public:
    QString selectedPath;
    QEventLoop loop;

public slots:
    void HandleResponse(uint response, const QVariantMap &results) {
        if (response == 0u) {
            const QStringList uris =
                    results.value(QStringLiteral("uris")).toStringList();
            if (!uris.isEmpty()) {
                selectedPath = QUrl(uris.front()).toLocalFile();
            }
        }
        loop.quit();
    }
};

QString PortalParentWindow() {
    if (qEnvironmentVariable("XDG_SESSION_TYPE").compare(
                QStringLiteral("wayland"), Qt::CaseInsensitive) == 0) {
        return {};
    }
    QWindow *const window = QGuiApplication::focusWindow();
    if (window == nullptr || QGuiApplication::platformName() !=
                                     QStringLiteral("xcb")) {
        return {};
    }
    return QStringLiteral("x11:%1").arg(
            static_cast<qulonglong>(window->winId()), 0, 16);
}

QByteArray PortalPathBytes(const QString &path) {
    QByteArray bytes = QDir::toNativeSeparators(path).toUtf8();
    bytes.append('\0');
    return bytes;
}

QString OpenPortalDialog(const QString &title,
                         const QString &initialPath,
                         bool directory, bool save = false) {
    QDBusConnection connection = QDBusConnection::sessionBus();
    if (!connection.isConnected()) return {};

    static std::atomic_uint64_t sequence{0u};
    const QString token = QStringLiteral("forevertas_%1_%2")
                                  .arg(QCoreApplication::applicationPid())
                                  .arg(sequence.fetch_add(1u));
    QString sender = connection.baseService();
    sender.remove(0, sender.startsWith(QLatin1Char(':')) ? 1 : 0);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString requestPath =
            QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
                    .arg(sender, token);

    PortalResponse response;
    if (!connection.connect(
                kPortalService,
                requestPath,
                kRequestInterface,
                QStringLiteral("Response"),
                &response,
                SLOT(HandleResponse(uint,QVariantMap)))) {
        return {};
    }

    const QFileInfo initial(initialPath);
    const QString initialDirectory = directory
            ? initialPath
            : (initial.isDir() ? initial.absoluteFilePath()
                               : initial.absolutePath());
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("multiple"), false);
    options.insert(QStringLiteral("directory"), directory);
    if (save) options.insert(QStringLiteral("current_name"), initial.fileName());
    if (!initialDirectory.isEmpty()) {
        options.insert(QStringLiteral("current_folder"),
                       PortalPathBytes(initialDirectory));
    }

    QDBusMessage message = QDBusMessage::createMethodCall(
            kPortalService,
            kPortalPath,
            kFileChooserInterface,
            save ? QStringLiteral("SaveFile") : QStringLiteral("OpenFile"));
    message << PortalParentWindow() << title << options;
    const QDBusMessage reply = connection.call(message);
    if (reply.type() == QDBusMessage::ErrorMessage ||
        reply.arguments().isEmpty()) {
        connection.disconnect(
                kPortalService,
                requestPath,
                kRequestInterface,
                QStringLiteral("Response"),
                &response,
                SLOT(HandleResponse(uint,QVariantMap)));
        return {};
    }

    const QString returnedPath = qvariant_cast<QDBusObjectPath>(
                                         reply.arguments().front())
                                         .path();
    if (returnedPath != requestPath) {
        connection.disconnect(
                kPortalService,
                requestPath,
                kRequestInterface,
                QStringLiteral("Response"),
                &response,
                SLOT(HandleResponse(uint,QVariantMap)));
        if (!connection.connect(
                    kPortalService,
                    returnedPath,
                    kRequestInterface,
                    QStringLiteral("Response"),
                    &response,
                    SLOT(HandleResponse(uint,QVariantMap)))) {
            return {};
        }
    }

    response.loop.exec();
    connection.disconnect(
            kPortalService,
            returnedPath,
            kRequestInterface,
            QStringLiteral("Response"),
            &response,
            SLOT(HandleResponse(uint,QVariantMap)));
    return response.selectedPath;
}

}  // namespace

SystemFileDialogBackend ActiveSystemFileDialogBackend() {
    return SystemFileDialogBackend::XdgDesktopPortal;
}

QString OpenSystemDirectoryDialog(const QString &title,
                                  const QString &initialDirectory) {
    return OpenPortalDialog(title, initialDirectory, true);
}

QString OpenSystemFileDialog(const QString &title,
                             const QString &initialPath) {
    return OpenPortalDialog(title, initialPath, false);
}

QString SaveSystemFileDialog(const QString &title, const QString &initialPath) {
    return OpenPortalDialog(title, initialPath, false, true);
}

}  // namespace forevertas::app

#include "system_file_dialog.moc"

#elif defined(Q_OS_WIN)
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace forevertas::app {
namespace {

QString OpenWindowsDialog(const QString &title,
                          const QString &initialPath,
                          bool directory, bool save = false) {
    const HRESULT initialized = CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitialize = SUCCEEDED(initialized);

    IFileDialog *dialog = nullptr;
    HRESULT result = CoCreateInstance(
            save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        if (uninitialize) CoUninitialize();
        return {};
    }
    dialog->SetTitle(
            reinterpret_cast<const wchar_t *>(title.utf16()));

    FILEOPENDIALOGOPTIONS options = 0;
    result = dialog->GetOptions(&options);
    if (SUCCEEDED(result)) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        options |= directory ? FOS_PICKFOLDERS : save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST;
        result = dialog->SetOptions(options);
    }

    const QFileInfo initial(initialPath);
    if (save) dialog->SetFileName(reinterpret_cast<const wchar_t *>(initial.fileName().utf16()));
    const QString initialDirectory = directory
            ? initialPath
            : (initial.isDir() ? initial.absoluteFilePath()
                               : initial.absolutePath());
    if (SUCCEEDED(result) && !initialDirectory.isEmpty()) {
        IShellItem *folder = nullptr;
        result = SHCreateItemFromParsingName(
                reinterpret_cast<const wchar_t *>(
                        initialDirectory.utf16()),
                nullptr,
                IID_PPV_ARGS(&folder));
        if (SUCCEEDED(result)) {
            dialog->SetFolder(folder);
            folder->Release();
        }
        result = S_OK;
    }

    QString selected;
    if (SUCCEEDED(result) && SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                selected = QString::fromWCharArray(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }

    dialog->Release();
    if (uninitialize) CoUninitialize();
    return selected;
}

}  // namespace

SystemFileDialogBackend ActiveSystemFileDialogBackend() {
    return SystemFileDialogBackend::WindowsIFileDialog;
}

QString OpenSystemDirectoryDialog(const QString &title,
                                  const QString &initialDirectory) {
    return OpenWindowsDialog(title, initialDirectory, true);
}

QString OpenSystemFileDialog(const QString &title,
                             const QString &initialPath) {
    return OpenWindowsDialog(title, initialPath, false);
}

QString SaveSystemFileDialog(const QString &title, const QString &initialPath) {
    return OpenWindowsDialog(title, initialPath, false, true);
}

}  // namespace forevertas::app

#else
#error ForeverTAS requires a platform-native system file dialog implementation
#endif
