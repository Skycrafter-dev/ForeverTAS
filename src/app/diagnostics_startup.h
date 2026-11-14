#ifndef FOREVERTAS_APP_DIAGNOSTICS_STARTUP_H
#define FOREVERTAS_APP_DIAGNOSTICS_STARTUP_H

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QVariantMap>
#include <QObject>
#include <QString>
#include <QVariant>

class QQmlApplicationEngine;
class QQuickWindow;

namespace forevertas::app {

// Opens this session's log in the logs folder (the newest logs kept),
// installs the crash handler (stack, and on Windows a minidump), routes Qt's
// and QML's messages into the log, and records the computer: system, CPU,
// memory, GPUs and drivers. Call right after the QApplication is made.
bool StartDiagnostics();
// Which GPU runtime libraries are loaded, with their versions: call after
// the GPU backends are queried.
void LogLoadedGpuRuntimes();
// Where the window's QML came from: import paths, the ForeverTAS QML plugin
// loaded, the main file, QML settings in the environment and the QML cache.
// Call after the window is loaded.
void LogQmlSetup(const QQmlApplicationEngine &engine);
// The window's graphics: API, renderer and driver, screens, and scene graph
// errors.
void WatchWindow(QQuickWindow *window);
// The last line before a normal exit.
void FinishDiagnostics();

// The logs for QML: Help > Open log folder, and the notice after a session
// that did not close normally.
class DiagnosticsLogs : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString folder READ folder CONSTANT)
    Q_PROPERTY(QString logPath READ logPath CONSTANT)
    Q_PROPERTY(QString previousSessionLog READ previousSessionLog NOTIFY previousSessionChanged)
    Q_PROPERTY(bool previousSessionCrashed READ previousSessionCrashed NOTIFY previousSessionChanged)

public:
    explicit DiagnosticsLogs(QObject *parent = nullptr);

    QString folder() const;
    QString logPath() const;
    // The previous session's log when that session did not close normally.
    QString previousSessionLog() const { return previousSessionLog_; }
    // ... and it left a crash report (not only a forced close).
    bool previousSessionCrashed() const { return previousSessionCrashed_; }

    Q_INVOKABLE bool openFolder() const;
    Q_INVOKABLE void dismissPreviousSession();
    // A line from QML (what the user did).
    Q_INVOKABLE void note(const QString &text) const;
    // Help > Check the car: keep one grabbed picture (QImage; Qt frees the
    // grab once its callback returns), then count how many pixels of another
    // differ from it; -1 when they cannot be compared.
    Q_INVOKABLE void keepPicture(const QVariant &image);
    Q_INVOKABLE int differingPixels(const QVariant &image) const;
    // The car probe: in a picture of the race view, the share of pixels
    // within radius of center that have the car's hue (its color shaded and
    // tone mapped). {fraction, samples}, or {skipped: reason} when the color
    // has no hue to look for.
    Q_INVOKABLE QVariantMap carPixels(const QVariant &image, const QPointF &center, qreal radius,
                                      const QColor &color) const;
    // Saves a picture next to the session log, named with suffix; the path.
    Q_INVOKABLE QString savePicture(const QVariant &image, const QString &suffix) const;
    // The same for the picture keepPicture kept.
    Q_INVOKABLE QString saveKeptPicture(const QString &suffix) const;

signals:
    void previousSessionChanged();

private:
    QString previousSessionLog_;
    bool previousSessionCrashed_ = false;
    QImage keptPicture_;
};

}  // namespace forevertas::app

#endif
