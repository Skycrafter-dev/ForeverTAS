#include "blocks/visual_catalog.h"
#include "blocks/visual_compiler.h"
#include "blocks/visual_macros.h"

#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
using namespace forevertas::blocks;
using Id = VisualNodeId;
void Check(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
class Builder {
public:
  VisualProgram program;
  Id add(const std::string &type, std::map<std::string, Id> inputs = {},
         std::map<std::string, std::string> fields = {},
         std::map<std::string, std::vector<Id>> statements = {}) {
    const Id id = program.nodes.size() + 1;
    program.nodes.emplace(id, VisualNode{id,type,std::move(fields),std::move(inputs),std::move(statements)});
    return id;
  }
  Id number() { return add("values/number", {}, {{"value", "1"}}); }
  Id set(Id value) { return add("data/set", {{"value", value}}, {{"name", "answer"}}); }
  Id start(std::vector<Id> body) {
    const Id id=add("flow/when-start", {}, {}, {{"body",std::move(body)}});
    program.topLevel.push_back(id); return id;
  }
};
void CatalogAndMacros() {
  std::set<std::string> ids,types,categories;
  for (const auto &category:VisualCategories()) categories.insert(category.id);
  for (const auto &block:VisualBlockCatalog()) {
    Check(ids.insert(block.id).second && types.insert(block.blocklyType).second,"Duplicate primitive ID/type.");
    Check(categories.count(block.categoryId),"Missing category: "+block.id);
    Check(block.id.rfind("mutate/",0)!=0 && block.id.rfind("search/",0)!=0 && block.id.rfind("objective/",0)!=0,
          "Native policy survived in the primitive catalog: "+block.id);
    for (const auto &input:block.inputs)
      Check(input.defaultBlockId.empty() || FindVisualBlock(input.defaultBlockId),"Dangling default: "+block.id+"/"+input.key);
    if (block.shape==VisualBlockShape::Command || block.shape==VisualBlockShape::Control)
      Check(block.statementFamily=="command","A primitive retains a search-phase placement restriction.");
  }
  for (const auto &removed:{"flow/simulate","flow/mutation-window","flow/set-objective","conditions/legacy-script","simulation/accepted","results/publish-sample","values/polygon"})
    Check(!FindVisualBlock(removed),std::string("Legacy wrapper was hidden rather than deleted: ")+removed);
  Check(FindVisualBlock("flow/when-start")->fields.empty(),"Native/program execution selector survived.");
  Check(FindVisualBlock("simulation/step")->inputs.empty(),"One-tick primitive hides repetition.");
  std::set<std::string> macroIds,macroCategories;
  for (const auto &macro:VisualMacroCatalog()) {
    Check(macroIds.insert(macro.id).second,"Duplicate macro ID.");
    macroCategories.insert(macro.category);
    const auto compiled=CompileVisualProgram(macro.program);
    Check(compiled.ok && compiled.executable,macro.id+": "+(compiled.errors.empty()?"No executable":compiled.errors.front()));
    Check(compiled.executable->nodes.size()==macro.program.nodes.size(),"Macro collapsed to native settings.");
    Check(macro.program.nodes.size()>2,"Macro is a cosmetic wrapper.");
    for (const auto &[id,node]:macro.program.nodes) {
      (void)id;
      Check(FindVisualBlock(node.definitionId),"Macro references an unavailable primitive.");
      Check(node.definitionId.rfind("macro",0)!=0,"Macro needs its own interpreter instruction.");
    }
  }
  for (const auto &category:{"Inputs","Conditions","Targets"})
    Check(macroCategories.count(category),std::string("No macroblocks for ")+category);
  for (const auto &id:{"existing-events","input-deletion","input-insertion","smooth-steering","random-steering","point-target","pose-target","box-target","prism-target","finish-target","stunt-target"})
    Check(macroIds.count(id),std::string("Missing source template: ")+id);
}
void StructureAndTypes() {
  Builder b;
  const auto number=b.number(), set=b.set(number), start=b.start({set});
  Check(CompileVisualProgram(b.program).ok,"Simple program rejected.");
  auto invalid=b.program;
  invalid.find(number)->fields["value"]="nan";
  Check(!CompileVisualProgram(invalid).ok,"Nonfinite literal accepted.");
  invalid=b.program; invalid.find(start)->fields["execution"]="native";
  Check(!CompileVisualProgram(invalid).ok,"Obsolete native lowering can still be invoked.");
  invalid=b.program; invalid.find(number)->x=std::numeric_limits<double>::infinity();
  Check(!ValidateVisualProgram(invalid).ok,"Nonfinite canvas coordinate accepted.");
  invalid=b.program; invalid.find(start)->statements["body"].push_back(set);
  Check(!ValidateVisualProgram(invalid).ok,"Node has multiple parents.");
  invalid=b.program; invalid.find(set)->inputs["value"]=999;
  Check(!ValidateVisualProgram(invalid).ok,"Missing input accepted.");
  invalid=b.program; invalid.find(set)->inputs["value"]=start;
  Check(!ValidateVisualProgram(invalid).ok,"Connection cycle accepted.");
  invalid=b.program; invalid.topLevel.push_back(start);
  Check(!ValidateVisualProgram(invalid).ok,"Duplicate root accepted.");
  Check(!ValidateVisualProgram(b.program,2).ok,"Node limit not enforced.");
  Check(!ValidateVisualProgram(b.program,4096,1).ok,"Nesting limit not enforced.");
  Check(VisualTypeCompatible(VisualValueType::Any,VisualValueType::Snapshot),"Variable cannot carry a snapshot.");
  Check(VisualTypeCompatible(VisualValueType::Scalar,VisualValueType::Milliseconds),"Computed times barred.");
  Check(!VisualTypeCompatible(VisualValueType::Text,VisualValueType::Boolean),"Text silently coerced to boolean.");
  invalid=b.program; invalid.find(set)->enabled=false; invalid.find(set)->inputs.clear(); invalid.nodes.erase(number);
  Check(CompileVisualProgram(invalid).ok,"Disabled incomplete command prevents execution.");
  invalid=b.program; invalid.find(number)->enabled=false;
  Check(!CompileVisualProgram(invalid).ok,"Disabled reporter supplies a value.");
}
void ProceduresAndControl() {
  Builder b;
  const auto parameter=b.add("data/get",{},{{"name","value"}});
  const auto returned=b.add("procedures/return",{{"value",parameter}});
  const auto definition=b.add("procedures/define",{},{{"name","identity"},{"parameters","value"}},{{"body",{returned}}});
  b.program.topLevel.push_back(definition);
  const auto call=b.add("procedures/value",{{"arg0",b.number()}},{{"name","identity"},{"parameters","value"}});
  b.start({b.set(call)});
  Check(CompileVisualProgram(b.program).ok,"User reporter procedure rejected.");
  auto invalid=b.program; invalid.find(call)->fields["parameters"]="different";
  Check(!CompileVisualProgram(invalid).ok,"Stale signature accepted.");
  invalid=b.program; invalid.find(call)->fields["name"]="missing";
  Check(!CompileVisualProgram(invalid).ok,"Unknown procedure accepted.");
  for (const auto &command:{"flow/break","flow/continue","procedures/return"}) {
    Builder c; c.start({c.add(command)});
    Check(!CompileVisualProgram(c.program).ok,std::string("Unscoped control transfer accepted: ")+command);
  }
  Builder local;
  local.start({local.add("data/local",{{"value",local.number()}},{{"name","temporary"}})});
  Check(CompileVisualProgram(local.program).ok,"Expanded locals cannot be used at entry level.");
}
}
int main() {
  try {
    CatalogAndMacros(); StructureAndTypes(); ProceduresAndControl();
    std::cout << "PASS visual language: primitive catalog, expanded macros, types, validation, procedures\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL visual language: " << error.what() << '\n'; return 1;
  }
}
