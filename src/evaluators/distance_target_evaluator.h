#ifndef FOREVERTAS_EVALUATORS_DISTANCE_TARGET_EVALUATOR_H
#define FOREVERTAS_EVALUATORS_DISTANCE_TARGET_EVALUATOR_H

#include "evaluators/iteration_evaluator.h"
#include "searches/option_configuration.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace forevertas {

OptionSettings DefaultDistanceTargetOptionSettings();
std::optional<std::string> ValidateDistanceTargetOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<IterationEvaluator> CreateDistanceTargetEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

// Shortest distance between the car's collision shapes (car-local
// ellipsoids, placed with the car's pose) and a trigger surface, for a car
// near it; 0 when they touch. Exposed for tests.
double CarDistanceToTriangles(
        const std::vector<forevervalidator::experimental::PhysicsSandboxEllipsoid>
                &carEllipsoids,
        const std::vector<
                forevervalidator::experimental::PhysicsSandboxCollisionTriangle>
                &triangles,
        const forevervalidator::experimental::PhysicsSandboxCarState &car);

}  // namespace forevertas

#endif
