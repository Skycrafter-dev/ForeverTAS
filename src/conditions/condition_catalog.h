#ifndef FOREVERTAS_CONDITIONS_CONDITION_CATALOG_H
#define FOREVERTAS_CONDITIONS_CONDITION_CATALOG_H

#include <forevervalidator/experimental/physics_sandbox.h>

#include <string>
#include <vector>

namespace forevertas {

struct ConditionSymbol {
    std::string name;
    std::vector<std::string> aliases;
    std::string type;
    std::string units;
    std::string description;
    forevervalidator::experimental::PhysicsSandboxCudaConditionValue value;
    int component = 0;
    bool conditionsOnly = false;
};

struct ConditionFunction {
    std::string name;
    std::vector<std::string> aliases;
    std::string type;
    std::string units;
    std::string description;
    std::string example;
    bool conditionsOnly = false;
    bool needsPointTarget = false;
};

const std::vector<ConditionSymbol> &ConditionSymbols();
const std::vector<ConditionFunction> &ConditionFunctions();

}  // namespace forevertas
#endif
