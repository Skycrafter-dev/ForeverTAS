#include "app/rolling_throughput.h"
#include "evaluators/iteration_evaluator.h"
#include "input_timeline_time.h"
#include "mutations/composite_input_mutator.h"
#include "mutations/input_event_formatter.h"
#include "mutations/input_event_utils.h"
#include "mutations/modifier_utils.h"
#include "replay_file_io.h"
#include "searches/algorithm_registry.h"
#include "searches/basic_brute_force_search.h"
#include "searches/cuda_batch_calibrator.h"
#include "searches/cuda_calibration_safety.h"
#include "searches/cuda_search_configuration.h"
#include "searches/option_settings_utils.h"
#include "searches/search_runner.h"
#include "searches/search_log_utils.h"
#include "time_format.h"

#include <algorithm>
#include <array>
#include <clocale>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using forevertas::AnalogInputState;
using forevertas::EvaluationSample;
using forevertas::InputMutator;
using forevertas::MutationRequest;
using forevertas::MutationResult;
using forevertas::OptionSettings;
using forevertas::SandboxInputAction;
using forevertas::SandboxInputEvent;
using forevervalidator::experimental::PhysicsSandboxInputValueKind;
using forevervalidator::experimental::PhysicsSandboxStateView;
using forevervalidator::experimental::PhysicsSandboxSwitchState;

bool Check(bool condition, const char *message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

class NumericLocaleGuard final {
public:
    NumericLocaleGuard() {
        if (const char *const current = std::setlocale(LC_NUMERIC, nullptr)) {
            original_ = current;
        }
    }

    ~NumericLocaleGuard() {
        if (!original_.empty()) {
            std::setlocale(LC_NUMERIC, original_.c_str());
        }
    }

    bool ActivateCommaDecimalLocale() {
        constexpr const char *localeNames[] = {
                "fr_FR.utf8", "fr_FR.UTF-8", "de_DE.utf8", "de_DE.UTF-8"};
        for (const char *const localeName : localeNames) {
            if (std::setlocale(LC_NUMERIC, localeName) == nullptr) continue;
            const lconv *const details = std::localeconv();
            if (details != nullptr && details->decimal_point != nullptr &&
                std::string(details->decimal_point) == ",") {
                return true;
            }
        }
        return false;
    }

private:
    std::string original_;
};

void ApplyOverrides(
        OptionSettings *settings,
        std::initializer_list<std::pair<const char *, const char *>> overrides) {
    for (const auto &[key, value] : overrides) {
        (*settings)[key] = value;
    }
}

bool ModifierAcceptsDotDecimals(
        const char *id,
        std::initializer_list<std::pair<const char *, const char *>> overrides) {
    const auto *const registration = forevertas::FindModifier(id);
    if (registration == nullptr) return false;
    OptionSettings settings = registration->defaultSettings;
    ApplyOverrides(&settings, overrides);
    return !registration->validateSettings(settings, 10u) &&
            registration->create(settings, 10u) != nullptr;
}

bool EvaluatorAcceptsDotDecimals(
        const char *id,
        std::initializer_list<std::pair<const char *, const char *>> overrides) {
    const auto *const registration = forevertas::FindEvaluationTarget(id);
    if (registration == nullptr) return false;
    OptionSettings settings = registration->defaultSettings;
    ApplyOverrides(&settings, overrides);
    return !registration->validateSettings(settings, 10u) &&
            registration->create(settings, 10u) != nullptr;
}

SandboxInputEvent Analog(std::int32_t timeMs,
                         SandboxInputAction action,
                         AnalogInputState value) {
    SandboxInputEvent event;
    event.timeMs = timeMs;
    event.action = action;
    event.value.kind = PhysicsSandboxInputValueKind::Analog;
    event.value.analog = value;
    return event;
}

SandboxInputEvent Steering(std::int32_t timeMs,
                           AnalogInputState value) {
    return Analog(timeMs, SandboxInputAction::Steer, value);
}

SandboxInputEvent Switch(std::int32_t timeMs,
                         SandboxInputAction action,
                         bool pressed) {
    SandboxInputEvent event;
    event.timeMs = timeMs;
    event.action = action;
    event.value.kind = PhysicsSandboxInputValueKind::Switch;
    event.value.switchState = pressed
            ? PhysicsSandboxSwitchState::Pressed
            : PhysicsSandboxSwitchState::Released;
    return event;
}

bool AllAnalogInputsValid(
        const std::vector<SandboxInputEvent> &events) {
    for (const SandboxInputEvent &event : events) {
        if (event.value.kind == PhysicsSandboxInputValueKind::Analog &&
            !forevervalidator::IsAnalogInputStateValid(
                    event.value.analog)) {
            return false;
        }
    }
    return true;
}

bool SameEvents(const std::vector<SandboxInputEvent> &left,
                const std::vector<SandboxInputEvent> &right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0u; index < left.size(); ++index) {
        if (!forevertas::SameInputEvent(left[index], right[index])) return false;
    }
    return true;
}

std::unique_ptr<forevertas::IterationEvaluator> Evaluator(
        const char *id,
        const OptionSettings *overrideSettings = nullptr) {
    const auto *const registration = forevertas::FindEvaluationTarget(id);
    if (registration == nullptr) return {};
    const OptionSettings &settings = overrideSettings == nullptr
            ? registration->defaultSettings
            : *overrideSettings;
    return registration->create(settings, 10u);
}


bool TestInputOnlyTimelineTimeOrigin() {
    const auto firstInput =
            forevertas::SimulationTimelineTimeFromUserTime(
                    0, forevertas::kInputTimelineTickDurationMs);
    const auto laterInput =
            forevertas::SimulationTimelineTimeFromUserTime(
                    1000, forevertas::kInputTimelineTickDurationMs);
    bool okay = Check(firstInput && *firstInput == 10 &&
                              laterInput && *laterInput == 1010,
                      "user timeline times were not shifted by one tick");
    okay &= Check(
            forevertas::UserTimelineTimeFromSimulationTime(
                    10, forevertas::kInputTimelineTickDurationMs) == 0 &&
                    forevertas::UserTimelineTimeFromSimulationTime(
                            1010,
                            forevertas::kInputTimelineTickDurationMs) == 1000,
            "simulation timeline times did not map back to the user origin");

    const OptionSettings sample{{"minTimeMs", "0"},
                                {"maxTimeMs", "1000"},
                                {"maxTimeShiftMs", "50"},
                                {"radiusMs", "200"},
                                {"maxSteerHoldMs", "300"}};
    const auto converted =
            forevertas::SimulationInputSettingsFromUserTimeline(
                    sample, forevertas::kInputTimelineTickDurationMs);
    okay &= Check(converted && converted->at("minTimeMs") == "10" &&
                              converted->at("maxTimeMs") == "1010" &&
                              converted->at("maxTimeShiftMs") == "50" &&
                              converted->at("radiusMs") == "200" &&
                              converted->at("maxSteerHoldMs") == "300",
                      "timeline conversion changed a duration setting");

    const auto verifyInputSettings = [&okay](const OptionSettings &settings) {
        const auto simulation =
                forevertas::SimulationInputSettingsFromUserTimeline(
                        settings,
                        forevertas::kInputTimelineTickDurationMs);
        if (!simulation) {
            okay &= Check(false,
                          "registered settings could not be timeline-converted");
            return;
        }
        for (const auto &[key, value] : settings) {
            if (forevertas::IsInputTimelineTimeSetting(key)) {
                const auto parsed = forevertas::ParseSignedDecimal(value);
                okay &= Check(parsed &&
                                      simulation->at(key) ==
                                              std::to_string(*parsed + 10),
                              "a registered absolute time was not shifted");
            } else {
                okay &= Check(simulation->at(key) == value,
                              "a registered non-time setting was shifted");
            }
        }
    };
    for (const auto &registration : forevertas::ModifierRegistry()) {
        verifyInputSettings(registration.defaultSettings);
    }

    const std::string largestAlignedTime =
            "9223372036854775800";
    const auto *const stuntRegistration =
            forevertas::FindEvaluationTarget(
                    forevertas::kStuntPointsEvaluationId);
    OptionSettings evaluationSettings =
            stuntRegistration->defaultSettings;
    evaluationSettings["targetTimeMs"] = largestAlignedTime;
    okay &= Check(
            !stuntRegistration->validateSettings(
                    evaluationSettings,
                    forevertas::kInputTimelineTickDurationMs),
            "a large non-input evaluation time was treated as an offset input "
            "time");
    const auto largeTimeEvaluator = stuntRegistration->create(
            evaluationSettings,
            forevertas::kInputTimelineTickDurationMs);
    const forevertas::EvaluationPlan largeTimePlan =
            largeTimeEvaluator->Plan(10000, 1010, 10u);
    okay &= Check(
            largeTimePlan.startTimeMs == 9223372036854775800ll &&
                    largeTimePlan.endTimeMs == 9223372036854775800ll,
            "a large non-input evaluation time was not preserved");

    const auto *const modifierRegistration = forevertas::FindModifier(
            forevertas::kRandomSteeringModifierId);
    OptionSettings inputSettings = modifierRegistration->defaultSettings;
    inputSettings["minTimeMs"] = largestAlignedTime;
    inputSettings["maxTimeMs"] = largestAlignedTime;
    okay &= Check(
            modifierRegistration->validateSettings(
                    inputSettings,
                    forevertas::kInputTimelineTickDurationMs)
                    .has_value(),
            "an overflowing input time offset was accepted");
    return okay;
}

bool TestHumanDurationFormatting() {
    bool okay = Check(
            forevertas::FormatHumanDurationMilliseconds(0.0) ==
                    "00:00:00",
            "zero duration formatting was incorrect");
    okay &= Check(
            forevertas::FormatHumanDurationMilliseconds(13500.0) ==
                    "00:00:13.5",
            "trailing duration zeroes were not trimmed");
    okay &= Check(
            forevertas::FormatHumanDurationMilliseconds(13.0) ==
                    "00:00:00.013",
            "millisecond duration formatting was incorrect");
    okay &= Check(
            forevertas::FormatHumanDurationMilliseconds(3723004.0) ==
                    "01:02:03.004",
            "hour duration formatting was incorrect");
    okay &= Check(
            forevertas::FormatHumanDurationNanoseconds(1234567890u) ==
                    "1.234567890",
            "sub-minute nanosecond formatting was incorrect");
    okay &= Check(
            forevertas::FormatHumanDurationNanoseconds(
                    62000000003u) ==
                    "1:02.000000003",
            "minute nanosecond formatting was incorrect");
    okay &= Check(
            forevertas::FormatHumanDurationNanoseconds(
                    3723000000004u) ==
                    "1:02:03.000000004",
            "hour nanosecond formatting was incorrect");
    okay &= Check(
            forevertas::FormatFixedDurationMilliseconds(0u) ==
                            "00:00:00.000" &&
                    forevertas::FormatFixedDurationMilliseconds(3723004u) ==
                            "01:02:03.004" &&
                    forevertas::FormatFixedSplitMilliseconds(1450u) ==
                            "1.450" &&
                    forevertas::FormatFixedSplitMilliseconds(7410u) ==
                            "7.410",
            "fixed millisecond formatting was incorrect");
    okay &= Check(
            forevertas::FormatSignificantDurationMilliseconds(0u) == "0" &&
                    forevertas::FormatSignificantDurationMilliseconds(
                            14500u) == "14.5" &&
                    forevertas::FormatSignificantDurationMilliseconds(
                            741000u) == "12:21" &&
                    forevertas::FormatSignificantDurationMilliseconds(
                            3723450u) == "1:02:03.45" &&
                    forevertas::FormatSignificantDurationMilliseconds(
                            5u) == "0.005" &&
                    forevertas::FormatSignificantDurationMilliseconds(
                            10u) == "0.01",
            "significant duration formatting was incorrect");
    return okay;
}

