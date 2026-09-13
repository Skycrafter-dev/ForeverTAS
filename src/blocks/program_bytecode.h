#ifndef FOREVERTAS_BLOCKS_PROGRAM_BYTECODE_H
#define FOREVERTAS_BLOCKS_PROGRAM_BYTECODE_H

#include "blocks/program_vm.h"
#include "blocks/visual_program.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace forevertas::blocks {

struct ProgramBytecode {
  std::vector<vm::Instruction> code;
  std::vector<vm::Procedure> procedures;
  std::vector<std::uint32_t> arguments;
  std::vector<std::string> symbols, strings, procedureNames;
  std::array<std::uint32_t,10> actionNames{};
  std::uint32_t entry = 0;
  bool usesPhysics = false;
  // A symbol-sized compiler proof; absent metadata requires importing all globals.
  std::vector<bool> initialGlobalReads;
  bool usesDynamicCalls = false;
  std::vector<std::vector<std::uint32_t>> procedureInitialGlobalReads;
  vm::Program view() const;
};

// A rejected optimization is not a rejected visual program. The caller runs
// the original interpreter on the requested physics backend in that case.
struct BytecodeCompilation {
  std::optional<ProgramBytecode> program;
  std::string reason;
};
BytecodeCompilation CompileMappedProgram(const VisualProgram &source, const std::string &procedure);
// Reachability before returning from this frame; active caller continuations
// must be checked separately. Indirect calls are deliberately conservative.
std::vector<std::uint8_t> HistoryReadReachability(const ProgramBytecode &program);
std::vector<std::uint8_t> RestoreReachability(const ProgramBytecode &program);
std::string BytecodeError(vm::Error error);

} // namespace forevertas::blocks
#endif
