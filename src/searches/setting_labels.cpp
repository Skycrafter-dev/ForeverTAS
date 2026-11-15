#include "searches/setting_labels.h"

#include "searches/algorithm_registry.h"
#include "searches/option_settings_utils.h"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace forevertas {
namespace {

using Labels = std::vector<std::pair<std::string, std::string>>;

// Each component's settings, as its form labels them, in the form's order.
const std::map<std::string, Labels> &AllLabels() {
    static const std::map<std::string, Labels> labels{
            {kBasicBruteForceSearchId,
            {
             {"autoPromoteBest", "Promote each best result to baseline"}}},
            {kTreeSearchId,
            {
             {"segmentCount", "Segments"},
             {"leafCount", "Leaves per tree (0 = 2^segments)"},
             {"branchedSegmentCount", "Segments changed per attempt (0 = all)"},
             {"varyBranchedSegmentCount", "Change a random number of segments up to that"},
             {"unbranchedSegments", "Other segments"},
             {"flatWorkerCount", "Basic bruteforce workers (multi-threaded CPU)"},
             {"autoPromoteBest", "Promote each best result to baseline"},
             {"branchedSegmentDistribution", "Segments changed: distribution"},
             {"itemKeepPercent", "Share of items kept (%)"},
             {"segmentChoice", "Segment choice"},
             {"migrationSeconds", "Island migration, seconds (0 = share every best)"},
             {"repeatImprovedSegments", "Repeat improved segments"}}},
            {kAdaptiveEscalationSearchId,
            {
             {"segmentCount", "Segments"},
             {"escalateAfterTrees", "Trees without improvement before widening"},
             {"migrationSeconds", "Island migration, seconds (0 = share every best)"},
             {"autoPromoteBest", "Promote each best result to baseline"}}},
            {kRandomSteeringModifierId,
            {
             {"minTimeMs", "Minimum time"},
             {"maxTimeMs", "Maximum time"},
             {"seed", "Seed"}}},
            {kExistingEventPerturbationModifierId,
            {
             {"minTimeMs", "Minimum time"},
             {"maxTimeMs", "Maximum time"},
             {"seed", "Seed"},
             {"minCount", "Minimum event count"},
             {"maxCount", "Maximum event count"},
             {"maxTimeShiftMs", "Maximum timing shift"},
             {"steerMode", "Steering value mode"},
             {"steerDeltaMin", "Steering delta minimum"},
             {"steerDeltaMax", "Steering delta maximum"},
             {"steerAbsoluteMin", "Steering absolute minimum"},
             {"steerAbsoluteMax", "Steering absolute maximum"},
             {"toggleAccelerate", "Toggle accelerate events"},
             {"toggleBrake", "Toggle brake events"}}},
            {kSmoothSteeringModifierId,
            {
             {"minTimeMs", "Minimum time"},
             {"maxTimeMs", "Maximum time"},
             {"seed", "Seed"},
             {"deformationCount", "Deformation count"},
             {"radiusMs", "Radius"},
             {"amplitudeMin", "Amplitude minimum"},
             {"amplitudeMax", "Amplitude maximum"}}},
            {kInputInsertionModifierId,
            {
             {"minTimeMs", "Minimum time"},
             {"maxTimeMs", "Maximum time"},
             {"seed", "Seed"},
             {"steerEnabled", "Enable steering insertion"},
             {"steerMode", "Steering value mode"},
             {"steerAbsoluteMin", "Absolute steering minimum"},
             {"steerAbsoluteMax", "Absolute steering maximum"},
             {"steerOffsetMin", "Steering offset minimum"},
             {"steerOffsetMax", "Steering offset maximum"},
             {"steerMinCount", "Minimum steering insertions"},
             {"steerMaxCount", "Maximum steering insertions"},
             {"steerMaxHoldMs", "Maximum steering hold"},
             {"accelerateEnabled", "Enable accelerate insertion"},
             {"accelerateMinCount", "Minimum accelerate insertions"},
             {"accelerateMaxCount", "Maximum accelerate insertions"},
             {"accelerateMaxHoldMs", "Maximum accelerate hold"},
             {"brakeEnabled", "Enable brake insertion"},
             {"brakeMinCount", "Minimum brake insertions"},
             {"brakeMaxCount", "Maximum brake insertions"},
             {"brakeMaxHoldMs", "Maximum brake hold"}}},
            {kInputDeletionModifierId,
            {
             {"minTimeMs", "Minimum time"},
             {"maxTimeMs", "Maximum time"},
             {"seed", "Seed"},
             {"steerEnabled", "Delete steering events"},
             {"steerMaxCount", "Maximum steering deletions"},
             {"accelerateEnabled", "Delete accelerate events"},
             {"accelerateMaxCount", "Maximum accelerate deletions"},
             {"brakeEnabled", "Delete brake events"},
             {"brakeMaxCount", "Maximum brake deletions"}}},
            {kVelocityEvaluationId,
            {
             {"minTimeMs", "Minimum time"},
             {"maxTimeMs", "Maximum time"},
             {"mode", "Mode"},
             {"alignmentEnabled", "Require direction alignment"},
             {"directionX", "Direction X"},
             {"directionY", "Direction Y"},
             {"directionZ", "Direction Z"},
             {"minAlignmentPercent", "Minimum alignment (%)"},
             {"directionMode", "Direction"},
             {"x", "X"},
             {"y", "Y"},
             {"z", "Z"}}},
            {kStuntPointsEvaluationId,
            {
             {"targetTimeMs", "Target time"}}},
            {kPreciseFinishTimeEvaluationId,
            {
}},
            {kVolumeEntryEvaluationId,
            {
             {"centerX", "Center X"},
             {"centerY", "Center Y"},
             {"centerZ", "Center Z"},
             {"sizeX", "Size X"},
             {"sizeY", "Size Y"},
             {"sizeZ", "Size Z"},
             {"measure", "Measure"}}},
            {kCustomVolumeEntryEvaluationId,
            {
             {"plane", "Plane"},
             {"originX", "Origin X"},
             {"originY", "Origin Y"},
             {"originZ", "Origin Z"},
             {"depth", "Depth"},
             {"polygon", "Outline"},
             {"measure", "Measure"}}},
            {kGateEntryEvaluationId,
            {
             {"gates", "Gates"},
             {"soft", "Rank misses by how close they came"},
             {"yawWeight", "Miss penalty per degree (m)"},
             {"speedWeight", "Miss penalty per km/h (m)"}}},
            {kPointTargetEvaluationId,
            {
             {"minTimeMs", "Minimum time"},
             {"maxTimeMs", "Maximum time"},
             {"x", "X"},
             {"y", "Y"},
             {"z", "Z"}}},
            {kPoseTargetEvaluationId,
            {
             {"minTimeMs", "Minimum time"},
             {"maxTimeMs", "Maximum time"},
             {"x", "X"},
             {"y", "Y"},
             {"z", "Z"},
             {"yawDegrees", "Yaw"},
             {"pitchDegrees", "Pitch"},
             {"rollDegrees", "Roll"},
             {"rotationWeightPercent", "Rotation weight (%)"},
             {"weightX", "Weight X"},
             {"weightY", "Weight Y"},
             {"weightZ", "Weight Z"},
             {"weightYaw", "Weight yaw"},
             {"weightPitch", "Weight pitch"},
             {"weightRoll", "Weight roll"}}},
            {kScriptedTargetEvaluationId,
            {
             {"minTimeMs", "Minimum time"},
             {"maxTimeMs", "Maximum time"},
             {"script", "Script"}}},
            {kDistanceTargetEvaluationId,
            {
             {"minTimeMs", "Minimum time"},
             {"maxTimeMs", "Maximum time"},
             {"eventType", "Event"},
             {"checkpointIndex", "Checkpoint number"},
             {"lap", "Lap"},
             {"checkpointSlot", "Map slot (-1 = any)"},
             {"eventIndex", "Event index (0 = any)"},
             {"route", "Route"},
             {"reach", "Reach"}}},
            {kCheckpointTimeEvaluationId,
            {
             {"eventType", "Event"},
             {"checkpointIndex", "Checkpoint number"},
             {"lap", "Lap"},
             {"checkpointSlot", "Map slot (-1 = any)"},
             {"eventIndex", "Event index (0 = any)"}}},
            {kTimeEvaluationId,
            {
             {"minTimeMs", "Minimum time"},
             {"maxTimeMs", "Maximum time"},
             {"goal", "Goal"}}}};
    return labels;
}

bool IsWholeNumber(const std::string &text) {
    return !text.empty() && text.find_first_not_of("0123456789") == std::string::npos;
}

// What a setting takes, judged by its default value.
std::string Expected(const std::string &defaultValue) {
    if (defaultValue == "true" || defaultValue == "false") return "on or off";
    if (IsWholeNumber(defaultValue)) return "a whole number, 0 or more";
    if (defaultValue.size() > 1 && defaultValue[0] == '-' && IsWholeNumber(defaultValue.substr(1)))
        return "a whole number";
    if (ParseFiniteDouble(defaultValue)) return "a number";
    return "";
}

}  // namespace

