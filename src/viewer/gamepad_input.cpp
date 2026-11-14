#include "viewer/gamepad_input.h"
#include "app/app_settings.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>

namespace forevertas::viewer {

GamepadInput::GamepadInput(QObject *parent) : QObject(parent) {
    selectedIdentity_ = app::AppSettings::Instance().value("drive/gamepadIdentity").toString();
    setDeadzone(app::AppSettings::Instance().value("drive/gamepadDeadzone", 0.12).toDouble());
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    initialized_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!initialized_) {
        status_ = QString::fromUtf8(SDL_GetError());
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

void GamepadInput::setSelectedId(quint32 id) {
    if (id == selectedId_ && (id != 0 || selectedIdentity_.isEmpty())) return;
    close();
    selectedIdentity_.clear();
    if (id != 0 && initialized_ && SDL_IsGamepad(id)) {
        gamepad_ = SDL_OpenGamepad(id);
        if (gamepad_) {
            selectedId_ = id;
            selectedIdentity_ = identity(id);
        } else status_ = QString::fromUtf8(SDL_GetError());
    }
    app::AppSettings::Instance().setValue("drive/gamepadIdentity", selectedIdentity_);
    emit devicesChanged();
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
        next.append(QVariantMap{{"id", ids[i]}, {"name", QString::fromUtf8(SDL_GetGamepadNameForID(ids[i]))}});
        if (!selectedIdentity_.isEmpty() && identity(ids[i]) == selectedIdentity_) {
            ++matching;
            reconnect = ids[i];
        }
    }
    SDL_free(ids);
    bool changed = next != devices_;
    devices_ = std::move(next);
    if (gamepad_ && !SDL_GamepadConnected(gamepad_)) {
        close();
        changed = true;
    }
    if (!gamepad_ && matching == 1) {
        gamepad_ = SDL_OpenGamepad(reconnect);
        if (gamepad_) selectedId_ = reconnect;
        changed = true;
    }
    const QString status = gamepad_ ? QString::fromUtf8(SDL_GetGamepadName(gamepad_))
            : selectedIdentity_.isEmpty() ? tr("No gamepad selected") : tr("Selected gamepad disconnected");
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
        if ((flipped >> button) & 1u) emit buttonChanged(button, ((buttons >> button) & 1u) != 0);
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
