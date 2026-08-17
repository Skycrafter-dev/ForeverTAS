#include "blocks/block_program.h"

#include "blocks/block_catalog.h"

#include <algorithm>
#include <set>

namespace forevertas::blocks {

std::map<std::string, std::string> DefaultFieldValues(
        const std::string &definitionId) {
    std::map<std::string, std::string> values;
    const BlockDefinition *const definition = FindBlock(definitionId);
    if (definition == nullptr) return values;
    for (const OptionField &field : definition->fields) {
        values.emplace(field.key, field.defaultValue);
    }
    return values;
}

BlockId BlockProgram::createBlock(
        const std::string &definitionId,
        const std::map<std::string, std::string> &fieldValues) {
    if (FindBlock(definitionId) == nullptr) return 0;
    BlockNode node;
    node.id = nextId_++;
    node.definitionId = definitionId;
    node.fields = DefaultFieldValues(definitionId);
    for (const auto &[key, value] : fieldValues) {
        node.fields.erase(key);
        node.fields.emplace(key, value);
    }
    const BlockId id = node.id;
    nodes_.emplace(id, std::move(node));
    topLevel_.push_back(id);
    return id;
}

BlockNode *BlockProgram::find(BlockId id) {
    const auto found = nodes_.find(id);
    return found == nodes_.end() ? nullptr : &found->second;
}

const BlockNode *BlockProgram::find(BlockId id) const {
    const auto found = nodes_.find(id);
    return found == nodes_.end() ? nullptr : &found->second;
}

void BlockProgram::detachFromParent(BlockId id) {
    topLevel_.erase(std::remove(topLevel_.begin(), topLevel_.end(), id),
                    topLevel_.end());
    for (auto &[parentId, node] : nodes_) {
        for (auto iterator = node.reporters.begin();
             iterator != node.reporters.end();) {
            if (iterator->second == id) {
                iterator = node.reporters.erase(iterator);
            } else {
                ++iterator;
            }
        }
        if (node.evaluator == id) node.evaluator = 0;
        node.substack.erase(
                std::remove(node.substack.begin(), node.substack.end(), id),
                node.substack.end());
    }
}

bool BlockProgram::isReachable(BlockId id) const {
    std::set<BlockId> visited;
    std::vector<BlockId> pending(topLevel_.begin(), topLevel_.end());
    while (!pending.empty()) {
        const BlockId current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second) continue;
        if (current == id) return true;
        const auto found = nodes_.find(current);
        if (found == nodes_.end()) continue;
        for (const BlockId child : found->second.substack) {
            pending.push_back(child);
        }
        for (const auto &[key, child] : found->second.reporters) {
            pending.push_back(child);
        }
        if (found->second.evaluator != 0) {
            pending.push_back(found->second.evaluator);
        }
    }
    return false;
}