bool TestMutableSuffixNormalization() {
    const std::vector<SandboxInputEvent> baseline{
            Steering(90, 1234),
            Switch(70, SandboxInputAction::Accelerate, true),
            Steering(80, -4321),
            Steering(100, 1000),
            Steering(110, 2000)};
    std::vector<SandboxInputEvent> iteration{
            Steering(90, 65536),
            Switch(70, SandboxInputAction::Accelerate, true),
            Steering(80, -4321),
            Steering(100, 1000),
            Steering(110, 2000),
            Steering(50, 9999),
            Steering(115, 70000),
            Steering(110, 3000)};

    forevertas::NormalizeMutableInputEvents(
            iteration, baseline, 10u, 100);

    bool okay = Check(iteration.size() == 5u,
                      "mutable suffix normalization returned the wrong size");
    okay &= Check(forevertas::SameInputEvent(iteration[0], baseline[0]) &&
                          forevertas::SameInputEvent(iteration[1], baseline[1]) &&
                          forevertas::SameInputEvent(iteration[2], baseline[2]),
                  "immutable input prefix was reordered or rewritten");
    okay &= Check(iteration[3].timeMs == 100 &&
                          iteration[3].value.analog == 1000 &&
                          iteration[4].timeMs == 110 &&
                          iteration[4].value.analog == 3000,
                  "mutable input suffix was not normalized independently");
    return okay;
}

bool TestEvaluationTargets() {
    bool okay = true;

    {
        constexpr std::array<const char *, 3> windowTargetIds{
                forevertas::kVelocityEvaluationId,
                forevertas::kPointTargetEvaluationId,
                forevertas::kPoseTargetEvaluationId};
        for (const char *const id : windowTargetIds) {
            const auto *const registration =
                    forevertas::FindEvaluationTarget(id);
            OptionSettings settings = registration->defaultSettings;
            settings["minTimeMs"] = "0";
            settings["maxTimeMs"] = "20";
            std::unique_ptr<forevertas::IterationEvaluator> evaluator =
                    registration->create(settings, 10u);
            const forevertas::EvaluationPlan plan =
                    evaluator->Plan(10000, 0, 10u);
            okay &= Check(
                    plan.startTimeMs == 0 &&
                            plan.endTimeMs == 20,
                    "an evaluation time frame received the input one-tick "
                    "offset");
            settings["minTimeMs"] = "-10";
            okay &= Check(
                    registration->validateSettings(settings, 10u).has_value(),
                    "a negative evaluation time frame was accepted");
        }
    }

    {
        auto evaluator = Evaluator(forevertas::kVelocityEvaluationId);
        auto session = evaluator->CreateSession();
        PhysicsSandboxStateView state;
        state.timeMs = 1000u;
        state.car.linearSpeed = {3.0f, 4.0f, 12.0f};
        const auto sample = session->Observe(std::nullopt, state);
        okay &= Check(sample && std::abs(sample->score - 13.0) < 1e-9,
                      "velocity target returned the wrong total speed");
    }

    {
        OptionSettings settings =
                forevertas::FindEvaluationTarget(
                        forevertas::kVelocityEvaluationId)->defaultSettings;
        settings["mode"] = "projected";
        settings["directionX"] = "1";
        settings["directionY"] = "0";
        settings["directionZ"] = "0";
        settings["alignmentEnabled"] = "true";
        settings["minAlignmentPercent"] = "80";
        auto evaluator = Evaluator(
                forevertas::kVelocityEvaluationId, &settings);
        auto session = evaluator->CreateSession();
        PhysicsSandboxStateView state;
        state.timeMs = 1000u;
        state.car.linearSpeed = {10.0f, 1.0f, 0.0f};
        const auto accepted = session->Observe(std::nullopt, state);
        state.car.linearSpeed = {1.0f, 10.0f, 0.0f};
        const auto rejected = session->Observe(std::nullopt, state);
        okay &= Check(accepted && std::abs(accepted->score - 10.0) < 1e-9,
                      "projected velocity was incorrect");
        okay &= Check(!rejected,
                      "velocity alignment threshold did not reject a sample");
    }

    {
        OptionSettings settings =
                forevertas::FindEvaluationTarget(
                        forevertas::kPointTargetEvaluationId)->defaultSettings;
        settings["x"] = "5";
        auto evaluator = Evaluator(
                forevertas::kPointTargetEvaluationId, &settings);
        auto session = evaluator->CreateSession();
        PhysicsSandboxStateView state;
        state.timeMs = 1000u;
        state.car.position = {2.0f, 0.0f, 0.0f};
        const auto sample = session->Observe(std::nullopt, state);
        okay &= Check(sample && std::abs(sample->score - 3.0) < 1e-9,
                      "point target returned the wrong distance");
    }

    {
        auto evaluator = Evaluator(forevertas::kPoseTargetEvaluationId);
        auto session = evaluator->CreateSession();
        PhysicsSandboxStateView state;
        state.timeMs = 1000u;
        state.car.rotationW = 1.0f;
        const auto sample = session->Observe(std::nullopt, state);
        okay &= Check(sample && std::abs(sample->score) < 1e-9,
                      "matching pose did not produce zero error");
    }

    {
        OptionSettings settings =
                forevertas::FindEvaluationTarget(
                        forevertas::kVolumeEntryEvaluationId)->defaultSettings;
        settings["sizeX"] = "2";
        settings["sizeY"] = "2";
        settings["sizeZ"] = "2";
        auto evaluator = Evaluator(
                forevertas::kVolumeEntryEvaluationId, &settings);
        auto session = evaluator->CreateSession();
        PhysicsSandboxStateView previous;
        previous.timeMs = 100u;
        previous.car.position = {-10.0f, 0.0f, 0.0f};
        PhysicsSandboxStateView current = previous;
        current.timeMs = 110u;
        current.car.position = {0.0f, 0.0f, 0.0f};
        const auto sample = session->Observe(previous, current);
        okay &= Check(sample && std::abs(sample->timeMs - 109.0) < 1e-9,
                      "volume entry interpolation was incorrect");

        session = evaluator->CreateSession();
        previous.car.position = {-10.0f, 5.0f, 0.0f};
        current.car.position = {10.0f, 6.0f, 0.0f};
        okay &= Check(!session->Observe(previous, current),
                      "disjoint swept segment entered the volume");
    }

    {
        OptionSettings settings =
                forevertas::FindEvaluationTarget(
                        forevertas::kCustomVolumeEntryEvaluationId)
                        ->defaultSettings;
        auto evaluator = Evaluator(
                forevertas::kCustomVolumeEntryEvaluationId, &settings);
        auto session = evaluator->CreateSession();
        PhysicsSandboxStateView previous;
        previous.timeMs = 100u;
        previous.car.position = {-10.0f, 1.0f, 0.0f};
        PhysicsSandboxStateView current = previous;
        current.timeMs = 110u;
        current.car.position = {10.0f, 1.0f, 0.0f};
        const auto sample = session->Observe(previous, current);
        okay &= Check(
                sample && std::abs(sample->timeMs - 103.75) < 1e-9,
                "custom volume outside-to-outside interpolation was "
                "incorrect");
        settings["plane"] = "xy";
        settings["polygon"] = "-1,-1;1,-1;1,1;-1,1";
        settings["depth"] = "2";
        evaluator = Evaluator(
                forevertas::kCustomVolumeEntryEvaluationId, &settings);
        session = evaluator->CreateSession();
        previous.car.position = {0.0f, 0.0f, -1.0f};
        current.car.position = {0.0f, 0.0f, 3.0f};
        const auto xySample = session->Observe(previous, current);
        okay &= Check(
                xySample && std::abs(xySample->timeMs - 102.5) < 1e-9,
                "custom XY volume interpolation was incorrect");
        settings["plane"] = "yz";
        evaluator = Evaluator(
                forevertas::kCustomVolumeEntryEvaluationId, &settings);
        session = evaluator->CreateSession();
        previous.car.position = {-1.0f, 0.0f, 0.0f};
        current.car.position = {3.0f, 0.0f, 0.0f};
        const auto yzSample = session->Observe(previous, current);
        okay &= Check(
                yzSample && std::abs(yzSample->timeMs - 102.5) < 1e-9,
                "custom YZ volume interpolation was incorrect");
        settings["polygon"] = "0,0;4,4;0,4;4,0";
        const auto *const registration = forevertas::FindEvaluationTarget(
                forevertas::kCustomVolumeEntryEvaluationId);
        okay &= Check(
                registration->validateSettings(settings, 10u).has_value(),
                "self-intersecting custom polygon was accepted");
        settings["polygon"] = "0,0;10000001,0;0,1";
        okay &= Check(
                registration->validateSettings(settings, 10u).has_value(),
                "out-of-range custom polygon was accepted");
    }

    {
        auto evaluator = Evaluator(
                forevertas::kPreciseFinishTimeEvaluationId);
        auto session = evaluator->CreateSession();
        PhysicsSandboxStateView previous;
        previous.timeMs = 1230u;
        PhysicsSandboxStateView current = previous;
        current.timeMs = 1240u;
        current.raceCompleted = true;
        current.finishTimeMs = 1234u;
        current.finishTime =
                forevervalidator::FinishTimeEstimate{
                        1234567889u,
                        1234567890u,
                        1234567890u};
        const auto sample = session->Observe(previous, current);
        okay &= Check(
                sample && sample->score == 1234567890.0 &&
                        std::abs(sample->timeMs - 1234.56789) < 1e-9,
                "precise finish target ignored the inclusive upper bound");
        okay &= Check(sample &&
                              sample->description ==
                                      "Precise finish time: 1.234567890",
                      "precise finish target did not show nanoseconds");
        auto laterSession = evaluator->CreateSession();
        current.finishTime =
                forevervalidator::FinishTimeEstimate{
                        1234567890u,
                        1234567891u,
                        1234567891u};
        const auto laterSample =
                laterSession->Observe(previous, current);
        okay &= Check(
                sample && laterSample &&
                        evaluator->IsBetter(*sample, *laterSample),
                "precise finish target did not rank within-tick nanoseconds");
        auto tickOnlySession = evaluator->CreateSession();
        current.finishTime.reset();
        okay &= Check(
                !tickOnlySession->Observe(previous, current),
                "precise finish target fell back to tick time");
    }

    {
        OptionSettings settings =
                forevertas::FindEvaluationTarget(
                        forevertas::kStuntPointsEvaluationId)->defaultSettings;
        settings["targetTimeMs"] = "2500";
        auto evaluator = Evaluator(
                forevertas::kStuntPointsEvaluationId, &settings);
        const forevertas::EvaluationPlan plan =
                evaluator->Plan(10000, 1010, 10u);
        auto session = evaluator->CreateSession();
        PhysicsSandboxStateView previous;
        previous.timeMs = 2490u;
        previous.stuntsScore = 999u;
        PhysicsSandboxStateView current = previous;
        current.timeMs = 2500u;
        current.stuntsScore = 250u;
        const auto sample = session->Observe(previous, current);
        EvaluationSample incumbent{249.0, 2500.0, {}};
        okay &= Check(
                plan.startTimeMs == 2500 &&
                        plan.endTimeMs == 2500,
                "stunt target time received the input one-tick offset");
        okay &= Check(
                sample && sample->score == 250.0 &&
                        sample->timeMs == 2500.0 &&
                        sample->description.find("Stunt points: 250") == 0u,
                "stunt target did not use the deadline's monotonic score");
        okay &= Check(
                sample && evaluator->IsBetter(*sample, incumbent) &&
                        !evaluator->IsBetter(incumbent, *sample),
                "stunt target did not rank the highest score first");
        current.stuntsScore.reset();
        const auto noPoints = evaluator->CreateSession()->Observe(
                previous, current);
        okay &= Check(
                noPoints && noPoints->score == 0.0 &&
                        noPoints->description.find("Stunt points: 0") == 0u,
                "an absent stunt score was not treated as zero points");
        settings["targetTimeMs"] = "2501";
        const auto *const registration =
                forevertas::FindEvaluationTarget(
                        forevertas::kStuntPointsEvaluationId);
        okay &= Check(
                registration->validateSettings(settings, 10u).has_value(),
                "stunt target accepted a time between physics ticks");
    }

    return okay;
}

