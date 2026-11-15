#include "viewer/gamepad_input.h"
#include "app/app_settings.h"
#include "diagnostics/diagnostics_log.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>

namespace forevertas::viewer {

GamepadInput::GamepadInput(QObject *parent) : QObject(parent) {
    // "" (never chosen, or None before there was a choice): automatic
    const QString saved = app::AppSettings::Instance().value("drive/gamepadIdentity").toString();
    choice_ = saved.isEmpty() ? QStringLiteral("auto") : saved;
    setDeadzone(app::AppSettings::Instance().value("drive/gamepadDeadzone", 0.12).toDouble());
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    initialized_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!initialized_) {
        status_ = QString::fromUtf8(SDL_GetError());
        diagnostics::Log("gamepad: SDL could not start: %s", qUtf8Printable(status_));
        return;
    }
    SDL_SetGamepadEventsEnabled(false);
    SDL_SetJoystickEventsEnabled(false);
    timer_.setInterval(10);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &GamepadInput::poll);
    timer_.start();
    poll();
}

GamepadInput::~GamepadInput() {
    disconnect(this, nullptr, nullptr, nullptr);
    close();
    if (initialized_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

QString GamepadInput::identity(SDL_JoystickID id) const {
    char guid[33]{};
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof(guid));
    // Include the connection path so identical controllers are never silently
    // substituted. A changed port can be selected again explicitly.
    return QString::fromLatin1(guid) + ":" + QString::fromUtf8(SDL_GetGamepadPathForID(id));
}

void GamepadInput::close() {
    emit sample(0, false, false);
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
        if ((buttons_ >> button) & 1u) emit buttonChanged(button, false);
    }
    buttons_ = 0;
    if (gamepad_) SDL_CloseGamepad(gamepad_);
    gamepad_ = nullptr;
    selectedId_ = 0;
    armed_ = false;
}

void GamepadInput::open(SDL_JoystickID id) {
    gamepad_ = SDL_OpenGamepad(id);
    if (gamepad_) {
        selectedId_ = id;
        diagnostics::Log("gamepad: using %s", SDL_GetGamepadName(gamepad_));
    } else {
        status_ = QString::fromUtf8(SDL_GetError());
        diagnostics::Log("gamepad: could not open %s: %s", SDL_GetGamepadNameForID(id), qUtf8Printable(status_));
    }
}

void GamepadInput::setChoice(const QString &choice) {
    const QString next = choice.isEmpty() ? QStringLiteral("auto") : choice;
    if (next == choice_) return;
    close();
    choice_ = next;
    // "auto" is kept as no choice, as before there were choices
    app::AppSettings::Instance().setValue("drive/gamepadIdentity",
                                          choice_ == QLatin1String("auto") ? QString() : choice_);
    poll();
    emit devicesChanged();
}

void GamepadInput::setSelectedId(quint32 id) {
    if (id == 0) {
        setChoice(QStringLiteral("none"));
    } else if (initialized_ && SDL_IsGamepad(id)) {
        setChoice(identity(id));
        if (selectedId_ != id) {
            close();
            open(id);
            emit devicesChanged();
        }
    }
}

// Every controller SDL sees, once each time they change: what it is, and
// whether it has a gamepad layout (without one it cannot drive).
void GamepadInput::logControllers() {
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    QStringList lines;
    QStringList unmapped;
    for (int i = 0; i < count; ++i) {
        const QString name = QString::fromUtf8(SDL_GetJoystickNameForID(ids[i]));
        const bool gamepad = SDL_IsGamepad(ids[i]);
        lines << QStringLiteral("%1 (%2:%3)%4")
                         .arg(name)
                         .arg(SDL_GetJoystickVendorForID(ids[i]), 4, 16, QLatin1Char('0'))
                         .arg(SDL_GetJoystickProductForID(ids[i]), 4, 16, QLatin1Char('0'))
                         .arg(gamepad ? QString() : QStringLiteral(", no gamepad layout"));
        if (!gamepad) unmapped << name;
    }
    SDL_free(ids);
    const QString text = lines.isEmpty() ? QStringLiteral("none") : lines.join(QStringLiteral("; "));
    if (unmapped != unmapped_) unmapped_ = unmapped;
    if (text == logged_) return;
    logged_ = text;
    diagnostics::Log("gamepad: controllers: %s", qUtf8Printable(text));
}

