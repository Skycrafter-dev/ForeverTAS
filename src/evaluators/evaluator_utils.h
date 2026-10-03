#ifndef FOREVERTAS_EVALUATORS_EVALUATOR_UTILS_H
#define FOREVERTAS_EVALUATORS_EVALUATOR_UTILS_H

#include "searches/option_configuration.h"
#include "searches/option_settings_utils.h"
#include "time_format.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>

#include <forevervalidator/experimental/physics_sandbox.h>

namespace forevertas {

struct EvaluationVector3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

inline std::optional<double> ReadDoubleSetting(
        const OptionSettings &settings,
        const char *key) {
    const auto found = settings.find(key);
    return found == settings.end()
            ? std::nullopt
            : ParseFiniteDouble(found->second);
}

inline std::optional<std::int64_t> ReadTimeSetting(
        const OptionSettings &settings,
        const char *key) {
    const auto found = settings.find(key);
    return found == settings.end()
            ? std::nullopt
            : ParseSignedDecimal(found->second);
}

inline std::optional<EvaluationVector3> ReadVector3Settings(
        const OptionSettings &settings,
        const char *xKey,
        const char *yKey,
        const char *zKey) {
    const auto x = ReadDoubleSetting(settings, xKey);
    const auto y = ReadDoubleSetting(settings, yKey);
    const auto z = ReadDoubleSetting(settings, zKey);
    if (!x || !y || !z) return std::nullopt;
    return EvaluationVector3{*x, *y, *z};
}

inline EvaluationVector3 PositionOf(
        const forevervalidator::experimental::PhysicsSandboxStateView &state) {
    return {state.car.position.x, state.car.position.y, state.car.position.z};
}

inline EvaluationVector3 VelocityOf(
        const forevervalidator::experimental::PhysicsSandboxStateView &state) {
    return {state.car.linearSpeed.x,
            state.car.linearSpeed.y,
            state.car.linearSpeed.z};
}

inline double Dot(const EvaluationVector3 &left,
                  const EvaluationVector3 &right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

inline double Length(const EvaluationVector3 &value) {
    return std::sqrt(Dot(value, value));
}

inline double Distance(const EvaluationVector3 &left,
                       const EvaluationVector3 &right) {
    return Length({left.x - right.x, left.y - right.y, left.z - right.z});
}

inline EvaluationVector3 Normalize(const EvaluationVector3 &value) {
    const double length = Length(value);
    return length <= 1e-12
            ? EvaluationVector3{}
            : EvaluationVector3{value.x / length,
                                value.y / length,
                                value.z / length};
}

inline std::string MetricDescription(const char *name,
                                     double value,
                                     const char *unit,
                                     double timeMs) {
    std::ostringstream stream;
    stream.precision(9);
    stream << name << ": " << value;
    if (unit != nullptr && *unit != '\0') stream << ' ' << unit;
    stream << " at " << FormatRaceTimeMilliseconds(timeMs);
    return stream.str();
}

inline std::string TimeMetricDescription(const char *name, double timeMs) {
    return std::string(name) + ": " +
            FormatInterpolatedTimeMilliseconds(timeMs);
}

}  // namespace forevertas

#endif