class AppendSteeringMutator final : public InputMutator {
public:
    explicit AppendSteeringMutator(AnalogInputState value)
        : value_(value) {}

    MutationResult Mutate(const MutationRequest &request) const override {
        std::vector<SandboxInputEvent> inputs = request.baselineInputs;
        inputs.push_back(Steering(1000, value_));
        return {inputs, 1u};
    }

    std::int64_t EarliestMutationTimeMs() const override { return 1000; }
    forevertas::MutationTimeRange AffectedTimeRange() const override {
        return {1000, 1000};
    }

private:
    AnalogInputState value_;
};

class NoOpMutator final : public InputMutator {
public:
    MutationResult Mutate(const MutationRequest &request) const override {
        return {request.baselineInputs, 0u};
    }
    std::int64_t EarliestMutationTimeMs() const override { return 1000; }
    forevertas::MutationTimeRange AffectedTimeRange() const override {
        return {1000, 1000};
    }
};

bool TestModifierComposition() {
    const std::vector<SandboxInputEvent> baseline{Steering(500, 6554)};
    std::vector<std::unique_ptr<InputMutator>> passes;
    passes.push_back(std::make_unique<AppendSteeringMutator>(13107));
    passes.push_back(std::make_unique<AppendSteeringMutator>(45875));
    forevertas::CompositeInputMutator composite(std::move(passes));
    const MutationResult result = composite.Mutate({baseline, 4u, 0u, 10u});
    bool okay = Check(result.mutationCount > 0u,
                      "composed modifiers reported no effective change");
    okay &= Check(result.inputs.size() == 2u,
                  "normalization did not merge same-tick steering events");
    okay &= Check(result.inputs.back().timeMs == 1000 &&
                          result.inputs.back().value.analog == 45875,
                  "normalization did not keep the last pass value");

    std::vector<std::unique_ptr<InputMutator>> noOpPasses;
    noOpPasses.push_back(std::make_unique<NoOpMutator>());
    forevertas::CompositeInputMutator noOp(std::move(noOpPasses));
    const MutationResult unchanged = noOp.Mutate({baseline, 0u, 0u, 10u});
    okay &= Check(unchanged.mutationCount == 0u &&
                          SameEvents(unchanged.inputs, baseline),
                  "no-op composition was not recognized");
    return okay;
}

bool TestModifierDeterminism() {
    const auto *const registration = forevertas::FindModifier(
            forevertas::kRandomSteeringModifierId);
    if (registration == nullptr) {
        return Check(false, "random steering modifier was not registered");
    }
    OptionSettings settings = registration->defaultSettings;
    settings["minTimeMs"] = "1000";
    settings["maxTimeMs"] = "2000";
    std::unique_ptr<InputMutator> modifier = registration->create(settings, 10u);
    const std::vector<SandboxInputEvent> baseline{
            Steering(1000, -6554),
            Steering(1010, -13107),
            Steering(1500, 19661),
            Steering(2010, 26214),
            Steering(2020, 32768)};
    const MutationResult first = modifier->Mutate({baseline, 7u, 0u, 10u});
    const MutationResult repeated = modifier->Mutate({baseline, 7u, 0u, 10u});
    const MutationResult otherIteration = modifier->Mutate(
            {baseline, 8u, 0u, 10u});
    bool okay = Check(modifier->EarliestMutationTimeMs() == 1010,
                      "an input modifier did not receive the one-tick offset");
    okay &= Check(SameEvents(first.inputs, repeated.inputs),
                      "same seed and iteration index were not deterministic");
    okay &= Check(!SameEvents(first.inputs, otherIteration.inputs),
                  "different iteration indices produced identical inputs");
    okay &= Check(AllAnalogInputsValid(first.inputs) &&
                          AllAnalogInputsValid(otherIteration.inputs),
                  "random steering produced an out-of-range input state");
    okay &= Check(forevertas::SameInputEvent(first.inputs.front(),
                                             baseline.front()) &&
                          forevertas::SameInputEvent(first.inputs.back(),
                                                     baseline.back()),
                  "modifier changed events outside its window");
    return okay;
}

bool TestModifierRandomGoldenSequence() {
    std::mt19937 unsignedRandom(5489u);
    std::array<std::uint32_t, 8u> unsignedValues{};
    for (std::uint32_t &value : unsignedValues) {
        value = forevertas::RandomInteger<std::uint32_t>(
                unsignedRandom, 0u, 9u);
    }
    bool okay = Check(
            unsignedValues ==
                    std::array<std::uint32_t, 8u>{8u, 1u, 9u, 8u,
                                                  1u, 9u, 9u, 2u},
            "modifier integer sampling changed its golden sequence");

    std::mt19937 signedRandom(5489u);
    std::array<std::int32_t, 8u> signedValues{};
    for (std::int32_t &value : signedValues) {
        value = forevertas::RandomInteger<std::int32_t>(
                signedRandom, -5, 5);
    }
    okay &= Check(
            signedValues ==
                    std::array<std::int32_t, 8u>{3, -4, 4, 4,
                                                 -4, 5, 5, -3},
            "signed modifier integer sampling changed its golden sequence");

    std::mt19937 shuffleRandom(5489u);
    std::array<std::uint32_t, 10u> shuffled{
            0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u};
    forevertas::ShuffleModifierValues(
            shuffled.begin(), shuffled.end(), shuffleRandom);
    okay &= Check(
            shuffled ==
                    std::array<std::uint32_t, 10u>{
                            2u, 9u, 0u, 5u, 4u,
                            6u, 7u, 1u, 3u, 8u},
            "modifier shuffle changed its golden sequence");
    return okay;
}

bool TestExistingEventWindowPatchParity() {
    const auto *const registration = forevertas::FindModifier(
            forevertas::kExistingEventPerturbationModifierId);
    if (registration == nullptr) {
        return Check(false,
                     "existing-event perturbation modifier was not registered");
    }
    OptionSettings settings = registration->defaultSettings;
    settings["minTimeMs"] = "100";
    settings["maxTimeMs"] = "1000";
    settings["minCount"] = "1";
    settings["maxCount"] = "6";
    settings["maxTimeShiftMs"] = "100";
    settings["steerDeltaMin"] = "-1";
    settings["steerDeltaMax"] = "1";
    std::unique_ptr<InputMutator> modifier =
            registration->create(settings, 10u);
    const std::vector<SandboxInputEvent> baseline{
            Steering(0, -30000),
            Switch(90, SandboxInputAction::Accelerate, true),
            Steering(100, -20000),
            Steering(250, -10000),
            Switch(400, SandboxInputAction::Brake, true),
            Steering(700, 10000),
            Switch(900, SandboxInputAction::Accelerate, false),
            Steering(1000, 20000),
            Steering(1100, 30000),
            Switch(1500, SandboxInputAction::Brake, false)};
    bool okay = true;
    for (std::uint64_t iteration = 0u; iteration < 128u; ++iteration) {
        const MutationResult legacy = modifier->Mutate(
                {baseline, iteration, 0u, 10u, 100, false});
        const MutationResult window = modifier->Mutate(
                {baseline, iteration, 0u, 10u, 100, true});
        if (!window.windowPatch) {
            return Check(false,
                         "existing-event fast path did not return a window patch");
        }
        const std::vector<SandboxInputEvent> materialized =
                forevertas::ApplyInputWindowPatch(
                        baseline, *window.windowPatch);
        okay &= Check(
                SameEvents(legacy.inputs, materialized),
                "window-local existing-event mutation changed semantics");
        okay &= Check(
                legacy.mutationCount == window.mutationCount,
                "window-local existing-event mutation count changed");
        if (!okay) return false;
    }
    return okay;
}

struct ModifierParitySpec {
    const char *id = nullptr;
    OptionSettings settings;
};

