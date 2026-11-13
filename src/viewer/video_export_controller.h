#ifndef FOREVERTAS_VIEWER_VIDEO_EXPORT_CONTROLLER_H
#define FOREVERTAS_VIEWER_VIDEO_EXPORT_CONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <atomic>

class QThread;

namespace forevertas::viewer {

class RaceViewerController;

// Renders the race view's selected run to an MP4 file as the game shows a
// replay, with a render module that can (render_module_api.h), on a worker
// thread.
class VideoExportController : public QObject {
    Q_OBJECT
    // Whether this build can export video.
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QObject *viewer READ viewer WRITE setViewer NOTIFY
                       viewerChanged)
    Q_PROPERTY(QString outputPath READ outputPath WRITE setOutputPath NOTIFY
                       settingsChanged)
    // The game's race camera: 1, 2 or 3.
    Q_PROPERTY(int camera READ camera WRITE setCamera NOTIFY settingsChanged)
    Q_PROPERTY(int videoWidth READ videoWidth WRITE setVideoWidth NOTIFY
                       settingsChanged)
    Q_PROPERTY(int videoHeight READ videoHeight WRITE setVideoHeight NOTIFY
                       settingsChanged)
    Q_PROPERTY(int framesPerSecond READ framesPerSecond WRITE
                       setFramesPerSecond NOTIFY settingsChanged)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    // 0 to 1 while running.
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    // The last file finished.
    Q_PROPERTY(QString finishedPath READ finishedPath NOTIFY stateChanged)

public:
    explicit VideoExportController(QObject *parent = nullptr);
    ~VideoExportController() override;

    bool available() const;
    QObject *viewer() const;
    void setViewer(QObject *viewer);
    QString outputPath() const;
    void setOutputPath(const QString &path);
    int camera() const;
    void setCamera(int camera);
    int videoWidth() const;
    void setVideoWidth(int width);
    int videoHeight() const;
    void setVideoHeight(int height);
    int framesPerSecond() const;
    void setFramesPerSecond(int framesPerSecond);
    bool running() const;
    double progress() const;
    QString status() const;
    QString finishedPath() const;

    // Asks where to save, with the system's dialog.
    Q_INVOKABLE bool chooseOutput();
    Q_INVOKABLE bool start();
    Q_INVOKABLE void abort();
    Q_INVOKABLE void showFinished() const;

signals:
    void viewerChanged();
    void settingsChanged();
    void stateChanged();
    void progressChanged();

private:
    void finish(bool succeeded, const QString &error);
    void pollProgress();
    void saveSettings() const;
    QString defaultOutputPath() const;

    QPointer<RaceViewerController> viewer_;
    QString outputPath_;
    int camera_ = 1;
    int videoWidth_ = 1920;
    int videoHeight_ = 1080;
    int framesPerSecond_ = 60;
    bool running_ = false;
    double progress_ = 0.0;
    QString status_;
    QString finishedPath_;
    QString runningPath_;

    QThread *worker_ = nullptr;
    QTimer progressTimer_;
    // Shared with the worker.
    std::atomic<int> framesDone_{0};
    std::atomic<int> frameCount_{0};
    volatile int cancel_ = 0;
};

}  // namespace forevertas::viewer

#endif
