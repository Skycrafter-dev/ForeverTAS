#include "searches/algorithm_registry.h"

#include "input_timeline_time.h"
#include "evaluators/precise_finish_time_evaluator.h"
#include "evaluators/custom_volume_entry_evaluator.h"
#include "evaluators/point_target_evaluator.h"
#include "evaluators/pose_target_evaluator.h"
#include "evaluators/stunt_points_evaluator.h"
#include "evaluators/velocity_evaluator.h"
#include "evaluators/volume_entry_evaluator.h"
#include "mutations/existing_event_perturbation_mutator.h"
#include "mutations/input_deletion_mutator.h"
#include "mutations/input_insertion_mutator.h"
#include "mutations/random_steering_mutator.h"
#include "mutations/smooth_steering_mutator.h"
#include "searches/basic_brute_force_search.h"

#include <algorithm>
#include <stdexcept>

namespace forevertas {
namespace {

using SettingsValidator = std::optional<std::string> (*)(
        const OptionSettings &, std::uint32_t);

enum class SettingsEffect {
    SearchPolicy,
    Input,
    Evaluation
};

template<typename Product>
using SettingsFactory = std::unique_ptr<Product> (*)(
        const OptionSettings &, std::uint32_t);

std::optional<OptionSettings> PrepareConfiguredSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs,
        SettingsEffect effect) {
    if (effect != SettingsEffect::Input) return settings;
    return SimulationInputSettingsFromUserTimeline(settings, tickDurationMs);
}

std::optional<std::string> ValidateConfiguredSettings(
        const OptionSettings &userSettings,
        std::uint32_t tickDurationMs,
        SettingsEffect effect,
        SettingsValidator validateSimulationSettings) {
    if (tickDurationMs == 0u) {
        return "tick duration must be greater than zero";
    }
    const std::optional<OptionSettings> simulationSettings =
            PrepareConfiguredSettings(userSettings, tickDurationMs, effect);
    if (!simulationSettings) {
        return "timeline time is too large";
    }
    return validateSimulationSettings(*simulationSettings, tickDurationMs);
}

template<typename Product>
std::unique_ptr<Product> CreateConfiguredComponent(
        const OptionSettings &userSettings,
        std::uint32_t tickDurationMs,
        SettingsEffect effect,
        SettingsValidator validateSimulationSettings,
        SettingsFactory<Product> createFromSimulationSettings) {
    if (tickDurationMs == 0u) {
        throw std::invalid_argument(
                "tick duration must be greater than zero");
    }
    const std::optional<OptionSettings> simulationSettings =
            PrepareConfiguredSettings(userSettings, tickDurationMs, effect);
    if (!simulationSettings) {
        throw std::invalid_argument("timeline time is too large");
    }
    if (const auto error = validateSimulationSettings(
                *simulationSettings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    return createFromSimulationSettings(
            *simulationSettings, tickDurationMs);
}

template<typename Registration>
const Registration *FindRegistration(
        const std::vector<Registration> &registrations,
        const std::string &id) {
    const auto found = std::find_if(
            registrations.begin(),
            registrations.end(),
            [&id](const Registration &registration) {
                return registration.id == id ||
                        std::find(registration.legacyIds.begin(),
                                  registration.legacyIds.end(),
                                  id) != registration.legacyIds.end();
            });
    return found == registrations.end() ? nullptr : &*found;
}

}  // namespace

std::optional<std::string> SearchAlgorithmRegistration::validateSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) const {
    return ValidateConfiguredSettings(
            settings,
            tickDurationMs,
            SettingsEffect::SearchPolicy,
            validateSimulationSettings);
}

std::unique_ptr<SearchAlgorithm> SearchAlgorithmRegistration::create(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) const {
    return CreateConfiguredComponent<SearchAlgorithm>(
            settings,
            tickDurationMs,
            SettingsEffect::SearchPolicy,
            validateSimulationSettings,
            createFromSimulationSettings);
}

std::optional<std::string> ModifierRegistration::validateSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) const {
    return ValidateConfiguredSettings(
            settings,
            tickDurationMs,
            SettingsEffect::Input,
            validateSimulationSettings);
}

std::unique_ptr<InputMutator> ModifierRegistration::create(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) const {
    return CreateConfiguredComponent<InputMutator>(
            settings,
            tickDurationMs,
            SettingsEffect::Input,
            validateSimulationSettings,
            createFromSimulationSettings);
}

std::optional<std::string> EvaluationTargetRegistration::validateSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) const {
    return ValidateConfiguredSettings(
            settings,
            tickDurationMs,
            SettingsEffect::Evaluation,
            validateSimulationSettings);
}

std::unique_ptr<IterationEvaluator> EvaluationTargetRegistration::create(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) const {
    return CreateConfiguredComponent<IterationEvaluator>(
            settings,
            tickDurationMs,
            SettingsEffect::Evaluation,
            validateSimulationSettings,
            createFromSimulationSettings);
}

const std::vector<SearchAlgorithmRegistration> &SearchAlgorithmRegistry() {
    static const std::vector<SearchAlgorithmRegistration> registrations{
            {kBasicBruteForceSearchId,
             {std::string("seri" "al-brute-force")},
             "Basic bruteforce",
             "",
             BasicBruteForceOptionFields(),
             DefaultBasicBruteForceOptionSettings(),
             {},
             &ValidateBasicBruteForceOptionSettings,
             &CreateBasicBruteForceSearch}};
    return registrations;
}

