#include "blocks/visual_compiler.h"
#include "blocks/visual_runtime.h"

namespace forevertas::blocks {
VisualCompileResult CompileVisualProgram(const VisualProgram &program) {
  const auto validation = ValidateExecutableVisualProgram(program);
  VisualCompileResult result{validation.ok, {}, validation.errors};
  if (result.ok) result.executable = std::make_shared<const VisualProgram>(program);
  return result;
}
} // namespace forevertas::blocks