void BlockProgram::collectGarbage() {
    std::set<BlockId> reachable;
    std::vector<BlockId> pending(topLevel_.begin(), topLevel_.end());
    if (script_) pending.push_back(*script_);
    while (!pending.empty()) {
        const BlockId current = pending.back();
        pending.pop_back();
        if (!reachable.insert(current).second) continue;
        const auto found = nodes_.find(current);
        if (found == nodes_.end()) continue;
        for (const BlockId child : found->second.substack) {
            pending.push_back(child);
        }
        for (const auto &[key, child] : found->second.reporters) {
            pending.push_back(child);
        }
        if (found->second.evaluator != 0) {
            pending.push_back(found->second.evaluator);
        }
    }
    for (auto iterator = nodes_.begin(); iterator != nodes_.end();) {
        if (reachable.count(iterator->first) == 0) {
            iterator = nodes_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    if (script_ && nodes_.find(*script_) == nodes_.end()) {
        script_.reset();
    }
}

bool BlockProgram::removeBlock(BlockId id) {
    if (nodes_.find(id) == nodes_.end()) return false;
    detachFromParent(id);
    nodes_.erase(id);
    collectGarbage();
    return true;
}

bool BlockProgram::setFieldValue(BlockId id,
                                 const std::string &key,
                                 const std::string &value) {
    BlockNode *const node = find(id);
    if (node == nullptr) return false;
    const auto existing = node->fields.find(key);
    if (existing != node->fields.end() && existing->second == value) {
        return false;
    }
    node->fields.erase(key);
    node->fields.emplace(key, value);
    return true;
}

BlockId BlockProgram::attachReporter(
        BlockId id,
        const std::string &key,
        const std::string &reporterDefinitionId,
        const std::map<std::string, std::string> &fieldValues) {
    BlockNode *const node = find(id);
    if (node == nullptr) return 0;
    const BlockDefinition *const definition =
            FindBlock(reporterDefinitionId);
    if (definition == nullptr ||
        definition->shape != BlockShape::Reporter ||
        definition->outputType != "number") {
        return 0;
    }
    const BlockId created = createBlock(reporterDefinitionId, fieldValues);
    if (created == 0) return 0;
    if (!graftReporter(id, key, created)) {
        removeBlock(created);
        return 0;
    }
    return created;
}

bool BlockProgram::graftReporter(BlockId id,
                                 const std::string &key,
                                 BlockId reporterId) {
    BlockNode *const node = find(id);
    BlockNode *const reporter = find(reporterId);
    if (node == nullptr || reporter == nullptr || node == reporter) {
        return false;
    }
    const BlockDefinition *const definition =
            FindBlock(reporter->definitionId);
    if (definition == nullptr ||
        definition->shape != BlockShape::Reporter ||
        definition->outputType != "number") {
        return false;
    }
    const BlockDefinition *const owner = FindBlock(node->definitionId);
    if (owner == nullptr) return false;
    bool numberField = false;
    for (const OptionField &field : owner->fields) {
        if (field.key == key && field.kind == OptionField::Kind::Number) {
            numberField = true;
        }
    }
    if (!numberField) return false;
    const auto oldReporter = node->reporters.find(key);
    const BlockId previous = oldReporter == node->reporters.end()
            ? 0
            : oldReporter->second;
    detachFromParent(reporterId);
    node->reporters.erase(key);
    node->reporters.emplace(key, reporterId);
    if (previous != 0 && previous != reporterId && !isReachable(previous)) {
        removeBlock(previous);
    }
    return true;
}

bool BlockProgram::detachReporter(BlockId id, const std::string &key) {
    BlockNode *const node = find(id);
    if (node == nullptr) return false;
    const auto found = node->reporters.find(key);
    if (found == node->reporters.end()) return false;
    const BlockId reporter = found->second;
    node->reporters.erase(found);
    removeBlock(reporter);
    return true;
}

bool BlockProgram::appendToSubstack(BlockId hatId, BlockId id) {
    BlockNode *const hat = find(hatId);
    BlockNode *const node = find(id);
    if (hat == nullptr || node == nullptr) return false;
    if (hat == node) return false;
    detachFromParent(id);
    hat->substack.push_back(id);
    return true;
}

bool BlockProgram::insertInSubstack(BlockId hatId,
                                    std::size_t index,
                                    BlockId id) {
    BlockNode *const hat = find(hatId);
    BlockNode *const node = find(id);
    if (hat == nullptr || node == nullptr || hat == node) return false;
    detachFromParent(id);
    if (index > hat->substack.size()) index = hat->substack.size();
    hat->substack.insert(hat->substack.begin() + static_cast<long>(index),
                         id);
    return true;
}

bool BlockProgram::moveWithinSubstack(BlockId id, std::size_t toIndex) {
    for (auto &[parentId, node] : nodes_) {
        const auto found = std::find(
                node.substack.begin(), node.substack.end(), id);
        if (found == node.substack.end()) continue;
        node.substack.erase(found);
        if (toIndex > node.substack.size()) {
            toIndex = node.substack.size();
        }
        node.substack.insert(
                node.substack.begin() + static_cast<long>(toIndex), id);
        return true;
    }
    return false;
}

bool BlockProgram::setEvaluator(BlockId hatId, BlockId evaluatorId) {
    BlockNode *const hat = find(hatId);
    BlockNode *const evaluator = find(evaluatorId);
    if (hat == nullptr || evaluator == nullptr || hat == evaluator) {
        return false;
    }
    const BlockId previous = hat->evaluator;
    detachFromParent(evaluatorId);
    hat->evaluator = evaluatorId;
    if (previous != 0 && previous != evaluatorId &&
        !isReachable(previous)) {
        removeBlock(previous);
    }
    return true;
}

bool BlockProgram::detachEvaluator(BlockId hatId) {
    BlockNode *const hat = find(hatId);
    if (hat == nullptr || hat->evaluator == 0) return false;
    const BlockId evaluator = hat->evaluator;
    hat->evaluator = 0;
    removeBlock(evaluator);
    return true;
}

bool BlockProgram::makeTopLevel(BlockId id) {
    BlockNode *const node = find(id);
    if (node == nullptr) return false;
    detachFromParent(id);
    topLevel_.push_back(id);
    return true;
}

void BlockProgram::setScript(BlockId hatId) {
    script_ = hatId;
    topLevel_.erase(std::remove(topLevel_.begin(), topLevel_.end(), hatId),
                    topLevel_.end());
}

void BlockProgram::adoptNode(BlockNode node) {
    if (node.id == 0) return;
    nextId_ = std::max(nextId_, node.id + 1);
    const BlockId id = node.id;
    nodes_.erase(id);
    nodes_.emplace(id, std::move(node));
}

void BlockProgram::clear() {
    nodes_.clear();
    topLevel_.clear();
    script_.reset();
    nextId_ = 1;
}

}  // namespace forevertas::blocks
