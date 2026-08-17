#include "blocks/block_catalog.h"

#include "searches/algorithm_registry.h"

#include <algorithm>
#include <utility>

namespace forevertas::blocks {
namespace {

BlockDefinition AtomBlock(const std::string &localId,
                          const std::string &label,
                          BlockShape shape,
                          const std::string &outputType,
                          const std::string &optionKind,
                          const std::string &optionId,
                          OptionFieldList fields,
                          const std::string &settingsComponent) {
    BlockDefinition definition;
    definition.id = "mutate/" + localId;
    definition.categoryId = "mutate";
    definition.label = label;
    definition.shape = shape;
    definition.outputType = outputType;
    definition.optionKind = optionKind;
    definition.optionId = optionId;
    definition.fields = std::move(fields);
    definition.settingsComponent = settingsComponent;
    return definition;
}

BlockDefinition EvaluationBlock(const std::string &localId,
                                const std::string &label,
                                const std::string &optionId,
                                OptionFieldList fields,
                                const std::string &settingsComponent) {
    BlockDefinition definition;
    definition.id = "evaluate/" + localId;
    definition.categoryId = "evaluate";
    definition.label = label;
    definition.shape = BlockShape::Reporter;
    definition.outputType = "evaluation";
    definition.optionKind = "evaluation";
    definition.optionId = optionId;
    definition.fields = std::move(fields);
    definition.settingsComponent = settingsComponent;
    return definition;
}

OptionField Grouped(OptionField field, const std::string &group) {
    field.group = group;
    return field;
}

// Window fields shared by every mutation window container.
OptionFieldList WindowFields() {
    OptionFieldList fields;
    AppendWindowFields(fields, "1000", "5990");
    AppendSeedField(fields);
    return fields;
}

// Event-count fields shared by the existing-event perturbation atoms.
void AppendCountFields(OptionFieldList &fields) {
    fields.push_back(NumberField("minCount", "Min events", "1"));
    fields.push_back(NumberField("maxCount", "Max events", "3"));
}

BlockDefinition OperatorBlock(const std::string &id,
                              const std::string &label,
                              const std::string &leftDefault,
                              const std::string &rightDefault) {
    BlockDefinition definition;
    definition.id = "values/" + id;
    definition.categoryId = "values";
    definition.label = label;
    definition.shape = BlockShape::Reporter;
    definition.outputType = "number";
    definition.fields.push_back(
            NumberField("left", std::string(), leftDefault));
    definition.fields.push_back(
            NumberField("right", std::string(), rightDefault));
    return definition;
}

}  // namespace

const std::vector<BlockDefinition> &BlockCatalog() {
    static const std::vector<BlockDefinition> catalog = [] {
        std::vector<BlockDefinition> definitions;

        // Search hats: derived from the registry so every registered
        // algorithm stays available as a program root.
        for (const SearchAlgorithmRegistration &registration :
             SearchAlgorithmRegistry()) {
            BlockDefinition definition;
            definition.id = "search/" + registration.id;
            definition.categoryId = "search";
            definition.label = registration.displayName;
            definition.shape = BlockShape::Hat;
            definition.fields = registration.fields;
            definition.optionKind = "search";
            definition.optionId = registration.id;
            definition.settingsComponent = registration.settingsComponent;
            definitions.push_back(std::move(definition));
        }

        // Mutation windows: the shared mutation window (from/to times plus
        // seed) that scopes the atoms inside it.
        definitions.push_back(AtomBlock(
                "window",
                "inputs in window",
                BlockShape::Container,
                std::string(),
                "mutation",
                std::string(),
                WindowFields(),
                std::string()));

        // Mutation atoms bound to existing-event perturbation.
        {
            OptionFieldList fields;
            AppendCountFields(fields);
            fields.push_back(NumberField(
                    "maxTimeShiftMs", "Max shift (ms)", "100"));
            definitions.push_back(AtomBlock(
                    "shift-events",
                    "shift existing events",
                    BlockShape::Stack,
                    std::string(),
                    "mutation",
                    kExistingEventPerturbationModifierId,
                    std::move(fields),
                    std::string()));
        }
        {
            OptionFieldList fields;
            AppendCountFields(fields);
            fields.push_back(ClampedField(
                    "steerDeltaMin", "Nudge min", "-0.15", -1.0, 1.0, 3, 0.01));
            fields.push_back(ClampedField(
                    "steerDeltaMax", "Nudge max", "0.15", -1.0, 1.0, 3, 0.01));
            definitions.push_back(AtomBlock(
                    "nudge-steering",
                    "nudge steering",
                    BlockShape::Stack,
                    std::string(),
                    "mutation",
                    kExistingEventPerturbationModifierId,
                    std::move(fields),
                    std::string()));
        }
        {
            OptionFieldList fields;
            AppendCountFields(fields);
            fields.push_back(ClampedField(
                    "steerAbsoluteMin", "Value min", "-1", -1.0, 1.0, 3, 0.01));
            fields.push_back(ClampedField(
                    "steerAbsoluteMax", "Value max", "1", -1.0, 1.0, 3, 0.01));
            definitions.push_back(AtomBlock(
                    "set-steering",
                    "set steering",
                    BlockShape::Stack,
                    std::string(),
                    "mutation",
                    kExistingEventPerturbationModifierId,
                    std::move(fields),
                    std::string()));
        }
        {
            OptionFieldList fields;
            AppendCountFields(fields);
            definitions.push_back(AtomBlock(
                    "flip-accelerate",
                    "flip accelerate presses",
                    BlockShape::Stack,
                    std::string(),
                    "mutation",
                    kExistingEventPerturbationModifierId,
                    std::move(fields),
                    std::string()));
        }
        {
            OptionFieldList fields;
            AppendCountFields(fields);
            definitions.push_back(AtomBlock(
                    "flip-brake",
                    "flip brake presses",
                    BlockShape::Stack,
                    std::string(),
                    "mutation",
                    kExistingEventPerturbationModifierId,
                    std::move(fields),
                    std::string()));
        }

        // Mutation atoms bound to input insertion.
        {
            OptionFieldList fields;
            fields.push_back(ClampedField(
                    "steerAbsoluteMin", "Value min", "-1", -1.0, 1.0, 3, 0.01));
            fields.push_back(ClampedField(
                    "steerAbsoluteMax", "Value max", "1", -1.0, 1.0, 3, 0.01));
            fields.push_back(NumberField("steerMinCount", "Min inserts", "0"));
            fields.push_back(NumberField("steerMaxCount", "Max inserts", "2"));
            fields.push_back(NumberField("steerMaxHoldMs", "Max hold (ms)", "200"));
            definitions.push_back(AtomBlock(
                    "insert-steering-at",
                    "insert steering at value",
                    BlockShape::Stack,
                    std::string(),
                    "mutation",
                    kInputInsertionModifierId,
                    std::move(fields),
                    std::string()));
        }
        {
            OptionFieldList fields;
            fields.push_back(ClampedField(
                    "steerOffsetMin", "Offset min", "-0.2", -1.0, 1.0, 3, 0.01));
            fields.push_back(ClampedField(
                    "steerOffsetMax", "Offset max", "0.2", -1.0, 1.0, 3, 0.01));
            fields.push_back(NumberField("steerMinCount", "Min inserts", "0"));
            fields.push_back(NumberField("steerMaxCount", "Max inserts", "2"));
            fields.push_back(NumberField("steerMaxHoldMs", "Max hold (ms)", "200"));
            definitions.push_back(AtomBlock(
                    "adjust-steering-by",
                    "adjust steering by offset",
                    BlockShape::Stack,
                    std::string(),
                    "mutation",
                    kInputInsertionModifierId,
                    std::move(fields),
                    std::string()));
        }
        {
            OptionFieldList fields;
            fields.push_back(
                    NumberField("accelerateMinCount", "Min inserts", "0"));
            fields.push_back(
                    NumberField("accelerateMaxCount", "Max inserts", "1"));
            fields.push_back(NumberField(
                    "accelerateMaxHoldMs", "Max hold (ms)", "200"));
            definitions.push_back(AtomBlock(
                    "press-accelerate",
                    "insert accelerate presses",
                    BlockShape::Stack,
                    std::string(),
                    "mutation",
                    kInputInsertionModifierId,
                    std::move(fields),
                    std::string()));
        }
        {
            OptionFieldList fields;
            fields.push_back(NumberField("brakeMinCount", "Min inserts", "0"));
            fields.push_back(NumberField("brakeMaxCount", "Max inserts", "1"));
            fields.push_back(NumberField("brakeMaxHoldMs", "Max hold (ms)", "200"));
            definitions.push_back(AtomBlock(
                    "press-brake",
                    "insert brake presses",
                    BlockShape::Stack,
                    std::string(),
                    "mutation",
                    kInputInsertionModifierId,
                    std::move(fields),
                    std::string()));
        }

        // Mutation atoms bound to input deletion.
        {
            OptionFieldList fields;
            fields.push_back(NumberField("steerMaxCount", "Max deletions", "2"));
            definitions.push_back(AtomBlock(
                    "delete-steering",
                    "delete steering events",
                    BlockShape::Stack,
                    std::string(),
                    "mutation",
                    kInputDeletionModifierId,
                    std::move(fields),
                    std::string()));
        }
        {
            OptionFieldList fields;
            fields.push_back(
                    NumberField("accelerateMaxCount", "Max deletions", "1"));
            definitions.push_back(AtomBlock(
                    "delete-accelerate",
                    "delete accelerate events",
                    BlockShape::Stack,
                    std::string(),
                    "mutation",
                    kInputDeletionModifierId,
                    std::move(fields),
                    std::string()));
        }
        {
            OptionFieldList fields;
            fields.push_back(NumberField("brakeMaxCount", "Max deletions", "1"));
            definitions.push_back(AtomBlock(
                    "delete-brake",
                    "delete brake events",
                    BlockShape::Stack,
                    std::string(),
                    "mutation",
                    kInputDeletionModifierId,
                    std::move(fields),
                    std::string()));
        }

        // Mutation atoms bound to the single-op mutators.
        definitions.push_back(AtomBlock(
                "reroll-steering",
                "reroll steering",
                BlockShape::Stack,
                std::string(),
                "mutation",
                kRandomSteeringModifierId,
                OptionFieldList{},
                std::string()));
        {
            OptionFieldList fields;
            fields.push_back(
                    NumberField("deformationCount", "Deformations", "1"));
            fields.push_back(NumberField("radiusMs", "Radius (ms)", "200"));
            fields.push_back(ClampedField(
                    "amplitudeMin", "Amplitude min", "-0.2", -1.0, 1.0, 2, 0.01));
            fields.push_back(ClampedField(
                    "amplitudeMax", "Amplitude max", "0.2", -1.0, 1.0, 2, 0.01));
            definitions.push_back(AtomBlock(
                    "smooth-steering",
                    "smooth steering deformation",
                    BlockShape::Stack,
                    std::string(),
                    "mutation",
                    kSmoothSteeringModifierId,
                    std::move(fields),
                    std::string()));
        }

        // Evaluation goals.
        definitions.push_back(EvaluationBlock(
                "finish-time",
                "finish time",
                kPreciseFinishTimeEvaluationId,
                OptionFieldList{},
                std::string()));
        {
            OptionFieldList fields;
            fields.push_back(
                    NumberField("targetTimeMs", "Deadline (ms)", "6000"));
            definitions.push_back(EvaluationBlock(
                    "stunt-points",
                    "stunt points",
                    kStuntPointsEvaluationId,
                    std::move(fields),
                    std::string()));
        }
        {
            OptionFieldList fields;
            AppendWindowFields(fields, "1000", "6000");
            definitions.push_back(EvaluationBlock(
                    "speed",
                    "speed",
                    kVelocityEvaluationId,
                    std::move(fields),
                    std::string()));
        }
        {
            OptionFieldList fields;
            AppendWindowFields(fields, "1000", "6000");
            fields.push_back(Grouped(
                    NumberField("directionX", "X", "1"), "Direction"));
            fields.push_back(Grouped(
                    NumberField("directionY", "Y", "0"), "Direction"));
            fields.push_back(Grouped(
                    NumberField("directionZ", "Z", "0"), "Direction"));
            fields.push_back(ClampedField("minAlignmentPercent",
                                          "Minimum alignment (%)",
                                          "-100",
                                          -100.0,
                                          100.0,
                                          0,
                                          1));
            definitions.push_back(EvaluationBlock(
                    "speed-toward",
                    "speed toward direction",
                    kVelocityEvaluationId,
                    std::move(fields),
                    std::string()));
        }
        {
            OptionFieldList fields;
            AppendWindowFields(fields, "1000", "6000");
            fields.push_back(Grouped(NumberField("x", "X", "0"), "Target point"));
            fields.push_back(Grouped(NumberField("y", "Y", "0"), "Target point"));
            fields.push_back(Grouped(NumberField("z", "Z", "0"), "Target point"));
            definitions.push_back(EvaluationBlock(
                    "distance-to-point",
                    "distance to point",
                    kPointTargetEvaluationId,
                    std::move(fields),
                    std::string()));
        }
        {
            OptionFieldList fields;
            AppendWindowFields(fields, "1000", "6000");
            fields.push_back(ClampedField("rotationWeightPercent",
                                          "Rotation weight (%)",
                                          "50",
                                          0.0,
                                          100.0,
                                          0,
                                          1));
            fields.push_back(Grouped(NumberField("x", "X", "0"), "Position"));
            fields.push_back(Grouped(NumberField("y", "Y", "0"), "Position"));
            fields.push_back(Grouped(NumberField("z", "Z", "0"), "Position"));
            fields.push_back(
                    Grouped(NumberField("yawDegrees", "Yaw (deg)", "0"), "Rotation"));
            fields.push_back(Grouped(
                    NumberField("pitchDegrees", "Pitch (deg)", "0"), "Rotation"));
            fields.push_back(Grouped(
                    NumberField("rollDegrees", "Roll (deg)", "0"), "Rotation"));
            definitions.push_back(EvaluationBlock(
                    "distance-to-pose",
                    "distance to pose",
                    kPoseTargetEvaluationId,
                    std::move(fields),
                    "PoseTargetBlockDetail.qml"));
        }
        {
            OptionFieldList fields;
            fields.push_back(
                    Grouped(NumberField("centerX", "Center X", "0"), "Center"));
            fields.push_back(
                    Grouped(NumberField("centerY", "Center Y", "0"), "Center"));
            fields.push_back(
                    Grouped(NumberField("centerZ", "Center Z", "0"), "Center"));
            fields.push_back(Grouped(NumberField("sizeX", "Size X", "10"), "Size"));
            fields.push_back(Grouped(NumberField("sizeY", "Size Y", "10"), "Size"));
            fields.push_back(Grouped(NumberField("sizeZ", "Size Z", "10"), "Size"));
            definitions.push_back(EvaluationBlock(
                    "box-entry-time",
                    "entry time into box",
                    kVolumeEntryEvaluationId,
                    std::move(fields),
                    "VolumeEntryBlockDetail.qml"));
        }
        {
            OptionFieldList fields;
            fields.push_back(EnumField("plane",
                                       "Plane",
                                       {{"xz", "XZ"}, {"xy", "XY"}, {"yz", "YZ"}}));
            fields.push_back(
                    Grouped(NumberField("originX", "Origin X", "0"), "Origin"));
            fields.push_back(
                    Grouped(NumberField("originY", "Origin Y", "0"), "Origin"));
            fields.push_back(
                    Grouped(NumberField("originZ", "Origin Z", "0"), "Origin"));
            fields.push_back(NumberField("depth", "Depth", "5"));
            fields.push_back(LineField("polygon",
                                       "Polygon (x,y;x,y;...)",
                                       "-5,-5;5,-5;0,5"));
            definitions.push_back(EvaluationBlock(
                    "prism-entry-time",
                    "entry time into prism",
                    kCustomVolumeEntryEvaluationId,
                    std::move(fields),
                    "VolumeEntryBlockDetail.qml"));
        }

        // Value primitives.
        BlockDefinition numberLiteral;
        numberLiteral.id = "values/number";
        numberLiteral.categoryId = "values";
        numberLiteral.label = "number";
        numberLiteral.shape = BlockShape::Reporter;
        numberLiteral.outputType = "number";
        numberLiteral.fields.push_back(
                NumberField("value", std::string(), "0"));
        definitions.push_back(std::move(numberLiteral));
        definitions.push_back(OperatorBlock("add", "+", "0", "0"));
        definitions.push_back(
                OperatorBlock("subtract", "\xE2\x88\x92", "0", "0"));
        definitions.push_back(
                OperatorBlock("multiply", "\xC3\x97", "1", "1"));
        definitions.push_back(
                OperatorBlock("divide", "\xC3\xB7", "0", "1"));
        definitions.push_back(OperatorBlock("minimum", "min", "0", "0"));
        definitions.push_back(OperatorBlock("maximum", "max", "0", "0"));
        return definitions;
    }();
    return catalog;
}

const BlockDefinition *FindBlock(const std::string &id) {
    const std::vector<BlockDefinition> &catalog = BlockCatalog();
    const auto found = std::find_if(
            catalog.begin(),
            catalog.end(),
            [&id](const BlockDefinition &definition) {
                return definition.id == id;
            });
    return found == catalog.end() ? nullptr : &*found;
}

const std::vector<BlockCategory> &BlockCategories() {
    static const std::vector<BlockCategory> categories{
            {"search", "Search", "#d97706"},
            {"evaluate", "Evaluate", "#2563eb"},
            {"mutate", "Mutate", "#7c3aed"},
            {"values", "Values", "#0f766e"}};
    return categories;
}

const BlockCategory *FindBlockCategory(const std::string &id) {
    const std::vector<BlockCategory> &categories = BlockCategories();
    const auto found = std::find_if(
            categories.begin(),
            categories.end(),
            [&id](const BlockCategory &category) {
                return category.id == id;
            });
    return found == categories.end() ? nullptr : &*found;
}

}  // namespace forevertas::blocks