OptionSettings ModifierParitySettings(
        const char *id,
        std::int64_t minimumTimeMs,
        std::int64_t maximumTimeMs,
        std::uint32_t seed) {
    const auto *const registration = forevertas::FindModifier(id);
    if (registration == nullptr) {
        throw std::runtime_error("modifier registration is missing");
    }
    OptionSettings settings = registration->defaultSettings;
    settings["minTimeMs"] = std::to_string(minimumTimeMs);
    settings["maxTimeMs"] = std::to_string(maximumTimeMs);
    settings["seed"] = std::to_string(seed);
    if (std::string(id) == forevertas::kExistingEventPerturbationModifierId) {
        settings["minCount"] = "1";
        settings["maxCount"] = "6";
        settings["maxTimeShiftMs"] = "100";
        settings["steerDeltaMin"] = "-1";
        settings["steerDeltaMax"] = "1";
        settings["toggleAccelerate"] = "true";
        settings["toggleBrake"] = "true";
    } else if (std::string(id) == forevertas::kSmoothSteeringModifierId) {
        settings["deformationCount"] = "3";
        settings["radiusMs"] = "100";
        settings["amplitudeMin"] = "-0.5";
        settings["amplitudeMax"] = "0.5";
    } else if (std::string(id) == forevertas::kInputInsertionModifierId) {
        settings["steerEnabled"] = "true";
        settings["steerMode"] = "offset";
        settings["steerOffsetMin"] = "-0.5";
        settings["steerOffsetMax"] = "0.5";
        settings["steerMinCount"] = "1";
        settings["steerMaxCount"] = "2";
        settings["steerMaxHoldMs"] = "200";
        settings["accelerateEnabled"] = "true";
        settings["accelerateMinCount"] = "1";
        settings["accelerateMaxCount"] = "2";
        settings["accelerateMaxHoldMs"] = "200";
        settings["brakeEnabled"] = "true";
        settings["brakeMinCount"] = "1";
        settings["brakeMaxCount"] = "2";
        settings["brakeMaxHoldMs"] = "200";
    } else if (std::string(id) == forevertas::kInputDeletionModifierId) {
        settings["steerEnabled"] = "true";
        settings["steerMaxCount"] = "3";
        settings["accelerateEnabled"] = "true";
        settings["accelerateMaxCount"] = "2";
        settings["brakeEnabled"] = "true";
        settings["brakeMaxCount"] = "2";
    }
    return settings;
}

std::unique_ptr<forevertas::CompositeInputMutator> BuildParityComposite(
        const std::vector<ModifierParitySpec> &specs) {
    std::vector<std::unique_ptr<InputMutator>> passes;
    passes.reserve(specs.size());
    for (const ModifierParitySpec &spec : specs) {
        const auto *const registration = forevertas::FindModifier(spec.id);
        if (registration == nullptr) {
            throw std::runtime_error("modifier registration is missing");
        }
        if (const auto error = registration->validateSimulationSettings(
                    spec.settings, 10u)) {
            throw std::runtime_error(*error);
        }
        passes.push_back(registration->createFromSimulationSettings(
                spec.settings, 10u));
    }
    return std::make_unique<forevertas::CompositeInputMutator>(
            std::move(passes));
}

std::vector<SandboxInputEvent> ModifierParityBaseline() {
    std::vector<SandboxInputEvent> baseline{
            Steering(0, -32000),
            Switch(10, SandboxInputAction::Accelerate, true),
            Switch(20, SandboxInputAction::Brake, false),
            Steering(100, -28000),
            Switch(120, SandboxInputAction::Accelerate, false),
            Switch(140, SandboxInputAction::Brake, true),
            Steering(200, -22000),
            Steering(300, -16000),
            Switch(320, SandboxInputAction::Accelerate, true),
            Steering(400, -8000),
            Switch(420, SandboxInputAction::Brake, false),
            Steering(500, 0),
            Steering(600, 8000),
            Switch(620, SandboxInputAction::Accelerate, false),
            Steering(700, 16000),
            Switch(720, SandboxInputAction::Brake, true),
            Steering(800, 22000),
            Steering(900, 28000),
            Switch(920, SandboxInputAction::Accelerate, true),
            Steering(1000, 32000),
            Switch(1020, SandboxInputAction::Brake, false),
            Steering(1100, 24000),
            Steering(1200, 12000),
            Switch(1220, SandboxInputAction::Accelerate, false),
            Steering(1300, 0),
            Switch(1320, SandboxInputAction::Brake, true),
            Steering(1400, -12000),
            Steering(1500, -24000),
            Switch(1520, SandboxInputAction::Accelerate, true),
            Steering(1600, -32000),
            Switch(1620, SandboxInputAction::Brake, false),
            Steering(1700, 0),
            Switch(1720, SandboxInputAction::Accelerate, false),
            Steering(2000, 1000)};
    forevertas::NormalizeInputEvents(baseline, 10u);
    return baseline;
}

bool CheckCompositeWindowParity(
        const std::vector<ModifierParitySpec> &specs,
        std::uint64_t iterationCount,
        const char *diagnostic) {
    const std::vector<SandboxInputEvent> baseline =
            ModifierParityBaseline();
    std::unique_ptr<forevertas::CompositeInputMutator> legacy =
            BuildParityComposite(specs);
    std::unique_ptr<forevertas::CompositeInputMutator> optimized =
            BuildParityComposite(specs);
    const std::int64_t mutableFromTimeMs =
            legacy->EarliestMutationTimeMs();
    for (std::uint64_t iteration = 0u;
         iteration < iterationCount; ++iteration) {
        const MutationResult full = legacy->Mutate(
                {baseline,
                 iteration,
                 0u,
                 10u,
                 mutableFromTimeMs,
                 false,
                 0u});
        const MutationResult window = optimized->Mutate(
                {baseline,
                 iteration,
                 0u,
                 10u,
                 mutableFromTimeMs,
                 true,
                 0u});
        if (!window.windowPatch) {
            std::cerr << diagnostic << " did not return a window patch\n";
            return false;
        }
        const std::vector<SandboxInputEvent> materialized =
                forevertas::ApplyInputWindowPatch(
                        baseline, *window.windowPatch);
        if (!SameEvents(full.inputs, materialized) ||
            full.mutationCount != window.mutationCount ||
            full.mutationCount != forevertas::EffectiveInputChangeCount(
                    baseline, *window.windowPatch)) {
            std::cerr << diagnostic << " diverged at iteration "
                      << iteration << '\n';
            return false;
        }
    }
    return true;
}

bool TestAllModifierWindowPatchParity() {
    constexpr const char *ids[] = {
            forevertas::kRandomSteeringModifierId,
            forevertas::kExistingEventPerturbationModifierId,
            forevertas::kSmoothSteeringModifierId,
            forevertas::kInputInsertionModifierId,
            forevertas::kInputDeletionModifierId};
    for (std::uint32_t index = 0u; index < std::size(ids); ++index) {
        const std::vector<ModifierParitySpec> specs{{
                ids[index],
                ModifierParitySettings(
                        ids[index], 100, 1000, 1179926867u + index)}};
        if (!CheckCompositeWindowParity(
                    specs, 128u, "single-pass modifier window path")) {
            return false;
        }
    }
    return true;
}

bool TestMultiPassModifierWindowPatchParity() {
    const std::vector<ModifierParitySpec> allPasses{
            {forevertas::kRandomSteeringModifierId,
             ModifierParitySettings(
                     forevertas::kRandomSteeringModifierId,
                     100, 700, 1179926867u)},
            {forevertas::kInputInsertionModifierId,
             ModifierParitySettings(
                     forevertas::kInputInsertionModifierId,
                     300, 1200, 1179926868u)},
            {forevertas::kExistingEventPerturbationModifierId,
             ModifierParitySettings(
                     forevertas::kExistingEventPerturbationModifierId,
                     500, 1400, 1179926869u)},
            {forevertas::kSmoothSteeringModifierId,
             ModifierParitySettings(
                     forevertas::kSmoothSteeringModifierId,
                     200, 1500, 1179926870u)},
            {forevertas::kInputDeletionModifierId,
             ModifierParitySettings(
                     forevertas::kInputDeletionModifierId,
                     700, 1600, 1179926871u)},
            {forevertas::kInputInsertionModifierId,
             ModifierParitySettings(
                     forevertas::kInputInsertionModifierId,
                     100, 400, 1179926872u)},
            {forevertas::kExistingEventPerturbationModifierId,
             ModifierParitySettings(
                     forevertas::kExistingEventPerturbationModifierId,
                     1200, 1600, 1179926873u)}};
    if (!CheckCompositeWindowParity(
                allPasses, 128u, "seven-pass modifier window path")) {
        return false;
    }

    std::vector<ModifierParitySpec> reversed = allPasses;
    std::reverse(reversed.begin(), reversed.end());
    return CheckCompositeWindowParity(
            reversed, 128u, "reversed seven-pass modifier window path");
}

bool TestEveryOrderedModifierPairWindowPatchParity() {
    constexpr const char *ids[] = {
            forevertas::kRandomSteeringModifierId,
            forevertas::kExistingEventPerturbationModifierId,
            forevertas::kSmoothSteeringModifierId,
            forevertas::kInputInsertionModifierId,
            forevertas::kInputDeletionModifierId};
    for (std::uint32_t first = 0u; first < std::size(ids); ++first) {
        for (std::uint32_t second = 0u; second < std::size(ids); ++second) {
            const std::vector<ModifierParitySpec> specs{
                    {ids[first],
                     ModifierParitySettings(
                             ids[first],
                             100,
                             1100,
                             1179926900u + first * 5u + second)},
                    {ids[second],
                     ModifierParitySettings(
                             ids[second],
                             500,
                             1600,
                             1179927000u + first * 5u + second)}};
            if (!CheckCompositeWindowParity(
                        specs,
                        32u,
                        "ordered two-pass modifier window path")) {
                return false;
            }
        }
    }
    return true;
}

bool TestNonCanonicalModifierWindowFallback() {
    std::vector<SandboxInputEvent> baseline = ModifierParityBaseline();
    baseline[4].timeMs += 1;
    const std::vector<ModifierParitySpec> specs{{
            forevertas::kSmoothSteeringModifierId,
            ModifierParitySettings(
                    forevertas::kSmoothSteeringModifierId,
                    100, 1000, 1179926867u)}};
    std::unique_ptr<forevertas::CompositeInputMutator> legacy =
            BuildParityComposite(specs);
    std::unique_ptr<forevertas::CompositeInputMutator> optimized =
            BuildParityComposite(specs);
    const MutationResult full = legacy->Mutate(
            {baseline, 3u, 0u, 10u, 100, false, 0u});
    const MutationResult fallback = optimized->Mutate(
            {baseline, 3u, 0u, 10u, 100, true, 0u});
    return Check(
            !fallback.windowPatch &&
                    SameEvents(full.inputs, fallback.inputs) &&
                    full.mutationCount == fallback.mutationCount,
            "non-canonical input did not use the full-stream fallback");
}