std::string SettingLabel(const std::string &componentId, const std::string &key) {
    const auto component = AllLabels().find(componentId);
    if (component == AllLabels().end()) return {};
    for (const auto &[name, label] : component->second)
        if (name == key) return label;
    return {};
}

std::string ExplainSettingsProblem(
        const std::string &componentId,
        const OptionSettings &settings,
        const OptionSettings &defaults,
        const std::string &problem,
        const std::function<std::optional<std::string>(const OptionSettings &)> &validate) {
    if (problem != kUnreadableSetting) return problem;
    // the form's order first, then any other setting
    std::vector<std::string> keys;
    if (const auto component = AllLabels().find(componentId); component != AllLabels().end())
        for (const auto &[key, label] : component->second) keys.push_back(key);
    for (const auto &[key, value] : settings)
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    for (const std::string &key : keys) {
        const auto value = settings.find(key);
        const auto fallback = defaults.find(key);
        if (value == settings.end() || fallback == defaults.end() || value->second == fallback->second) continue;
        OptionSettings repaired = settings;
        repaired[key] = fallback->second;
        const auto after = validate(repaired);
        if (after && *after == problem) continue;
        std::string label = SettingLabel(componentId, key);
        if (label.empty()) label = key;
        std::string text = "\"" + label + "\" can't be \"" + value->second + "\"";
        if (value->second.empty()) text = "\"" + label + "\" is empty";
        const std::string expected = Expected(fallback->second);
        return expected.empty() ? text : text + ": it takes " + expected;
    }
    return "a setting could not be read";
}

}  // namespace forevertas
