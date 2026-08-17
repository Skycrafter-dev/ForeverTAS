#ifndef FOREVERTAS_BLOCKS_BLOCK_PROGRAM_IO_H
#define FOREVERTAS_BLOCKS_BLOCK_PROGRAM_IO_H

#include "blocks/block_program.h"

#include <optional>
#include <string>

namespace forevertas::blocks {

// Full-fidelity JSON persistence, including block positions and loose
// blocks. Returns nullopt with `error` set when the document is invalid.
struct BlockProgramJson {
    std::optional<BlockProgram> program;
    std::string error;
    // Field values remembered per option kind, used when a hat or
    // evaluator block is replaced, mirroring per-option persistence.
    std::map<std::string, std::map<std::string, std::string>> remembered;
};

BlockProgramJson ParseBlockProgramJson(const std::string &json);

std::string PrintBlockProgramJson(
        const BlockProgram &program,
        const std::map<std::string, std::map<std::string, std::string>>
                &remembered = {});

// Human-editable text interchange describing the semantic program: the
// script's search block, its evaluation slot, and its mutation substack
// with every field value. Number slots may carry expressions built from
// + - * / min max and number literals; parsing rebuilds the matching
// reporter blocks.
struct BlockProgramText {
    std::optional<BlockProgram> program;
    std::string error;
};

BlockProgramText ParseBlockProgramText(const std::string &text);

std::string PrintBlockProgramText(const BlockProgram &program);

}  // namespace forevertas::blocks

#endif
