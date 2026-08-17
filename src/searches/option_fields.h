#ifndef FOREVERTAS_SEARCHES_OPTION_FIELDS_H
#define FOREVERTAS_SEARCHES_OPTION_FIELDS_H

#include <string>
#include <utility>
#include <vector>

namespace forevertas {

// Typed description of one settings key owned by a registered option.
// Registrations declare their fields next to their default settings so
// the block system can derive editing slots, the text interchange, and
// compile-time settings transport from one schema.
struct OptionField {
    enum class Kind {
        Number,   // numeric literal slot; accepts number reporters
        Line,     // free-form text slot
        Enum,     // one of a fixed value set
        Boolean,  // "true" / "false"
        Mirrored  // owned by an app-level target collection; filled at
                  // compile time from the selected target
    };

    std::string key;
    std::string label;
    Kind kind = Kind::Number;
    std::string defaultValue;  // exact settings-string default
    // Enum fields: settings value and display label per choice.
    std::vector<std::pair<std::string, std::string>> enumValues;
    double minimum = 0.0;
    double maximum = 0.0;
    bool hasRange = false;
    int decimals = 0;
    double step = 1.0;
    bool isSeed = false;      // participates in seed randomization
    std::string group;        // optional display grouping
    std::string mirrorAsset;  // "cuboid" | "custom-volume" | "pose"
};

using OptionFieldList = std::vector<OptionField>;

inline OptionField NumberField(std::string key,
                               std::string label,
                               std::string defaultValue) {
    OptionField field;
    field.key = std::move(key);
    field.label = std::move(label);
    field.kind = OptionField::Kind::Number;
    field.defaultValue = std::move(defaultValue);
    return field;
}

inline OptionField SeedField(std::string key,
                             std::string label,
                             std::string defaultValue) {
    OptionField field = NumberField(
            std::move(key), std::move(label), std::move(defaultValue));
    field.isSeed = true;
    return field;
}

inline OptionField ClampedField(std::string key,
                                std::string label,
                                std::string defaultValue,
                                double minimum,
                                double maximum,
                                int decimals,
                                double step) {
    OptionField field = NumberField(
            std::move(key), std::move(label), std::move(defaultValue));
    field.minimum = minimum;
    field.maximum = maximum;
    field.hasRange = true;
    field.decimals = decimals;
    field.step = step;
    return field;
}

inline OptionField EnumField(
        std::string key,
        std::string label,
        std::vector<std::pair<std::string, std::string>> enumValues) {
    OptionField field;
    field.key = std::move(key);
    field.label = std::move(label);
    field.kind = OptionField::Kind::Enum;
    if (!enumValues.empty()) field.defaultValue = enumValues.front().first;
    field.enumValues = std::move(enumValues);
    return field;
}

inline OptionField BooleanField(std::string key,
                                std::string label,
                                bool defaultValue) {
    OptionField field;
    field.key = std::move(key);
    field.label = std::move(label);
    field.kind = OptionField::Kind::Boolean;
    field.defaultValue = defaultValue ? "true" : "false";
    return field;
}

inline OptionField LineField(std::string key,
                             std::string label,
                             std::string defaultValue) {
    OptionField field;
    field.key = std::move(key);
    field.label = std::move(label);
    field.kind = OptionField::Kind::Line;
    field.defaultValue = std::move(defaultValue);
    return field;
}

inline OptionField MirroredField(std::string key,
                                 std::string mirrorAsset,
                                 std::string defaultValue) {
    OptionField field;
    field.key = std::move(key);
    field.label = std::string();
    field.kind = OptionField::Kind::Mirrored;
    field.mirrorAsset = std::move(mirrorAsset);
    field.defaultValue = std::move(defaultValue);
    return field;
}

// Shared shapes used by several options.
inline void AppendWindowFields(OptionFieldList &fields,
                               const std::string &minimum,
                               const std::string &maximum) {
    fields.push_back(NumberField("minTimeMs", "From (ms)", minimum));
    fields.push_back(NumberField("maxTimeMs", "To (ms)", maximum));
}

inline void AppendSeedField(OptionFieldList &fields) {
    fields.push_back(SeedField("seed", "Seed", "1179926867"));
}

}  // namespace forevertas

#endif
