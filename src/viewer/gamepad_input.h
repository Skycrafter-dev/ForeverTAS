#ifndef FOREVERTAS_VIEWER_GAMEPAD_INPUT_H
#define FOREVERTAS_VIEWER_GAMEPAD_INPUT_H

#include <QObject>
#include <QTimer>
#include <QStringList>
#include <QVariantList>
#include <SDL3/SDL_gamepad.h>

namespace forevertas::viewer {

class GamepadInput final : public QObject {
    Q_OBJECT
    // The connected gamepads: {id, name, identity}.
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    // Which gamepad drives: "auto" (the first connected, the default),
    // "none", or one gamepad's identity.
    Q_PROPERTY(QString choice READ choice WRITE setChoice NOTIFY devicesChanged)
    // The gamepad in use (0: none); writing one chooses it.
    Q_PROPERTY(quint32 selectedId READ selectedId WRITE setSelectedId NOTIFY devicesChanged)
    // Controllers SDL sees as joysticks without a gamepad layout (unusable).
    Q_PROPERTY(QStringList unmapped READ unmapped NOTIFY devicesChanged)
    // The last button pressed on the gamepad in use, by name (to see input arrive).
    Q_PROPERTY(QString lastButton READ lastButton NOTIFY lastButtonChanged)
    Q_PROPERTY(double deadzone READ deadzone WRITE setDeadzone NOTIFY settingsChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY settingsChanged)
    Q_PROPERTY(QString status READ status NOTIFY devicesChanged)
public:
    explicit GamepadInput(QObject *parent = nullptr);
    ~GamepadInput() override;
    QVariantList devices() const { return devices_; }
    QString choice() const { return choice_; }
    void setChoice(const QString &choice);
    quint32 selectedId() const { return selectedId_; }
    QStringList unmapped() const { return unmapped_; }
    QString lastButton() const { return lastButton_; }
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
    void lastButtonChanged();
    void sample(int steering, bool accelerate, bool brake);
    // A button of the selected gamepad pressed or released (SDL_GamepadButton), whatever `active` is: the race's
    // actions (RaceControls) and their bindings take them.
    void buttonChanged(int button, bool pressed);
private:
    QString identity(SDL_JoystickID id) const;
    void close();
    void open(SDL_JoystickID id);
    void logControllers();
    QTimer timer_;
    QVariantList devices_;
    QStringList unmapped_;
    QString choice_ = QStringLiteral("auto");
    QString lastButton_;
    // the controllers last logged (logged again when they change)
    QString logged_;
    QString status_;
    SDL_Gamepad *gamepad_ = nullptr;
    quint32 selectedId_ = 0;
    double deadzone_ = 0.12;
    bool initialized_ = false;
    bool active_ = false;
    bool armed_ = false;
    quint64 buttons_ = 0;   // the buttons down at the last poll
};
}  // namespace forevertas::viewer
#endif