bool TestModifierWindowBaselineGeneration() {
    const std::vector<ModifierParitySpec> specs{
            {forevertas::kExistingEventPerturbationModifierId,
             ModifierParitySettings(
                     forevertas::kExistingEventPerturbationModifierId,
                     100, 1200, 1179927100u)},
            {forevertas::kInputInsertionModifierId,
             ModifierParitySettings(
                     forevertas::kInputInsertionModifierId,
                     300, 1500, 1179927101u)},
            {forevertas::kSmoothSteeringModifierId,
             ModifierParitySettings(
                     forevertas::kSmoothSteeringModifierId,
                     200, 1400, 1179927102u)}};
    std::vector<SandboxInputEvent> baseline = ModifierParityBaseline();
    std::unique_ptr<forevertas::CompositeInputMutator> optimized =
            BuildParityComposite(specs);
    const std::int64_t mutableFromTimeMs =
            optimized->EarliestMutationTimeMs();
    static_cast<void>(optimized->Mutate(
            {baseline,
             9u,
             0u,
             10u,
             mutableFromTimeMs,
             true,
             0u}));

    const auto changed = std::find_if(
            baseline.begin(), baseline.end(),
            [](const SandboxInputEvent &event) {
                return event.timeMs == 600 &&
                        event.action == SandboxInputAction::Steer;
            });
    if (changed == baseline.end()) {
        return Check(false, "baseline generation test input is missing");
    }
    changed->value.analog = -12345;

    std::unique_ptr<forevertas::CompositeInputMutator> legacy =
            BuildParityComposite(specs);
    const MutationResult expected = legacy->Mutate(
            {baseline,
             9u,
             0u,
             10u,
             mutableFromTimeMs,
             false,
             1u});
    const MutationResult actual = optimized->Mutate(
            {baseline,
             9u,
             0u,
             10u,
             mutableFromTimeMs,
             true,
             1u});
    if (!actual.windowPatch) {
        return Check(false,
                     "changed baseline did not retain the window path");
    }
    return Check(
            SameEvents(
                    expected.inputs,
                    forevertas::ApplyInputWindowPatch(
                            baseline, *actual.windowPatch)) &&
                    expected.mutationCount == actual.mutationCount,
            "window baseline cache ignored its generation change");
}

bool TestInputScriptFormatting() {
    NumericLocaleGuard locale;
    if (!locale.ActivateCommaDecimalLocale()) {
        return Check(false, "no comma-decimal locale is installed for testing");
    }
    const std::vector<SandboxInputEvent> inputs{
            Switch(1350, SandboxInputAction::SteerLeft, true),
            Analog(130, SandboxInputAction::Gas, -16384),
            Switch(110, SandboxInputAction::Accelerate, true),
            Steering(125, 32768),
            Steering(140, -16384),
            Steering(150, forevertas::kAnalogInputMaximum),
            Switch(1340, SandboxInputAction::Brake, false),
            Switch(1360, SandboxInputAction::SteerRight, false),
            Switch(1370, SandboxInputAction::Respawn, true),
            Switch(1380, SandboxInputAction::Respawn, false),
            Switch(90, SandboxInputAction::Brake, true),
            Switch(100, SandboxInputAction::RaceRunning, true),
            Switch(1390, SandboxInputAction::FinishLine, true),
            Switch(1400, SandboxInputAction::Accelerate, false)};
    const std::string formatted =
            forevertas::FormatInputScript(inputs);
    return Check(
            formatted ==
                    "0.00 press up\n"
                    "0.02 steer 32768\n"
                    "0.02 gas -16384\n"
                    "0.03 steer -16384\n"
                    "0.04 steer 65536\n"
                    "1.23 rel down\n"
                    "1.24 press left\n"
                    "1.25 rel right\n"
                    "1.26 press enter",
            "input script formatting was incorrect or locale-sensitive");
}

bool TestInputScriptParsingAndBaseline() {
    const forevertas::InputScriptParseResult parsed =
            forevertas::ParseInputScript(
                    "# Base controls\n"
                    "1.00 PRESS up\n"
                    "0.00 steer 32768\n"
                    "0.00 STEER -16384 // last command wins\n"
                    "0.20 release down\n"
                    "0.30 gas -65536\n");
    bool okay = Check(
            parsed && parsed.commands.size() == 5u,
            "valid input script was not parsed");
    if (!parsed) return false;

    const std::vector<SandboxInputEvent> fixedInputs{
            Switch(0, SandboxInputAction::RaceRunning, true)};
    const forevertas::InputScriptBaselineResult baseline =
            forevertas::BuildInputScriptBaseline(
                    fixedInputs, parsed.commands, 10u);
    okay &= Check(
            baseline && baseline.events.size() == 5u,
            "input script baseline was not materialized");
    if (baseline) {
        okay &= Check(
                baseline.events[0].timeMs == 0 &&
                        baseline.events[0].action ==
                                SandboxInputAction::RaceRunning &&
                        baseline.events[1].timeMs == 10 &&
                        baseline.events[1].action ==
                                SandboxInputAction::Steer &&
                        baseline.events[1].value.analog == -16384 &&
                        baseline.events[2].timeMs == 210 &&
                        baseline.events[3].timeMs == 310 &&
                        baseline.events[4].timeMs == 1010,
                "script controls did not populate the canonical timeline");
    }

    const forevertas::InputScriptParseResult empty =
            forevertas::ParseInputScript(" \n# no controls\n// still empty");
    okay &= Check(empty && empty.commands.empty(),
                  "empty input script was not accepted");

    const auto expectError = [&okay](std::string_view script,
                                     std::string_view fragment) {
        const forevertas::InputScriptParseResult result =
                forevertas::ParseInputScript(script);
        okay &= Check(
                !result && result.error &&
                        result.error->find(fragment) != std::string::npos,
                "invalid input script did not report the expected error");
    };
    expectError("0.001 press up", "10 ms-aligned");
    expectError("-0.10 press up", "non-negative");
    expectError("0,10 press up", "10 ms-aligned");
    expectError("0.10.0 press up", "10 ms-aligned");
    expectError("0.00 steer 65537", "[-65536, 65536]");
    expectError("0.00 gas -65537", "[-65536, 65536]");
    expectError("0.00 gas 999999999999999999999", "[-65536, 65536]");
    expectError("0.00 launch up", "command must be");
    expectError("0.00 press space", "switch must be");
    expectError("0.00 press up trailing", "expected");
    expectError("\n0.00 rel enter", "Line 2");
    expectError("9223372036854776.00 press up", "10 ms-aligned");

    const forevertas::InputScriptParseResult tooLate =
            forevertas::ParseInputScript("2.00 press up");
    const forevertas::InputScriptBaselineResult lateInput =
            forevertas::BuildInputScriptBaseline(
                    fixedInputs, tooLate.commands, 10u);
    okay &= Check(
            lateInput && std::any_of(
                    lateInput.events.begin(),
                    lateInput.events.end(),
                    [](const SandboxInputEvent &event) {
                        return event.timeMs == 2010 &&
                                event.action ==
                                SandboxInputAction::Accelerate;
                    }),
            "late canonical input was rejected");

    std::string currentConfigurationScript;
    for (std::size_t line = 1u; line < 300u; ++line) {
        currentConfigurationScript += "# retained input\n";
    }
    currentConfigurationScript += "4.69 steer 65528";
    const forevertas::InputScriptParseResult currentConfiguration =
            forevertas::ParseInputScript(currentConfigurationScript);
    const forevertas::InputScriptBaselineResult currentAccepted =
            forevertas::BuildInputScriptBaseline(
                    fixedInputs,
                    currentConfiguration.commands,
                    10u);
    okay &= Check(
            currentAccepted && currentAccepted.events.size() == 2u &&
                    currentAccepted.events.back().timeMs == 4700 &&
                    currentAccepted.events.back().action ==
                            SandboxInputAction::Steer,
            "current persisted input was rejected");

    const std::string formatted = forevertas::FormatInputScript(fixedInputs);
    const forevertas::InputScriptParseResult roundTrip =
            forevertas::ParseInputScript(formatted);
    const forevertas::InputScriptBaselineResult rebuilt =
            forevertas::BuildInputScriptBaseline(
                    fixedInputs, roundTrip.commands, 10u);
    okay &= Check(
            roundTrip && rebuilt &&
                    forevertas::FormatInputScript(rebuilt.events) == formatted,
            "formatted input script did not round-trip");
    return okay;
}

bool TestAnalogInputRepresentation() {
    const auto half = forevertas::ParseNormalizedAnalogInput("0.5");
    const auto quarterLeft =
            forevertas::ParseNormalizedAnalogInput("-0.25");
    bool okay = Check(half && *half == 32768 &&
                              quarterLeft && *quarterLeft == -16384,
                      "normalized settings were not quantized exactly");
    okay &= Check(!forevertas::ParseNormalizedAnalogInput("1.0001") &&
                          !forevertas::ParseNormalizedAnalogInput("-1.0001"),
                  "out-of-range normalized analog settings were accepted");
    okay &= Check(forevertas::SaturateAnalogInputState(70000) == 65536 &&
                          forevertas::SaturateAnalogInputState(-70000) ==
                                  -65536,
                  "integer analog saturation was incorrect");

    std::vector<SandboxInputEvent> events{
            Steering(100, 70000), Steering(110, -70000)};
    forevertas::NormalizeInputEvents(events, 10u);
    okay &= Check(events.size() == 2u &&
                          events[0].value.analog == 65536 &&
                          events[1].value.analog == -65536 &&
                          AllAnalogInputsValid(events),
                  "input normalization did not enforce integer bounds");
    return okay;
}

