#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <functional>

namespace forevertas::app {

class ShutdownController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool closing READ closing NOTIFY stateChanged)
    Q_PROPERTY(bool delayed READ delayed NOTIFY stateChanged)
public:
    ShutdownController(std::function<void()> begin, std::function<bool()> idle,
                       std::function<void()> persist, std::function<void()> forceExit,
                       int graceMs = 5000, QObject *parent = nullptr);
    bool closing() const { return closing_; }
    bool delayed() const { return delayed_; }
    Q_INVOKABLE bool requestClose();
    Q_INVOKABLE void forceExit();
signals:
    void stateChanged();
    void readyToClose();
private:
    void poll();
    std::function<void()> begin_, persist_, forceExit_;
    std::function<bool()> idle_;
    QTimer timer_;
    QElapsedTimer elapsed_;
    int graceMs_;
    bool closing_ = false;
    bool delayed_ = false;
    bool ready_ = false;
    bool forced_ = false;
};

}
