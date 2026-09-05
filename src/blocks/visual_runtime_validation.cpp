#include "blocks/visual_runtime.h"
#include "blocks/visual_catalog.h"

#include <functional>
#include <set>

namespace forevertas::blocks {

VisualProgramValidation ValidateExecutableVisualProgram(const VisualProgram &program) {
  auto result = ValidateVisualProgram(program);
  if (!result.ok) return result;
  const auto error = [&](const VisualNode &node, const std::string &message) {
    result.errors.push_back("Block #" + std::to_string(node.id) + " (" + node.definitionId + "): " + message);
  };
  const auto name = [](const VisualNode &node) {
    const auto it = node.fields.find("name");
    return it == node.fields.end() ? std::string{} : it->second;
  };
  std::map<std::string, const VisualNode *> procedures;
  const VisualNode *start = nullptr;
  for (auto id : program.topLevel) {
    const auto &node = *program.find(id);
    if (!node.enabled) continue;
    if (node.definitionId == "flow/when-start") {
      if (start) error(node, "Use one 'when run starts' block.");
      start = &node;
    } else if (node.definitionId == "procedures/define") {
      if (name(node).empty() || !procedures.emplace(name(node), &node).second)
        error(node, "Give each procedure a unique, nonempty name.");
      std::set<std::string> names;
      const auto parameters = VisualProcedureParameters(node);
      if (parameters.size() > 64) error(node, "A procedure may have at most 64 parameters.");
      for (const auto &parameter : parameters)
        if (parameter.empty() || !names.insert(parameter).second)
          error(node, "Parameter names must be nonempty and distinct.");
    }
  }
  if (!start) result.errors.push_back("Add a 'when run starts' block.");

  std::function<void(const VisualNode &, bool, bool)> visit;
  visit = [&](const VisualNode &node, bool inProcedure, bool inLoop) {
    if (!node.enabled) return;
    const auto &id = node.definitionId;
    for (const auto &input : VisualInputsForNode(node))
      if (!node.inputs.count(input.key)) error(node, "Connect input '" + input.key + "'.");
    if (id == "procedures/call" || id == "procedures/value" || id == "procedures/reference") {
      const auto found = procedures.find(name(node));
      if (found == procedures.end()) error(node, "Unknown procedure '" + name(node) + "'.");
      else if (id != "procedures/reference" && VisualProcedureParameters(node) != VisualProcedureParameters(*found->second))
        error(node, "The call's parameters do not match its definition.");
    }
    if (id == "procedures/return" && !inProcedure) error(node, "Return belongs inside a procedure.");
    if ((id == "flow/break" || id == "flow/continue") && !inLoop)
      error(node, "This block belongs inside a loop in the same procedure.");
    if ((id == "data/get" || id == "data/set" || id == "data/change" || id == "data/local" || id == "flow/for-each") && name(node).empty())
      error(node, "Choose a variable name.");
    for (const auto &[key, child] : node.inputs) {
      if (!program.find(child)->enabled) error(node, "Connect an enabled reporter to '"+key+"'.");
      visit(*program.find(child), inProcedure, inLoop);
    }
    const bool loop = inLoop || id == "flow/for-each" || id == "flow/repeat" || id == "flow/while" || id == "flow/until" ||
        id == "flow/forever";
    for (const auto &[key, children] : node.statements) {
      (void)key;
      for (auto child : children) visit(*program.find(child), inProcedure, loop);
    }
  };
  if (start) visit(*start, false, false);
  for (auto id : program.topLevel) {
    const auto &node = *program.find(id);
    if (node.definitionId.rfind("events/", 0) == 0 &&
        FindVisualBlock(node.definitionId)->shape == VisualBlockShape::Hat)
      visit(node, true, false);
  }
  for (const auto &[key, procedure] : procedures) { (void)key; visit(*procedure, true, false); }
  result.ok = result.errors.empty();
  return result;
}
} // namespace forevertas::blocks