const std::vector<ModifierRegistration> &ModifierRegistry() {
    static const std::vector<ModifierRegistration> registrations{
            {kRandomSteeringModifierId,
             {},
             "Random steering",
             "",
             RandomSteeringOptionFields(),
             DefaultRandomSteeringOptionSettings(),
             {{"minTimeMs", "search/minMutateMs"},
              {"maxTimeMs", "search/maxMutateMs"},
              {"seed", "search/mutationSeed"}},
             &ValidateRandomSteeringOptionSettings,
             &CreateRandomSteeringMutator},
            {kExistingEventPerturbationModifierId,
             {},
             "Existing-event perturbation",
             "",
             ExistingEventPerturbationOptionFields(),
             DefaultExistingEventPerturbationSettings(),
             {},
             &ValidateExistingEventPerturbationSettings,
             &CreateExistingEventPerturbationMutator},
            {kSmoothSteeringModifierId,
             {},
             "Smooth steering deformation",
             "",
             SmoothSteeringOptionFields(),
             DefaultSmoothSteeringSettings(),
             {},
             &ValidateSmoothSteeringSettings,
             &CreateSmoothSteeringMutator},
            {kInputInsertionModifierId,
             {},
             "Input insertion",
             "",
             InputInsertionOptionFields(),
             DefaultInputInsertionSettings(),
             {},
             &ValidateInputInsertionSettings,
             &CreateInputInsertionMutator},
            {kInputDeletionModifierId,
             {},
             "Input deletion",
             "",
             InputDeletionOptionFields(),
             DefaultInputDeletionSettings(),
             {},
             &ValidateInputDeletionSettings,
             &CreateInputDeletionMutator}};
    return registrations;
}

const std::vector<EvaluationTargetRegistration> &EvaluationTargetRegistry() {
    static const std::vector<EvaluationTargetRegistration> registrations{
            {kVelocityEvaluationId,
             {"maximum-speed"},
             "Velocity",
             "",
             VelocityOptionFields(),
             DefaultVelocityOptionSettings(),
             {},
             &ValidateVelocityOptionSettings,
             &CreateVelocityEvaluator},
            {kStuntPointsEvaluationId,
             {},
             "Stunt points",
             "",
             StuntPointsOptionFields(),
             DefaultStuntPointsOptionSettings(),
             {},
             &ValidateStuntPointsOptionSettings,
             &CreateStuntPointsEvaluator},
            {kPreciseFinishTimeEvaluationId,
             {"finish-time"},
             "Precise finish time",
             "",
             {},
             DefaultPreciseFinishTimeOptionSettings(),
             {},
             &ValidatePreciseFinishTimeOptionSettings,
             &CreatePreciseFinishTimeEvaluator},
            {kVolumeEntryEvaluationId,
             {},
             "Volume entry time",
             "VolumeEntryBlockDetail.qml",
             VolumeEntryOptionFields(),
             DefaultVolumeEntryOptionSettings(),
             {},
             &ValidateVolumeEntryOptionSettings,
             &CreateVolumeEntryEvaluator},
            {kCustomVolumeEntryEvaluationId,
             {},
             "Custom volume entry time",
             "VolumeEntryBlockDetail.qml",
             CustomVolumeEntryOptionFields(),
             DefaultCustomVolumeEntryOptionSettings(),
             {},
             &ValidateCustomVolumeEntryOptionSettings,
             &CreateCustomVolumeEntryEvaluator},
            {kPointTargetEvaluationId,
             {},
             "Point target",
             "",
             PointTargetOptionFields(),
             DefaultPointTargetOptionSettings(),
             {},
             &ValidatePointTargetOptionSettings,
             &CreatePointTargetEvaluator},
            {kPoseTargetEvaluationId,
             {},
             "Pose target",
             "PoseTargetBlockDetail.qml",
             PoseTargetOptionFields(),
             DefaultPoseTargetOptionSettings(),
             {},
             &ValidatePoseTargetOptionSettings,
             &CreatePoseTargetEvaluator}};
    return registrations;
}

const SearchAlgorithmRegistration *FindSearchAlgorithm(
        const std::string &id) {
    return FindRegistration(SearchAlgorithmRegistry(), id);
}

const ModifierRegistration *FindModifier(
        const std::string &id) {
    return FindRegistration(ModifierRegistry(), id);
}

const EvaluationTargetRegistration *FindEvaluationTarget(
        const std::string &id) {
    return FindRegistration(EvaluationTargetRegistry(), id);
}

OptionConfiguration DefaultSearchAlgorithmConfiguration() {
    const SearchAlgorithmRegistration &registration =
            SearchAlgorithmRegistry().front();
    return {registration.id, registration.defaultSettings};
}

std::vector<OptionConfiguration> DefaultModifierConfigurations() {
    const ModifierRegistration &registration = ModifierRegistry().front();
    return {{registration.id, registration.defaultSettings}};
}

OptionConfiguration DefaultEvaluationTargetConfiguration() {
    const EvaluationTargetRegistration &registration =
            EvaluationTargetRegistry().front();
    return {registration.id, registration.defaultSettings};
}

}  // namespace forevertas
