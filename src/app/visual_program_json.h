#ifndef FOREVERTAS_APP_VISUAL_PROGRAM_JSON_H
#define FOREVERTAS_APP_VISUAL_PROGRAM_JSON_H

#include "blocks/visual_program.h"

#include <QString>

#include <optional>

namespace forevertas::app {

struct VisualProgramJson {
  std::optional<blocks::VisualProgram> program;
  QString error;
};

// Versioned semantic persistence for the v3 block language. Blockly workspace
// JSON is deliberately separate UI state; this document is the native source
// of truth for nodes, typed connections and statement structure.
VisualProgramJson ParseVisualProgramJson(const QString &json);
QString PrintVisualProgramJson(const blocks::VisualProgram &program);

} // namespace forevertas::app

#endif
