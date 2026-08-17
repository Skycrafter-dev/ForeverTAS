#ifndef FOREVERTAS_BLOCKS_BLOCK_PROGRAM_H
#define FOREVERTAS_BLOCKS_BLOCK_PROGRAM_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace forevertas::blocks {

using BlockId = std::uint64_t;  // nonzero; zero means "no block"

// One placed block. Fields hold literal settings values keyed by the
// definition's field key; number fields may instead reference a reporter
// block through `reporters`. Hat blocks own one evaluation slot and an
// ordered mutation substack.
struct BlockNode {
    BlockId id = 0;
    std::string definitionId;
    std::map<std::string, std::string> fields;
    std::map<std::string, BlockId> reporters;
    std::vector<BlockId> substack;
    BlockId evaluator = 0;
    double x = 0.0;
    double y = 0.0;
};

// A placed-block program. Blocks live in a flat id-keyed pool; the
// structure is a forest of top-level blocks (hats and loose blocks).
class BlockProgram {
public:
    // Creates a loose top-level block with default field values.
    BlockId createBlock(const std::string &definitionId,
                        const std::map<std::string, std::string>
                                &fieldValues = {});

    BlockNode *find(BlockId id);
    const BlockNode *find(BlockId id) const;

    // Detaches the block from its parent (if any) and removes it together
    // with every block that becomes unreachable.
    bool removeBlock(BlockId id);

    bool setFieldValue(BlockId id,
                       const std::string &key,
                       const std::string &value);

    // Replaces the reporter in a value slot with a newly created reporter
    // of `reporterDefinitionId`; returns the new reporter id.
    BlockId attachReporter(BlockId id,
                           const std::string &key,
                           const std::string &reporterDefinitionId,
                           const std::map<std::string, std::string>
                                   &fieldValues = {});
    // Moves an existing block into a value slot, replacing its reporter.
    bool graftReporter(BlockId id, const std::string &key, BlockId reporterId);
    bool detachReporter(BlockId id, const std::string &key);

    // Stack editing. The workspace keeps one script; the core stays
    // general so future blocks can nest freely.
    bool appendToSubstack(BlockId hatId, BlockId id);
    bool insertInSubstack(BlockId hatId, std::size_t index, BlockId id);
    bool moveWithinSubstack(BlockId id, std::size_t toIndex);
    bool setEvaluator(BlockId hatId, BlockId evaluatorId);
    bool detachEvaluator(BlockId hatId);
    bool makeTopLevel(BlockId id);

    void setScript(BlockId hatId);
    std::optional<BlockId> script() const { return script_; }

    const std::map<BlockId, BlockNode> &nodes() const { return nodes_; }
    const std::vector<BlockId> &topLevel() const { return topLevel_; }

    void clear();

    // Adopts a fully formed node (used by persistence loading); keeps ids
    // unique by advancing the id counter past the adopted id.
    void adoptNode(BlockNode node);

    // Removes every node that cannot be reached from the top level.
    void collectGarbage();

private:
    BlockId nextId_ = 1;
    std::optional<BlockId> script_;
    std::map<BlockId, BlockNode> nodes_;
    std::vector<BlockId> topLevel_;

    void detachFromParent(BlockId id);
    bool isReachable(BlockId id) const;
};

// Default field values for a definition, taken from its field schema.
std::map<std::string, std::string> DefaultFieldValues(
        const std::string &definitionId);

}  // namespace forevertas::blocks

#endif