void GamepadInput::setDeadzone(double value) {
    if (!std::isfinite(value)) return;
    deadzone_ = std::clamp(value, 0.0, 0.5);
    app::AppSettings::Instance().setValue("drive/gamepadDeadzone", deadzone_);
    emit settingsChanged();
}

void GamepadInput::setActive(bool value) {
    if (active_ == value) return;
    active_ = value;
    armed_ = false;
    if (!active_) emit sample(0, false, false);
    emit settingsChanged();
}

int GamepadInput::QuantizeSteering(int axis, double deadzone) {
    if (!std::isfinite(deadzone)) return 0;
    const double normalized = std::clamp(axis, -32768, 32767) / (axis < 0 ? 32768.0 : 32767.0);
    const double zone = std::clamp(deadzone, 0.0, 0.5);
    if (std::abs(normalized) <= zone) return 0;
    return static_cast<int>(std::lround(std::copysign(
            (std::abs(normalized) - zone) / (1.0 - zone), normalized) * 65536.0));
}

void GamepadInput::poll() {
    if (!initialized_) return;
    SDL_UpdateGamepads();
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    QVariantList next;
    int matching = 0;
    SDL_JoystickID reconnect = 0;
    for (int i = 0; i < count; ++i) {
        const QString id = identity(ids[i]);
        next.append(QVariantMap{{"id", ids[i]}, {"name", QString::fromUtf8(SDL_GetGamepadNameForID(ids[i]))},
                                {"identity", id}});
        if (id == choice_) {
            ++matching;
            reconnect = ids[i];
        }
    }
    // the chosen gamepad plugged into another port: the one with its GUID
    if (matching == 0 && choice_.contains(QLatin1Char(':'))) {
        const QString guid = choice_.section(QLatin1Char(':'), 0, 0);
        for (int i = 0; i < count; ++i) {
            if (identity(ids[i]).section(QLatin1Char(':'), 0, 0) == guid) {
                ++matching;
                reconnect = ids[i];
            }
        }
    }
    // automatic: the first connected
    if (choice_ == QLatin1String("auto") && count > 0) {
        matching = 1;
        reconnect = ids[0];
    }
    SDL_free(ids);
    const QStringList unmappedBefore = unmapped_;
    bool changed = next != devices_;
    devices_ = std::move(next);
    if (changed || logged_.isEmpty()) logControllers();
    changed |= unmapped_ != unmappedBefore;
    if (gamepad_ && !SDL_GamepadConnected(gamepad_)) {
        diagnostics::Log("gamepad: %s disconnected", SDL_GetGamepadName(gamepad_));
        close();
        changed = true;
    }
    if (!gamepad_ && matching == 1) {
        open(reconnect);
        changed = true;
    }
    const QString status = gamepad_ ? QString::fromUtf8(SDL_GetGamepadName(gamepad_))
            : choice_ == QLatin1String("none") ? tr("No gamepad: None is chosen")
            : choice_ == QLatin1String("auto") ? tr("No gamepad connected")
            : tr("The chosen gamepad is not connected");
    changed |= status_ != status;
    status_ = status;
    if (changed) emit devicesChanged();
    if (!gamepad_) return;
    quint64 buttons = 0;
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
        if (SDL_GetGamepadButton(gamepad_, static_cast<SDL_GamepadButton>(button))) buttons |= quint64{1} << button;
    }
    const quint64 flipped = buttons ^ buttons_;
    buttons_ = buttons;
    for (int button = 0; flipped != 0 && button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
        if (!((flipped >> button) & 1u)) continue;
        const bool pressed = ((buttons >> button) & 1u) != 0;
        if (pressed) {
            const char *label = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(button));
            lastButton_ = label ? QString::fromUtf8(label) : QString::number(button);
            emit lastButtonChanged();
        }
        emit buttonChanged(button, pressed);
    }
    if (!active_) return;
    const int steering = QuantizeSteering(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX), deadzone_);
    const bool accelerate = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384 ||
            SDL_GetGamepadButton(gamepad_, SDL_GAMEPAD_BUTTON_SOUTH);
    const bool brake = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384 ||
            SDL_GetGamepadButton(gamepad_, SDL_GAMEPAD_BUTTON_EAST);
    // Focus changes and reconnection require a neutral sample before takeover.
    if (!armed_) {
        armed_ = steering == 0 && !accelerate && !brake;
        return;
    }
    emit sample(steering, accelerate, brake);
}
}  // namespace forevertas::viewer
