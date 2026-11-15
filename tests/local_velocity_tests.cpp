// car.localvel (the car's velocity in its own axes) on a real replay, on
// each CPU backend: users found it 0 everywhere. It must have the world
// velocity's length and be that velocity turned into the car's axes, and the
// condition program must read the same value.
#include "conditions/condition_program.h"
#include "mutations/input_event_formatter.h"
#include "mutations/replay_input_script.h"
#include "replay_file_io.h"

#include <forevervalidator/native.h>

#include <QQuaternion>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace forevertas;
using namespace forevervalidator;
using namespace forevervalidator::experimental;

void Check(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}

template<typename Result>
auto Require(Result result) {
    if (!result) throw std::runtime_error(result.Error().diagnostic);
    return std::move(result.Value());
}

QVector3D ToQt(const Vector3 &v) { return {v.x, v.y, v.z}; }

void TestBackend(const char *packs, const char *path, SimulationBackend backend, const char *name) {
    PhysicsSandboxOptions options;
    options.backend = backend;
    options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
    options.simulationHorizonMs = 600000;
    auto sandbox = Require(CreatePhysicsSandbox(Require(OpenInstalledPackDirectory(packs)), options));
    ReplayIdentity identity{path};
    const auto bytes = Require(ReadReplayFileUtf8(path, identity));
    Require(sandbox.LoadReplay({bytes.data(), bytes.size()}, identity));
    const auto parsed = ParseInputScript(ExtractReplayInputScript(packs, path));
    Check(!parsed.error, "replay script failed to parse");
    const auto baseline = BuildInputScriptBaseline(Require(sandbox.ReadInputs()), parsed.commands, 10);
    Check(!baseline.error, "replay input conversion failed");
    Require(sandbox.ReplaceInputs(baseline.events));

    const auto compiled = CompileScalarExpression("car.localvel.z");
    Check(compiled.program.has_value(), "car.localvel.z did not compile");
    int moving = 0;
    double worst = 0.0;
    PhysicsSandboxStateView previous = Require(sandbox.AdvanceTicks(1));
    for (int tick = 1; tick < 3000; ++tick) {
        const PhysicsSandboxStateView view = Require(sandbox.AdvanceTicks(1));
        const QVector3D world = ToQt(view.car.linearSpeed);
        if (world.length() > 5.0f) {
            ++moving;
            const QQuaternion rotation(view.car.rotationW, view.car.rotationX, view.car.rotationY,
                                       view.car.rotationZ);
            const QVector3D expected = rotation.conjugated().rotatedVector(world);
            const QVector3D local = ToQt(view.car.localSpeed);
            worst = std::max(worst, double((local - expected).length()));
            Check((local - expected).length() < 0.05f * world.length() + 0.01f,
                  std::string(name) + ": car.localvel at tick " + std::to_string(tick) + " is (" +
                          std::to_string(local.x()) + ", " + std::to_string(local.y()) + ", " +
                          std::to_string(local.z()) + "), the velocity in the car's axes is (" +
                          std::to_string(expected.x()) + ", " + std::to_string(expected.y()) + ", " +
                          std::to_string(expected.z()) + ")");
            // the condition program reads the same value
            const auto read = compiled.program->Evaluate(previous, view, {});
            Check(read.has_value() && std::fabs(*read - local.z()) < 1e-4,
                  std::string(name) + ": the condition program reads another car.localvel.z");
        }
        previous = view;
        if (view.raceCompleted) break;
    }
    Check(moving > 50, std::string(name) + ": the replay hardly moved");
    std::cout << name << ": car.localvel matches the velocity in the car's axes over " << moving
              << " ticks (worst off by " << worst << " m/s)\n";
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: forevertas-local-velocity-tests PACKS REPLAY\n";
        return 2;
    }
    try {
        TestBackend(argv[1], argv[2], SimulationBackend::Reference, "Reference");
        TestBackend(argv[1], argv[2], SimulationBackend::OptimizedCpu, "Optimized CPU");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
