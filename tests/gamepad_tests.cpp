#include "viewer/gamepad_input.h"
#include "viewer/race_viewer_controller.h"
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <SDL3/SDL.h>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
template <typename Predicate> void Wait(Predicate predicate, int timeout = 60000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    Check(predicate(), "gamepad test timed out");
}
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    QGuiApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName("gamepad-test");
    QCoreApplication::setApplicationName("gamepad-test");
    using namespace forevertas::viewer;
    try {
        Check(GamepadInput::QuantizeSteering(-32768, .12) == -65536 &&
              GamepadInput::QuantizeSteering(32767, .12) == 65536 &&
              GamepadInput::QuantizeSteering(1000, .12) == 0 &&
              GamepadInput::QuantizeSteering(-1000, .12) == 0 &&
              GamepadInput::QuantizeSteering(16384, 0) == 32769,
              "axis quantization or deadzone is wrong");
        RaceViewerController viewer;
        auto &pad = *viewer.gamepad();
        SDL_VirtualJoystickDesc desc{};
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        desc.name = "ForeverTAS virtual gamepad";
        const auto id = SDL_AttachVirtualJoystick(&desc);
        Check(id != 0, SDL_GetError());
        SDL_Joystick *joystick = SDL_OpenJoystick(id);
        Check(joystick != nullptr, SDL_GetError());
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
        pad.poll();
        Check(!pad.devices().isEmpty() && pad.selectedId() == 0, "enumeration or explicit selection failed");
        pad.setSelectedId(id);
        pad.setDeadzone(.2);
        Check(pad.selectedId() == id && QSettings().value("drive/gamepadDeadzone").toDouble() == .2 &&
              !QSettings().value("drive/gamepadIdentity").toString().isEmpty(), "settings did not persist");
        const auto packs = QString::fromLocal8Bit(argv[1]);
        const auto replay = QString::fromLocal8Bit(argv[2]);
        viewer.setSimulationHorizonMs(1500);
        viewer.setPreviewInputScript("0.00 press up");
        viewer.loadMap(packs, replay);
        Wait([&] { return viewer.loaded() && !viewer.loading() && viewer.tickCount() > 1; });
        viewer.startManualDrive();
        Check(viewer.manualDriving(), "manual drive did not start");
        const auto advance = [&] {
            const auto tick = viewer.currentTick();
            Wait([&] { return viewer.currentTick() >= tick + 3; });
        };
        pad.setActive(true);
        pad.poll();
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, 32767);
        SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
        pad.poll();
        advance();
        Check(viewer.inputSample(viewer.currentTick()).steering == 1 && viewer.manualAccelerate(),
              "virtual input did not reach the canonical tick");
        viewer.setManualInput("left", true);
        pad.poll();
        advance();
        Check(viewer.inputSample(viewer.currentTick()).steering == -1, "keyboard steering lost precedence");
        viewer.setManualInput("left", false);
        advance();
        Check(viewer.inputSample(viewer.currentTick()).steering == 1, "analog steering did not resume");
        viewer.setManualInput("accelerate", true);
        pad.setActive(false);
        advance();
        Check(viewer.inputSample(viewer.currentTick()).steering == 0 && viewer.manualAccelerate(),
              "focus release lost keyboard input or retained analog input");
        viewer.releaseManualInputs();
        pad.setActive(true);
        pad.poll();
        Check(!viewer.manualAccelerate(), "held input rearmed without neutral");
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, 0);
        SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, false);
        pad.poll();
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, -16384);
        pad.poll();
        advance();
        Check(viewer.inputSample(viewer.currentTick()).steering < 0, "neutral rearm failed");
        SDL_CloseJoystick(joystick);
        SDL_DetachVirtualJoystick(id);
        pad.poll();
        advance();
        Check(pad.selectedId() == 0 && viewer.inputSample(viewer.currentTick()).steering == 0,
              "disconnect retained gamepad input");
        pad.setActive(false);
        viewer.stopManualDrive();
        const QString script = viewer.currentInputScript();
        const auto endTime = viewer.timeMs();
        const auto position = viewer.carPosition();
        const auto rotation = viewer.carRotation();
        {
            RaceViewerController replayed;
            replayed.setSimulationHorizonMs(1500);
            replayed.setPreviewInputScript(script);
            replayed.loadMap(packs, replay);
            Wait([&] { return replayed.loaded() && !replayed.loading() && replayed.tickCount() > endTime / 10; });
            replayed.setTimeMs(endTime);
            Check(replayed.carPosition() == position && replayed.carRotation() == rotation,
                  "recorded gamepad script does not replay to the same pose");
        }
        viewer.setSelectedRunId("preview");
        viewer.setTakeOverOnInput(true);
        viewer.setTimeMs(50);
        viewer.play();
        viewer.setGamepadInput(23456, false, false);
        Check(viewer.manualDriving() && viewer.manualSteeringTakenOver(), "analog takeover failed");
        advance();
        Check(viewer.inputSample(viewer.currentTick()).steering == 23456.0f / 65536.0f,
              "takeover value did not survive simulation");
        viewer.stopManualDrive();
        std::cout << "virtual gamepad, focus/disconnect, precedence, replay and takeover passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
