// The Gates target on states made up for it: the crossing instant and what
// is checked there are interpolated; a crossing outside the limits is a
// miss; earlier gates are waypoints taken in order; soft scoring ranks a
// closer miss higher; and the gate list's mistakes are named.
#include "evaluators/gate_entry_evaluator.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace forevertas;
using forevervalidator::experimental::PhysicsSandboxStateView;

void Check(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}

// A car at (x, y, z) at timeMs, heading +Z at `speed` m/s, four wheels down.
PhysicsSandboxStateView State(std::int64_t timeMs, double x, double y, double z, double speed = 20.0) {
    PhysicsSandboxStateView state;
    state.timeMs = static_cast<std::uint64_t>(timeMs);
    state.car.position = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    state.car.linearSpeed = {0.0f, 0.0f, static_cast<float>(speed)};
    state.car.rotationW = 1.0f;
    state.car.wheelContact = {true, true, true, true};
    return state;
}

// The best sample a gate list gives a run along +Z through `zs` (one state
// per 10 ms tick).
std::optional<EvaluationSample> Run(const std::string &gates, const std::vector<double> &zs, double x = 0.0,
                                    double speed = 20.0, const std::string &soft = "true") {
    OptionSettings settings = DefaultGateEntryOptionSettings();
    settings["gates"] = gates;
    settings["soft"] = soft;
    const auto evaluator = CreateGateEntryEvaluator(settings, 10u);
    auto session = evaluator->CreateSession();
    std::optional<PhysicsSandboxStateView> previous;
    std::optional<EvaluationSample> best;
    for (std::size_t index = 0; index < zs.size() && !session->IsComplete(); ++index) {
        const PhysicsSandboxStateView current = State(static_cast<std::int64_t>(index) * 10, x, 1.0, zs[index], speed);
        if (auto sample = session->Observe(previous, current)) {
            if (!best || evaluator->IsBetter(*sample, *best)) best = sample;
        }
        previous = current;
    }
    return best;
}

}  // namespace

int main() {
    try {
        // Crossing z = 10 between z 9.5 (t 0) and 10.5 (t 10): at 5 ms.
        auto crossed = Run("x=0,y=0,z=10,dir=0,hw=4", {9.5, 10.5});
        Check(crossed && std::fabs(crossed->score - 5.0) < 1e-9 && crossed->score < kGateMissScoreBase,
              "the crossing was not at 5 ms");
        // The same, 6 m to the side of a 4 m half width: a miss, past its limits.
        auto wide = Run("x=0,y=0,z=10,dir=0,hw=4", {9.5, 10.5}, 6.0);
        Check(wide && wide->score > kGateMissScoreBase &&
                      wide->description.find("outside its limits") != std::string::npos,
              "a crossing outside the gate counted");
        // Too fast for vmax: a miss.
        auto fast = Run("x=0,y=0,z=10,dir=0,hw=4,vmax=50", {9.5, 10.5}, 0.0, 30.0);
        Check(fast && fast->score > kGateMissScoreBase, "a crossing over vmax counted");
        // Waypoint at z 5 then the scored gate at z 10: the score is the second crossing.
        auto both = Run("x=0,y=0,z=5,dir=0|x=0,y=0,z=10,dir=0", {4.5, 5.5, 7.0, 9.0, 9.5, 10.5});
        Check(both && std::fabs(both->score - 45.0) < 1e-9, "the scored gate after a waypoint was not at 45 ms");
        // Reaching the scored gate without the waypoint (passed beside it): a miss.
        auto skipped = Run("x=40,y=0,z=5,dir=0|x=0,y=0,z=10,dir=0", {4.5, 5.5, 9.5, 10.5});
        Check(skipped && skipped->score > kGateMissScoreBase, "the waypoint could be skipped");
        // Soft scoring: closer misses rank higher; without it, no sample at all.
        auto near = Run("x=0,y=0,z=100,dir=0", {0.0, 50.0, 90.0});
        auto far = Run("x=0,y=0,z=100,dir=0", {0.0, 10.0, 20.0});
        Check(near && far && near->score < far->score, "a closer miss did not rank higher");
        Check(!Run("x=0,y=0,z=100,dir=0", {0.0, 50.0}, 0.0, 20.0, "false"), "a hard miss gave a score");
        // The gate list's mistakes are named.
        std::string error;
        Check(!ParseGateList("x=0,y=0,z=0", &error) && error.find("dir") != std::string::npos,
              "a gate without dir was taken");
        Check(!ParseGateList("x=0,y=0,z=0,dir=0,wheels=5", &error) && error.find("wheels") != std::string::npos,
              "wheels=5 was taken");
        Check(!ParseGateList("x=0,y=0,z=0,dir=0,colour=3", &error) && error.find("colour") != std::string::npos,
              "an unknown field was taken");
        Check(ValidateGateEntryOptionSettings({{"gates", "x=0,y=0,z=0,dir=0"}, {"soft", "maybe"}}, 10u).has_value(),
              "soft=maybe was taken");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "gate crossings, limits, waypoints, soft scoring and gate list errors hold\n";
    return 0;
}