bool TestKeyboardSteeringConversion() {
    std::vector<SandboxInputEvent> events{
            Switch(0, SandboxInputAction::SteerLeft, true),
            Switch(0, SandboxInputAction::SteerRight, true),
            Switch(0, SandboxInputAction::Accelerate, true),
            Switch(10, SandboxInputAction::SteerLeft, false),
            Steering(20, -32768),
            Switch(20, SandboxInputAction::SteerRight, true),
            Switch(30, SandboxInputAction::SteerLeft, true),
            Switch(40, SandboxInputAction::SteerLeft, false),
            Switch(50, SandboxInputAction::SteerRight, false),
            Steering(50, 500),
            Switch(60, SandboxInputAction::SteerRight, false),
            Steering(60, 656),
            Switch(70, SandboxInputAction::SteerLeft, true),
            Switch(70, SandboxInputAction::SteerRight, true),
            Switch(80, SandboxInputAction::SteerLeft, false),
            Switch(90, SandboxInputAction::SteerRight, false)};
    forevertas::ConvertKeyboardSteeringToAnalog(events);

    const std::vector<SandboxInputEvent> expected{
            Switch(0, SandboxInputAction::Accelerate, true),
            Steering(0, -65536),
            Steering(10, 65536),
            Steering(20, 65536),
            Steering(30, -65536),
            Steering(40, 65536),
            Steering(50, 0),
            Steering(60, 656),
            Steering(70, -65536),
            Steering(80, 65536),
            Steering(90, 0)};
    bool okay = Check(
            SameEvents(events, expected),
            "keyboard steering did not follow engine priority semantics");
    okay &= Check(
            std::none_of(
                    events.begin(),
                    events.end(),
                    [](const SandboxInputEvent &event) {
                        return event.action ==
                                       SandboxInputAction::SteerLeft ||
                                event.action ==
                                       SandboxInputAction::SteerRight;
                    }),
            "keyboard steering actions remained after analog conversion");

    std::vector<SandboxInputEvent> unsorted{
            Switch(30, SandboxInputAction::SteerRight, false),
            Switch(10, SandboxInputAction::SteerRight, true),
            Steering(20, -16384)};
    forevertas::ConvertKeyboardSteeringToAnalog(unsorted);
    okay &= Check(
            SameEvents(
                    unsorted,
                    {Steering(10, 65536),
                     Steering(20, -16384),
                     Steering(30, 0)}),
            "unsorted mixed steering inputs were not converted chronologically");

    std::vector<SandboxInputEvent> invalid{
            Steering(10, 70000),
            Switch(20, SandboxInputAction::SteerLeft, true)};
    forevertas::ConvertKeyboardSteeringToAnalog(invalid);
    okay &= Check(
            invalid.size() == 2u &&
                    invalid[0].action == SandboxInputAction::Steer &&
                    invalid[0].value.analog == 70000 &&
                    invalid[1].action == SandboxInputAction::Steer &&
                    invalid[1].value.analog == -65536,
            "keyboard conversion concealed an invalid analog input");
    return okay;
}

bool TestAllModifierAnalogInvariants() {
    const std::vector<SandboxInputEvent> baseline{
            Steering(1000, -32768),
            Switch(1200, SandboxInputAction::Accelerate, true),
            Steering(2000, 0),
            Switch(2500, SandboxInputAction::Brake, true),
            Steering(4000, 32768),
            Switch(5000, SandboxInputAction::Brake, false)};
    for (const auto &registration : forevertas::ModifierRegistry()) {
        std::unique_ptr<InputMutator> modifier = registration.create(
                registration.defaultSettings, 10u);
        for (std::uint64_t iteration = 0u; iteration < 32u; ++iteration) {
            const MutationResult result = modifier->Mutate(
                    {baseline, iteration, 0u, 10u});
            if (!AllAnalogInputsValid(result.inputs)) {
                return Check(false,
                             "modifier produced an out-of-range analog state");
            }
        }
    }
    return true;
}

bool TestRegistries() {
    bool okay = true;
    for (const auto &registration : forevertas::SearchAlgorithmRegistry()) {
        okay &= Check(!registration.settingsComponent.empty(),
                      "search option is missing its QML component");
        okay &= Check(!registration.validateSettings(
                              registration.defaultSettings, 10u),
                      "search option defaults are invalid");
        okay &= Check(registration.create(
                              registration.defaultSettings, 10u) != nullptr,
                      "search option factory returned null");
    }
    for (const auto &registration : forevertas::ModifierRegistry()) {
        okay &= Check(!registration.settingsComponent.empty(),
                      "modifier is missing its QML component");
        okay &= Check(!registration.validateSettings(
                              registration.defaultSettings, 10u),
                      "modifier defaults are invalid");
        okay &= Check(registration.create(
                              registration.defaultSettings, 10u) != nullptr,
                      "modifier factory returned null");
    }
    for (const auto &registration : forevertas::EvaluationTargetRegistry()) {
        okay &= Check(!registration.settingsComponent.empty(),
                      "evaluation target is missing its QML component");
        okay &= Check(!registration.validateSettings(
                              registration.defaultSettings, 10u),
                      "evaluation target defaults are invalid");
        okay &= Check(registration.create(
                              registration.defaultSettings, 10u) != nullptr,
                      "evaluation target factory returned null");
    }
    okay &= Check(forevertas::ModifierRegistry().size() == 5u,
                  "not all required modifiers are registered");
    okay &= Check(forevertas::EvaluationTargetRegistry().size() == 7u,
                  "not all required evaluation targets are registered");
    return okay;
}

bool TestLocaleIndependentFloatingPointSettings() {
    NumericLocaleGuard locale;
    if (!locale.ActivateCommaDecimalLocale()) {
        return Check(false, "no comma-decimal locale is installed for testing");
    }

    const auto parsed = forevertas::ParseFiniteDouble("-12.5e-1");
    bool okay = Check(parsed && std::abs(*parsed + 1.25) < 1e-12,
                      "dot decimal parsing followed LC_NUMERIC");
    const forevertas::InputScriptParseResult parsedScript =
            forevertas::ParseInputScript("12.50 steer -32768");
    okay &= Check(
            parsedScript && parsedScript.commands.size() == 1u &&
                    parsedScript.commands.front().userTimeMs == 12500,
            "input script parsing followed LC_NUMERIC");
    okay &= Check(!forevertas::ParseFiniteDouble("12,5"),
                  "comma decimal input was accepted");
    okay &= Check(!forevertas::ParseFiniteDouble("12.5x"),
                  "floating setting accepted trailing characters");

    const auto analogQuarter =
            forevertas::ParseNormalizedAnalogInput("0.25");
    okay &= Check(analogQuarter && *analogQuarter == 16384,
                  "analog setting quantization followed LC_NUMERIC");
    okay &= Check(!forevertas::ParseNormalizedAnalogInput("0,25"),
                  "comma-decimal analog setting was accepted");

    okay &= Check(
            ModifierAcceptsDotDecimals(
                    forevertas::kExistingEventPerturbationModifierId,
                    {{"steerDeltaMin", "-0.25"},
                     {"steerDeltaMax", "0.25"},
                     {"steerAbsoluteMin", "-0.75"},
                     {"steerAbsoluteMax", "0.75"}}),
            "existing-event decimal settings followed LC_NUMERIC");
    okay &= Check(
            ModifierAcceptsDotDecimals(
                    forevertas::kSmoothSteeringModifierId,
                    {{"amplitudeMin", "-0.25"},
                     {"amplitudeMax", "0.25"}}),
            "smooth-steering decimal settings followed LC_NUMERIC");
    okay &= Check(
            ModifierAcceptsDotDecimals(
                    forevertas::kInputInsertionModifierId,
                    {{"steerAbsoluteMin", "-0.75"},
                     {"steerAbsoluteMax", "0.75"},
                     {"steerOffsetMin", "-0.25"},
                     {"steerOffsetMax", "0.25"}}),
            "input-insertion decimal settings followed LC_NUMERIC");

    okay &= Check(
            EvaluatorAcceptsDotDecimals(
                    forevertas::kVelocityEvaluationId,
                    {{"mode", "projected"},
                     {"alignmentEnabled", "true"},
                     {"directionX", "0.5"},
                     {"directionY", "0.25"},
                     {"directionZ", "0.75"},
                     {"minAlignmentPercent", "12.5"}}),
            "velocity decimal settings followed LC_NUMERIC");
    okay &= Check(
            EvaluatorAcceptsDotDecimals(
                    forevertas::kPointTargetEvaluationId,
                    {{"x", "12.5"}, {"y", "-3.25"}, {"z", "0.75"}}),
            "point-target decimal settings followed LC_NUMERIC");
    okay &= Check(
            EvaluatorAcceptsDotDecimals(
                    forevertas::kPoseTargetEvaluationId,
                    {{"x", "12.5"},
                     {"yawDegrees", "22.5"},
                     {"pitchDegrees", "-7.25"},
                     {"rotationWeightPercent", "37.5"}}),
            "pose-target decimal settings followed LC_NUMERIC");
    okay &= Check(
            EvaluatorAcceptsDotDecimals(
                    forevertas::kVolumeEntryEvaluationId,
                    {{"centerX", "12.5"},
                     {"centerY", "-3.25"},
                     {"sizeX", "1.5"},
                     {"sizeY", "2.25"}}),
            "volume-entry decimal settings followed LC_NUMERIC");
    return okay;
}

bool TestStructuredSearchLogEscaping() {
    const std::string input =
            "schema=2 \\\"broken\\\"\nforged=value\r\t" +
            std::string(1u, '\x01');
    return Check(
            forevertas::detail::EscapeStructuredLogValue(input) ==
                    "schema=2 \\\\\\\"broken\\\\\\\"\\nforged=value\\r\\t\\x01",
            "structured search log value was not escaped safely");
}

bool TestSearchControl() {
    const OptionSettings defaults =
            forevertas::DefaultBasicBruteForceOptionSettings();
    bool okay = Check(
            defaults.at("autoPromoteBest") == "false" &&
                    !forevertas::ValidateBasicBruteForceOptionSettings(
                            defaults, 10u),
            "default Basic search settings were rejected");
    okay &= Check(
            forevertas::ValidateBasicBruteForceOptionSettings(
                    {{"unexpected", "1"}}, 10u)
                    .has_value(),
            "an unexpected Basic search setting was accepted");
    okay &= Check(
            forevertas::ValidateBasicBruteForceOptionSettings(
                    {{"autoPromoteBest", "yes"}}, 10u)
                    .has_value(),
            "an invalid auto-promote setting was accepted");
    okay &= Check(
            forevertas::ValidateBasicBruteForceOptionSettings(
                    defaults, 0u)
                    .has_value(),
            "zero tick duration was accepted");

    forevertas::SearchRunControl control;
    control.cancellationRequested = []() { return true; };
    try {
        static_cast<void>(forevertas::RunSearch(
                {"unused", "unused"}, &control));
        okay &= Check(false, "immediate hard abort was ignored");
    } catch (const forevertas::SearchCancelled &) {
    } catch (...) {
        okay &= Check(false, "immediate hard abort returned wrong failure");
    }
    return okay;
}

