#ifndef FOREVERTAS_CONDITIONS_CONDITION_LANGUAGE_CATALOG_H
#define FOREVERTAS_CONDITIONS_CONDITION_LANGUAGE_CATALOG_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <forevervalidator/experimental/physics_sandbox.h>

namespace forevertas {

enum class ConditionLanguageFunctionKind {
    KmH,
    Deg,
    TimeSince,
    Distance,
    VariableLookup
};

enum class ConditionLanguageSymbolKind { Scalar, Vector };

enum class ConditionLanguageValueType {
    Unknown,
    Scalar,
    Vector,
    ExternalName,
    Dynamic
};

struct ConditionLanguageSymbol final {
    std::string_view canonicalName;
    std::vector<std::string_view> aliases;
    std::string_view friendlyName;
    std::string_view category;
    std::string_view type;
    std::string_view unit;
    std::string_view detail;
    std::string_view documentation;
    std::string_view insertionTemplate;
    std::string_view example;
    bool pointTargetOnly = false;
    bool external = false;
    ConditionLanguageSymbolKind kind = ConditionLanguageSymbolKind::Scalar;
    forevervalidator::experimental::PhysicsSandboxCudaConditionValue sourceValue =
            forevervalidator::experimental::PhysicsSandboxCudaConditionValue::CurrentTime;
    std::uint32_t sourceComponent = 0u;
};

struct ConditionLanguageFunction final {
    std::string_view canonicalName;
    std::vector<std::string_view> aliases;
    std::string_view friendlyName;
    std::string_view category;
    std::string_view signature;
    std::string_view returnType;
    std::string_view documentation;
    std::string_view insertionTemplate;
    std::string_view example;
    ConditionLanguageFunctionKind kind;
    bool pointTargetOnly = false;
    std::vector<ConditionLanguageValueType> parameterTypes;
    ConditionLanguageValueType resultType =
            ConditionLanguageValueType::Unknown;
};

struct ConditionLanguageSymbolResolution final {
    const ConditionLanguageSymbol *symbol;
    forevervalidator::experimental::PhysicsSandboxCudaConditionValue sourceValue;
    std::uint32_t sourceComponent;
};

const ConditionLanguageSymbol *FindConditionSymbol(std::string_view name);
const ConditionLanguageFunction *FindConditionFunction(std::string_view name);
const std::vector<ConditionLanguageSymbol> &GetConditionSymbols();
const std::vector<ConditionLanguageFunction> &GetConditionFunctions();
std::optional<ConditionLanguageSymbolResolution> ResolveConditionSymbol(
        std::string_view name);

}  // namespace forevertas

#endif
