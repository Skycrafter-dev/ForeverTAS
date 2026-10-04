#ifndef FOREVERTAS_VIEWER_GAMEPAD_INPUT_H
#define FOREVERTAS_VIEWER_GAMEPAD_INPUT_H

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <SDL3/SDL_gamepad.h>

namespace forevertas::viewer {

class GamepadInput final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(quint32 selectedId READ selectedId WRITE setSelectedId NOTIFY devicesChanged)
    Q_PROPERTY(double deadzone READ deadzone WRITE setDeadzone NOTIFY settingsChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY settingsChanged)
    Q_PROPERTY(QString status READ status NOTIFY devicesChanged)
public:
    explicit GamepadInput(QObject *parent = nullptr);
    ~GamepadInput() override;
    QVariantList devices() const { return devices_; }
    quint32 selectedId() const { return selectedId_; }
    double deadzone() const { return deadzone_; }
    bool active() const { return active_; }
    QString status() const { return status_; }
    void setSelectedId(quint32 id);
    void setDeadzone(double value);
    void setActive(bool value);
    Q_INVOKABLE void poll();
    static int QuantizeSteering(int axis, double deadzone);
signals:
    void devicesChanged();
    void settingsChanged();
    void sample(int steering, bool accelerate, bool brake);
private:
    QString identity(SDL_JoystickID id) const;
    void close();
    QTimer timer_;
    QVariantList devices_;
    QString selectedIdentity_;
    QString status_;
    SDL_Gamepad *gamepad_ = nullptr;
    quint32 selectedId_ = 0;
    double deadzone_ = 0.12;
    bool initialized_ = false;
    bool active_ = false;
    bool armed_ = false;
};
}  // namespace forevertas::viewer
#endif