bool TestRollingThroughput() {
    using namespace std::chrono_literals;

    forevertas::app::RollingThroughput throughput;
    bool okay = Check(
            throughput.Observe(0u, 0s) == 0.0,
            "zero-duration throughput was not zero");
    okay &= Check(
            std::abs(throughput.Observe(50u, 5s) - 10.0) < 1e-9,
            "startup throughput average was incorrect");
    okay &= Check(
            std::abs(throughput.Observe(100u, 10s) - 10.0) < 1e-9,
            "ten-second throughput average was incorrect");
    okay &= Check(
            std::abs(throughput.Observe(250u, 15s) - 20.0) < 1e-9,
            "throughput included samples older than ten seconds");

    throughput.Reset();
    static_cast<void>(throughput.Observe(70u, 7s));
    static_cast<void>(throughput.Observe(190u, 13s));
    okay &= Check(
            std::abs(throughput.Observe(290u, 18s) - 20.0) < 1e-9,
            "throughput did not interpolate the ten-second boundary");

    throughput.Reset();
    static_cast<void>(throughput.Observe(100u, 5s));
    okay &= Check(
            throughput.Observe(100u, 15s) == 0.0,
            "idle throughput did not decay over the rolling window");
    return okay;
}

bool TestCudaBatchCalibrationStrategy() {
    const auto observe = [](
                                 forevertas::CudaBatchCalibrator *calibrator,
                                 double throughput) {
        const std::uint32_t batchSize =
                calibrator->CurrentBatchSize();
        const auto elapsed =
                std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(
                                static_cast<double>(batchSize) /
                                throughput));
        for (int sample = 0; sample < 7; ++sample) {
            calibrator->Observe(batchSize, elapsed);
        }
    };

    const auto realisticThroughput = [](std::uint32_t batchSize) {
        if (batchSize <= 7500u) {
            constexpr double saturationScale = 1000.0;
            constexpr double peakSize = 7500.0;
            const double size = static_cast<double>(batchSize);
            return 9100.0 *
                    (size / (size + saturationScale)) /
                    (peakSize / (peakSize + saturationScale));
        }
        const double distance = std::abs(
                static_cast<double>(batchSize) - 7500.0);
        return 9100.0 - distance * (950.0 / 5300.0);
    };

    const auto calibrate = [&observe](
                                   const auto &throughputForSize,
                                   std::uint32_t capacity) {
        forevertas::CudaBatchCalibrator calibrator;
        for (int measurement = 0;
             measurement < 128 && !calibrator.Complete();
             ++measurement) {
            const std::uint32_t batchSize =
                    calibrator.CurrentBatchSize();
            if (batchSize > capacity) {
                calibrator.CapacityUnavailable();
            } else {
                observe(
                        &calibrator,
                        throughputForSize(batchSize));
            }
        }
        return calibrator;
    };

    auto calibrator = calibrate(
            realisticThroughput,
            102400u);
    const std::uint32_t calibratedSize =
            calibrator.BestBatchSize();
    bool okay = Check(
            calibrator.Complete(),
            "CUDA calibration did not converge");
    okay &= Check(
            calibratedSize >= 7000u &&
                    calibratedSize <= 8000u,
            "CUDA calibration missed the measured peak");
    okay &= Check(
            realisticThroughput(calibratedSize) >
                    realisticThroughput(12800u),
            "CUDA calibration retained the slower coarse batch");
    okay &= Check(
            calibrator.CurrentBatchSize() == calibratedSize,
            "CUDA calibration did not restore its measured winner");

    forevertas::CudaBatchCalibrator capacityLimited;
    for (std::uint32_t batchSize = 1u;
         batchSize <= 4096u;
         batchSize *= 2u) {
        observe(
                &capacityLimited,
                static_cast<double>(batchSize));
    }
    okay &= Check(
            capacityLimited.CurrentBatchSize() == 8192u,
            "CUDA calibration did not grow from the universal minimum");
    capacityLimited.CapacityUnavailable();
    okay &= Check(
            capacityLimited.CurrentBatchSize() > 4096u &&
                    capacityLimited.CurrentBatchSize() < 8192u,
            "CUDA calibration discarded the allocation boundary");
    for (int measurement = 0;
         measurement < 64 && !capacityLimited.Complete();
         ++measurement) {
        observe(
                &capacityLimited,
                static_cast<double>(
                        capacityLimited.CurrentBatchSize()));
    }
    okay &= Check(
            capacityLimited.Complete() &&
                    capacityLimited.BestBatchSize() > 4096u &&
                    capacityLimited.BestBatchSize() < 8192u,
            "CUDA calibration did not approach its measured capacity");

    const auto slowlyIncreasing = [](std::uint32_t batchSize) {
        return 1000.0 + std::log2(
                                static_cast<double>(batchSize));
    };
    const auto boundaryMaximizer =
            calibrate(slowlyIncreasing, 128u);
    okay &= Check(
            boundaryMaximizer.Complete() &&
                    boundaryMaximizer.BestBatchSize() > 120u,
            "CUDA calibration stopped on a throughput plateau instead "
            "of maximizing the measured safe range");

    forevertas::CudaBatchCalibrator isolatedFastPass;
    const auto observeSamples = [](
                                        forevertas::CudaBatchCalibrator
                                                *candidate,
                                        const std::vector<double>
                                                &throughputs) {
        const std::uint32_t size =
                candidate->CurrentBatchSize();
        for (double throughput : throughputs) {
            const auto elapsed =
                    std::chrono::duration_cast<
                            std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(
                                    static_cast<double>(size) /
                                    throughput));
            candidate->Observe(size, elapsed);
        }
    };
    observeSamples(
            &isolatedFastPass,
            {100.0, 100.0,
             100.0, 100.0, 100.0, 100.0, 100.0});
    observeSamples(
            &isolatedFastPass,
            {100.0, 100.0,
             100.0, 100.0, 100.0, 100.0, 1000.0});
    okay &= Check(
            isolatedFastPass.BestBatchSize() == 1u,
            "CUDA calibration selected an isolated fast pass");

    forevertas::CudaBatchCalibrator unstable;
    observeSamples(
            &unstable,
            {100.0, 100.0,
             100.0, 1000.0, 100.0, 1000.0, 100.0,
             1000.0, 100.0, 1000.0, 100.0});
    okay &= Check(
            unstable.Complete() &&
                    !unstable.HasReliableMeasurement(),
            "CUDA calibration accepted irrepeatable throughput");
    return okay;
}

bool TestCudaCalibrationSafety() {
    using forevertas::CudaCalibrationBatchProfile;
    using forevertas::CudaCalibrationDeviceLimits;
    using forevertas::CudaCalibrationSafetyPlanner;

    constexpr std::uint64_t gib = 1024ull * 1024ull * 1024ull;
    CudaCalibrationDeviceLimits limits;
    limits.totalMemoryBytes = 8u * gib;
    limits.freeMemoryBytes = 6u * gib;
    limits.maximumThreadsPerBlock = 1024u;
    limits.maximumGridDimensionX = 1000000u;
    limits.registersPerBlock = 65536u;
    limits.registersPerMultiprocessor = 65536u;
    limits.maximumThreadsPerMultiprocessor = 2048u;
    limits.maximumBlocksPerMultiprocessor = 32u;
    limits.multiprocessorCount = 32u;

    CudaCalibrationBatchProfile first;
    first.batchSize = 1u;
    first.batchCapacity = 1u;
    first.residentDeviceBytes = 100u * 1024u * 1024u;
    first.kernelMilliseconds = 1.0;
    first.simulationThreadsPerBlock = 256u;
    first.simulationRegistersPerThread = 32u;
    first.simulationActiveBlocksPerMultiprocessor = 8u;
    first.simulationTheoreticalOccupancy = 1.0;

    CudaCalibrationSafetyPlanner planner;
    bool okay = Check(
            !planner.Evaluate(1u, 1u, limits).safe,
            "CUDA calibration accepted an unprofiled device");
    planner.Observe(first);
    okay &= Check(
            planner.Evaluate(2u, 1u, limits).safe,
            "CUDA calibration rejected a safe measured growth step");

    CudaCalibrationBatchProfile second = first;
    second.batchSize = 2u;
    second.batchCapacity = 2u;
    second.residentDeviceBytes = 110u * 1024u * 1024u;
    second.kernelMilliseconds = 2.0;
    planner.Observe(second);

    CudaCalibrationDeviceLimits memoryLimited = limits;
    memoryLimited.freeMemoryBytes = 2u * gib;
    const auto memoryDecision =
            planner.Evaluate(100u, 2u, memoryLimited);
    okay &= Check(
            !memoryDecision.safe &&
                    memoryDecision.requiredTransientBytes > 0u &&
                    memoryDecision.reservedMemoryHeadroomBytes >=
                            512u * 1024u * 1024u,
            "CUDA calibration crossed the reserved memory headroom");

    CudaCalibrationDeviceLimits launchLimited = limits;
    launchLimited.maximumGridDimensionX = 10u;
    okay &= Check(
            !planner.Evaluate(2560u, 2u, launchLimited).safe,
            "CUDA calibration approached the grid launch limit");

    CudaCalibrationDeviceLimits watchdog = limits;
    watchdog.kernelExecutionTimeoutEnabled = true;
    okay &= Check(
            !planner.Evaluate(300u, 2u, watchdog).safe,
            "CUDA calibration approached the kernel watchdog limit");

    CudaCalibrationDeviceLimits occupancyLimited = limits;
    occupancyLimited.registersPerMultiprocessor = 32768u;
    okay &= Check(
            !planner.Evaluate(2u, 2u, occupancyLimited).safe,
            "CUDA calibration exceeded multiprocessor occupancy limits");

    CudaCalibrationSafetyPlanner measuredWatchdog;
    CudaCalibrationBatchProfile nearWatchdog = first;
    nearWatchdog.batchSize = 32u;
    nearWatchdog.batchCapacity = 32u;
    nearWatchdog.kernelMilliseconds = 210.0;
    measuredWatchdog.Observe(nearWatchdog);
    watchdog.freeMemoryBytes = limits.freeMemoryBytes;
    const auto measuredWatchdogDecision =
            measuredWatchdog.Evaluate(32u, 32u, watchdog);
    okay &= Check(
            !measuredWatchdogDecision.safe &&
                    measuredWatchdogDecision
                            .predictedKernelMilliseconds > 250.0,
            "CUDA calibration accepted a measured kernel too close "
            "to the watchdog limit");

    CudaCalibrationBatchProfile slowerSameBatch = nearWatchdog;
    slowerSameBatch.batchCapacity = 64u;
    slowerSameBatch.kernelMilliseconds = 220.0;
    measuredWatchdog.Observe(slowerSameBatch);
    const auto duplicateBatchDecision =
            measuredWatchdog.Evaluate(64u, 64u, watchdog);
    okay &= Check(
            !duplicateBatchDecision.safe &&
                    duplicateBatchDecision
                            .predictedKernelMilliseconds >= 550.0,
            "CUDA calibration ignored the slowest profile for a batch "
            "measured under multiple capacities");

    CudaCalibrationSafetyPlanner decreasingKernel;
    CudaCalibrationBatchProfile slowerSmall = nearWatchdog;
    slowerSmall.batchSize = 2u;
    slowerSmall.batchCapacity = 2u;
    CudaCalibrationBatchProfile fasterLarge = nearWatchdog;
    fasterLarge.batchSize = 4u;
    fasterLarge.batchCapacity = 4u;
    fasterLarge.kernelMilliseconds = 100.0;
    decreasingKernel.Observe(slowerSmall);
    decreasingKernel.Observe(fasterLarge);
    const auto decreasingKernelDecision =
            decreasingKernel.Evaluate(3u, 4u, watchdog);
    okay &= Check(
            !decreasingKernelDecision.safe &&
                    decreasingKernelDecision
                            .predictedKernelMilliseconds >= 262.5,
            "CUDA calibration underestimated a noisy decreasing kernel "
            "measurement");

    CudaCalibrationSafetyPlanner localMemory;
    CudaCalibrationBatchProfile localHeavy = first;
    localHeavy.simulationLocalBytesPerThread =
            4u * 1024u * 1024u;
    localMemory.Observe(localHeavy);
    CudaCalibrationDeviceLimits localLimited = limits;
    localLimited.freeMemoryBytes = 1400u * 1024u * 1024u;
    okay &= Check(
            !localMemory.Evaluate(512u, 512u, localLimited).safe,
            "CUDA calibration ignored the kernel local-memory "
            "working set");
    return okay;
}

