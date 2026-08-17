#ifndef FOREVERTAS_SEARCHES_ALGORITHM_REGISTRY_H
#define FOREVERTAS_SEARCHES_ALGORITHM_REGISTRY_H

#include "evaluators/iteration_evaluator.h"
#include "mutations/input_mutator.h"
#include "searches/option_configuration.h"
#include "searches/option_fields.h"
#include "searches/search_algorithm.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace forevertas {

inline constexpr char kBasicBruteForceSearchId[] = "basic-brute-force";
inline constexpr char kRandomSteeringModifierId[] = "random-steering";
inline constexpr char kExistingEventPerturbationModifierId[] =
        "existing-event-perturbation";
inline constexpr char kSmoothSteeringModifierId[] = "smooth-steering";
inline constexpr char kInputInsertionModifierId[] = "input-insertion";
inline constexpr char kInputDeletionModifierId[] = "input-deletion";
inline constexpr char kVelocityEvaluationId[] = "velocity";
inline constexpr char kPreciseFinishTimeEvaluationId[] =
        "precise-finish-time";
inline constexpr char kStuntPointsEvaluationId[] = "stunt-points";
inline constexpr char kVolumeEntryEvaluationId[] = "volume-entry-time";
inline constexpr char kCustomVolumeEntryEvaluationId[] =
        "custom-volume-entry-time";
inline constexpr char kPointTargetEvaluationId[] = "point-target";
inline constexpr char kPoseTargetEvaluationId[] = "pose-target";

struct SearchAlgorithmRegistration {
    std::string id;
    std::vector<std::string> legacyIds;
    std::string displayName;
    // Optional rich QML detail component for options that manage app-level
    // collections; scalar settings render from `fields` generically.
    std::string settingsComponent;
    OptionFieldList fields;
    OptionSettings defaultSettings;
    OptionSettings legacyPersistenceKeys;

    // Search-policy settings do not receive the input-timeline offset.
    // Application code must use validateSettings() and create().
    std::optional<std::string> (*validateSimulationSettings)(
            const OptionSettings &, std::uint32_t);
    std::unique_ptr<SearchAlgorithm> (*createFromSimulationSettings)(
            const OptionSettings &, std::uint32_t);

    std::optional<std::string> validateSettings(
            const OptionSettings &settings,
            std::uint32_t tickDurationMs) const;
    std::unique_ptr<SearchAlgorithm> create(
            const OptionSettings &settings,
            std::uint32_t tickDurationMs) const;
};

struct ModifierRegistration {
    std::string id;
    std::vector<std::string> legacyIds;
    std::string displayName;
    // Optional rich QML detail component for options that manage app-level
    // collections; scalar settings render from `fields` generically.
    std::string settingsComponent;
    OptionFieldList fields;
    OptionSettings defaultSettings;
    OptionSettings legacyPersistenceKeys;

    // Internal hooks receive input timing settings after the user timeline
    // origin has been translated. Application code must use validateSettings()
    // and create().
    std::optional<std::string> (*validateSimulationSettings)(
            const OptionSettings &, std::uint32_t);
    std::unique_ptr<InputMutator> (*createFromSimulationSettings)(
            const OptionSettings &, std::uint32_t);

    std::optional<std::string> validateSettings(
            const OptionSettings &settings,
            std::uint32_t tickDurationMs) const;
    std::unique_ptr<InputMutator> create(
            const OptionSettings &settings,
            std::uint32_t tickDurationMs) const;
};

struct EvaluationTargetRegistration {
    std::string id;
    std::vector<std::string> legacyIds;
    std::string displayName;
    // Optional rich QML detail component for options that manage app-level
    // collections; scalar settings render from `fields` generically.
    std::string settingsComponent;
    OptionFieldList fields;
    OptionSettings defaultSettings;
    OptionSettings legacyPersistenceKeys;

    // Evaluation settings use their entered simulation times directly and do
    // not receive the input-timeline offset. Application code must use
    // validateSettings() and create().
    std::optional<std::string> (*validateSimulationSettings)(
            const OptionSettings &, std::uint32_t);
    std::unique_ptr<IterationEvaluator> (*createFromSimulationSettings)(
            const OptionSettings &, std::uint32_t);

    std::optional<std::string> validateSettings(
            const OptionSettings &settings,
            std::uint32_t tickDurationMs) const;
    std::unique_ptr<IterationEvaluator> create(
            const OptionSettings &settings,
            std::uint32_t tickDurationMs) const;
};

const std::vector<SearchAlgorithmRegistration> &SearchAlgorithmRegistry();
const std::vector<ModifierRegistration> &ModifierRegistry();
const std::vector<EvaluationTargetRegistration> &EvaluationTargetRegistry();

const SearchAlgorithmRegistration *FindSearchAlgorithm(
        const std::string &id);
const ModifierRegistration *FindModifier(
        const std::string &id);
const EvaluationTargetRegistration *FindEvaluationTarget(
        const std::string &id);

OptionConfiguration DefaultSearchAlgorithmConfiguration();
std::vector<OptionConfiguration> DefaultModifierConfigurations();
OptionConfiguration DefaultEvaluationTargetConfiguration();

}  // namespace forevertas

#endif