bool TestCudaConfigurationCoverage() {
    bool okay = true;
    for (const auto &registration :
         forevertas::ModifierRegistry()) {
        try {
            const auto modifiers = forevertas::BuildCudaModifiers(
                    {{registration.id,
                      registration.defaultSettings}},
                    10u);
            okay &= Check(
                    modifiers.size() == 1u,
                    "a registered modifier was not translated for CUDA");
            if (modifiers.size() == 1u) {
                const auto window = std::visit(
                        [](const auto &modifier) {
                            return modifier.window;
                        },
                        modifiers.front());
                const auto minimum = forevertas::ParseSignedDecimal(
                        registration.defaultSettings.at("minTimeMs"));
                const auto maximum = forevertas::ParseSignedDecimal(
                        registration.defaultSettings.at("maxTimeMs"));
                okay &= Check(
                        minimum && maximum &&
                                window.minimumTimeMs == *minimum + 10 &&
                                window.maximumTimeMs == *maximum + 10,
                        "a CUDA input modifier did not receive the one-tick "
                        "offset");
            }
        } catch (...) {
            okay &= Check(
                    false,
                    "a registered modifier was rejected by CUDA");
        }
    }
    for (const auto &registration :
         forevertas::EvaluationTargetRegistry()) {
        if (registration.id ==
            forevertas::kCustomVolumeEntryEvaluationId) {
            try {
                static_cast<void>(forevertas::BuildCudaEvaluator(
                        {registration.id, registration.defaultSettings},
                        10u));
                okay &= Check(
                        false,
                        "custom volume unexpectedly used an inexact CUDA "
                        "evaluator");
            } catch (const std::invalid_argument &) {
            }
            continue;
        }
        try {
            const auto evaluator = forevertas::BuildCudaEvaluator(
                    {registration.id,
                     registration.defaultSettings},
                    10u);
            okay &= Check(
                    evaluator.has_value(),
                    "a registered evaluator skipped CUDA batching");
        } catch (...) {
            okay &= Check(
                    false,
                    "a registered evaluator was rejected by CUDA");
        }
    }
    try {
        static_cast<void>(forevertas::BuildCudaModifiers(
                {{"unsupported-cuda-modifier", {}}}, 10u));
        okay &= Check(
                false,
                "unsupported CUDA modifier did not produce an error");
    } catch (const std::invalid_argument &) {
    }
    try {
        static_cast<void>(forevertas::BuildCudaEvaluator(
                {"unsupported-cuda-evaluator", {}}, 10u));
        okay &= Check(
                false,
                "unsupported CUDA evaluator did not produce an error");
    } catch (const std::invalid_argument &) {
    }
    return okay;
}

bool TestReplayPathRobustness() {
    namespace fs = std::filesystem;

    const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
    const fs::path directory =
            fs::temp_directory_path() /
            fs::u8path("ForeverTAS replay paths #[]&' " +
                       std::to_string(nonce));
    struct DirectoryCleanup final {
        fs::path path;
        ~DirectoryCleanup() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } cleanup{directory};

    std::error_code error;
    fs::create_directories(directory, error);
    bool okay = Check(
            !error, "special-character replay directory creation failed");
    if (error) {
        return false;
    }

    const std::string fileName =
            "run with spaces #[]&' - "
            "\xE6\xB5\x8B\xE8\xAF\x95 - "
            "\xF0\x9F\x8F\x81.Replay.Gbx";
    const fs::path replayPath = directory / fs::u8path(fileName);
    constexpr std::array<std::byte, 8> payload{
            std::byte{0u},
            std::byte{1u},
            std::byte{2u},
            std::byte{127u},
            std::byte{128u},
            std::byte{200u},
            std::byte{254u},
            std::byte{255u}};
    {
        std::ofstream replay(replayPath, std::ios::binary);
        replay.write(
                reinterpret_cast<const char *>(payload.data()),
                static_cast<std::streamsize>(payload.size()));
        okay &= Check(
                replay.good(),
                "special-character replay file creation failed");
    }

    const std::string replayPathUtf8 = replayPath.u8string();
    const forevervalidator::ReplayIdentity identity{replayPathUtf8};
    auto result =
            forevertas::ReadReplayFileUtf8(replayPathUtf8, identity);
    okay &= Check(
            result &&
                    result.Value().size() == payload.size() &&
                    std::equal(
                            result.Value().begin(),
                            result.Value().end(),
                            payload.begin()),
            "UTF-8 replay path did not preserve the file contents");

    auto empty = forevertas::ReadReplayFileUtf8({}, {});
    okay &= Check(
            !empty &&
                    empty.Error().reason ==
                            forevervalidator::ValidationFailureReason::
                                    EmptyReplayPath,
            "empty replay path did not retain its validation error");

    const std::string missingPath =
            (directory / fs::u8path("missing # \xE2\x98\x83.Gbx"))
                    .u8string();
    auto missing = forevertas::ReadReplayFileUtf8(
            missingPath, {missingPath});
    okay &= Check(
            !missing &&
                    missing.Error().reason ==
                            forevervalidator::ValidationFailureReason::
                                    ReplayFileOpenFailed &&
                    missing.Error().relatedAsset == missingPath,
            "failed UTF-8 replay path did not retain its diagnostic path");
    return okay;
}

bool TestConditionLanguageParity() {
    const std::string script =
            "kmh(car.speed) >= 36\n"
            "deg(car.yaw) = 0\n"
            "distance(car.pos, variable(bf_target_point)) < 0.01\n"
            "car.prev.x = 1\n"
            "car.localvel.z = 10\n"
            "car.freewheel = 1\n"
            "car.lateralcontact = 1\n"
            "car.is_sliding = 1\n"
            "car.gear = 3\n"
            "car.rpm = 9000\n"
            "car.tr = 0.25\n"
            "car.tt = 2\n"
            "car.tbf = 1.5\n"
            "car.wheels.frontleft.groundcontact = 1\n"
            "car.wheels.frontleft.is = 1\n"
            "car.wheels.frontleft.surface = 2\n"
            "iterations = 17\n"
            "time_since(last_improvement.time) = 4";
    forevertas::ConditionVariables variables{{
            "bf_target_point", {4.0, 5.0, 6.0, true}}};
    const forevertas::ConditionCompileResult compiled =
            forevertas::CompileConditionScript(script, variables);
    if (compiled.error) {
        std::cerr << *compiled.error << '\n';
    }
    bool okay = Check(
            compiled.program.has_value() && !compiled.error,
            "BfV2-compatible condition script did not compile");
    if (!compiled.program) return false;
    PhysicsSandboxStateView previous;
    previous.car.position = {1.0f, 2.0f, 3.0f};
    PhysicsSandboxStateView current;
    current.car.position = {4.0f, 5.0f, 6.0f};
    current.car.linearSpeed = {0.0f, 0.0f, 10.0f};
    current.car.localSpeed = {0.0f, 0.0f, 10.0f};
    current.car.freeWheeling = true;
    current.car.lateralContact = true;
    current.car.sliding = true;
    current.car.gear = 3;
    current.car.rpm = 9000.0f;
    current.car.turningRate = 0.25f;
    current.car.turboType = 2u;
    current.car.turboBoostFactor = 1.5f;
    current.car.wheelContact[0] = true;
    current.car.wheelSliding[0] = true;
    current.car.wheelSurface[0] = 2u;
    okay &= Check(
            compiled.program->Evaluate(
                    previous, current,
                    {17u, 100.0, 90.0, 104.0}),
            "compiled condition rejected an eligible tick");
    current.car.linearSpeed.z = 9.0f;
    okay &= Check(
            !compiled.program->Evaluate(
                    previous, current,
                    {17u, 100.0, 90.0, 104.0}),
            "compiled condition accepted a tick below the speed threshold");
    const auto invalid = forevertas::CompileConditionScript(
            "kmh(car.speed) > 200\nnot_a_variable = 1");
    okay &= Check(
            !invalid.program && invalid.error &&
                    invalid.error->find("line 2") != std::string::npos,
            "condition compile error did not identify its source line");
    return okay;
}

}  // namespace

int main() {
    const bool okay = TestInputOnlyTimelineTimeOrigin() &&
            TestHumanDurationFormatting() &&
            TestMutableSuffixNormalization() &&
            TestEvaluationTargets() &&
            TestModifierComposition() &&
            TestModifierDeterminism() &&
            TestModifierRandomGoldenSequence() &&
            TestExistingEventWindowPatchParity() &&
            TestAllModifierWindowPatchParity() &&
            TestEveryOrderedModifierPairWindowPatchParity() &&
            TestMultiPassModifierWindowPatchParity() &&
            TestNonCanonicalModifierWindowFallback() &&
            TestModifierWindowBaselineGeneration() &&
            TestInputScriptFormatting() &&
            TestInputScriptParsingAndBaseline() &&
            TestAnalogInputRepresentation() &&
            TestKeyboardSteeringConversion() &&
            TestAllModifierAnalogInvariants() &&
            TestRegistries() &&
            TestLocaleIndependentFloatingPointSettings() &&
            TestStructuredSearchLogEscaping() &&
            TestSearchControl() &&
            TestRollingThroughput() &&
            TestCudaBatchCalibrationStrategy() &&
            TestCudaCalibrationSafety() &&
            TestCudaConfigurationCoverage() &&
            TestConditionLanguageParity() &&
            TestReplayPathRobustness();
    return okay ? 0 : 1;
}
